/*
 * Wi-Fi P1 spike: boot the modem DSP (MPSS, it runs the WCN3990 WLAN firmware) the way
 * Linux qcom_mdt_load() + qcom_q6v5_pas do: TrustZone PAS init_image with the .mdt
 * metadata, copy the .bNN segments into the modem carveout, PAS auth_and_reset.
 * Windows copy of uefi/TopazOtgDxe/ModemPas.c: firmware from C:\topaz\fw\image\modem.*,
 * TZ-visible buffers from PhysAlloc(), the carveout mapped only while segments are copied.
 */
#include "Modem.h"

#define Out ModemOut

#pragma pack(1)
typedef struct {
  UINT8  e_ident[16];
  UINT16 e_type, e_machine;
  UINT32 e_version, e_entry, e_phoff, e_shoff, e_flags;
  UINT16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;
typedef struct {
  UINT32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;
#pragma pack()
#define PT_LOAD           1

#define PAS_ID_MODEM      4
#define SCM_FN(svc, cmd)  (0x42000000u | ((UINT32)(svc) << 8) | (UINT32)(cmd))
#define SCM_SVC_PIL       0x02
#define PIL_INIT_IMAGE    0x01
#define PIL_MEM_SETUP     0x02
#define PIL_AUTH_RESET    0x05
#define PIL_IS_SUPPORTED  0x07
#define PIL_SHUTDOWN      0x06
#define SCM_ARG_RW        2
#define SCM_ARGS(n)       (n)

#define MDT_TYPE_MASK     (7u << 24)
#define MDT_TYPE_HASH     (2u << 24)
#define MDT_RELOCATABLE   (1u << 27)

STATIC UINTN mScmResumes;

STATIC UINTN Scm(UINT32 Fn, UINTN ArgInfo, UINTN A, UINTN B, UINTN C, UINTN *Res1)
{
  ARM_SMC_ARGS args;
  UINTN n;

  ZeroMem (&args, sizeof (args));
  args.Arg0 = Fn;
  args.Arg1 = ArgInfo;
  args.Arg2 = A;
  args.Arg3 = B;
  args.Arg4 = C;
  ArmCallSmc (&args);
  /*
   * Long TZ calls return QCOM_SCM_INTERRUPTED (1); resume with x0 = 1 and the returned x6
   * (session) until done, as Linux __scm_smc_do_quirk() does. x1..x5 keep the original args.
   */
  for (n = 0; args.Arg0 == 1 && n < 100000; n++) {
    UINTN a6 = args.Arg6;
    ZeroMem (&args, sizeof (args));
    args.Arg0 = 1;
    args.Arg1 = ArgInfo;
    args.Arg2 = A;
    args.Arg3 = B;
    args.Arg4 = C;
    args.Arg6 = a6;
    ArmCallSmc (&args);
  }
  mScmResumes = n;
  if (Res1 != NULL) {
    *Res1 = args.Arg1;
  }
  return args.Arg0;
}

/* Firmware lives in C:\topaz\fw (image\ = NON-HLOS FAT of modem_a, efs\ = EFS backups). */
STATIC EFI_FILE_PROTOCOL *OpenFwDir(VOID)
{
  EFI_FILE_PROTOCOL *root = FwRoot (), *f;

  if (EFI_ERROR (root->Open (root, &f, L"\\image\\modem.mdt", EFI_FILE_MODE_READ, 0))) {
    Out ("  C:\\topaz\\fw\\image\\modem.mdt not found\r\n");
    return NULL;
  }
  f->Close (f);
  return root;
}

/* Read a whole file into *Buf (pool) or into Dest when Dest != NULL. */
STATIC EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name, VOID **Buf, UINTN *Size, VOID *Dest, UINTN DestMax)
{
  EFI_FILE_PROTOCOL *f;
  EFI_FILE_INFO *info;
  UINTN isz = SIZE_OF_EFI_FILE_INFO + 256, sz;
  EFI_STATUS s;
  VOID *p;

  s = Root->Open (Root, &f, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (s)) {
    return s;
  }
  info = AllocatePool (isz);
  s = f->GetInfo (f, &gEfiFileInfoGuid, &isz, info);
  sz = EFI_ERROR (s) ? 0 : (UINTN)info->FileSize;
  FreePool (info);
  if (EFI_ERROR (s)) {
    f->Close (f);
    return s;
  }
  if (Dest != NULL) {
    if (sz > DestMax) {
      f->Close (f);
      return EFI_BUFFER_TOO_SMALL;
    }
    p = Dest;
  } else {
    p = AllocatePool (sz);
  }
  s = f->Read (f, &sz, p);
  f->Close (f);
  if (Buf != NULL) {
    *Buf = p;
  }
  *Size = sz;
  return s;
}


/*
 * Linux qcom_scm_assign_mem() (SCM_SVC_MP 0x0C, ASSIGN 0x16): hand a buffer from HLOS to
 * HLOS + modem (MSS_MSA 15) + NAV (0x2B) RW, as the downstream sharedmem-uio driver does
 * for the rmtfs buffer (qcom,vm-nav-path). 7 args: x2..x4 = args 0..2, x5 = phys of args 3..
 */
#define VMID_HLOS     3
#define VMID_MSS_MSA  15
#define VMID_NAV      0x2B
#define PERM_RW       6

UINTN ScmAssign(UINT64 Addr, UINT64 Size, CONST UINT32 *Vmids, UINT32 Count)
{
  UINT64 pa = 0;
  UINT64 *p;
  UINT32 *src;
  ARM_SMC_ARGS a;
  UINTN n;

  p = PhysAlloc (SIZE_4KB, &pa);                 /* zeroed */
  if (p == NULL) {
    return (UINTN)-1;
  }
  /* 0x000: mem map {addr, size} */
  p[0] = Addr;
  p[1] = Size;
  /* 0x040: src vmids (le32) */
  src = (UINT32 *)(p + 8);
  src[0] = VMID_HLOS;
  /* 0x080: dest perms {u32 vmid, u32 perm, u64 ctx, u32 ctx_size, u32 unused} x3 */
  {
    UINT32 *d = (UINT32 *)(p + 16), i;
    for (i = 0; i < Count && i < 8; i++) {
      d[6 * i]     = Vmids[i];
      d[6 * i + 1] = PERM_RW;
    }
  }
  /* 0x100: extended args 3..6 */
  p[32] = 4;                     /* src_sz */
  p[33] = pa + 0x80;             /* dest */
  p[34] = Count * 24;            /* dest_sz */
  p[35] = 0;
  WriteBackInvalidateDataCacheRange (p, SIZE_4KB);

  ZeroMem (&a, sizeof (a));
  a.Arg0 = SCM_FN (0x0C, 0x16);
  a.Arg1 = 7 | (1 << 4) | (1 << 8) | (1 << 12);   /* RO VAL RO VAL RO VAL VAL */
  a.Arg2 = pa;                   /* mem map */
  a.Arg3 = 16;                   /* mem map size */
  a.Arg4 = pa + 0x40;            /* src vmids */
  a.Arg5 = pa + 0x100;           /* args 3..6 */
  ArmCallSmc (&a);
  for (n = 0; a.Arg0 == 1 && n < 100000; n++) {
    UINTN a6 = a.Arg6;
    ZeroMem (&a, sizeof (a));
    a.Arg0 = 1;
    a.Arg1 = 7 | (1 << 4) | (1 << 8) | (1 << 12);
    a.Arg2 = pa;
    a.Arg3 = 16;
    a.Arg4 = pa + 0x40;
    a.Arg5 = pa + 0x100;
    a.Arg6 = a6;
    ArmCallSmc (&a);
  }
  Out ("  SCM assign %lx+%lx -> %u vmids: ret=%lx res=%lx\r\n", Addr, Size, Count, (UINT64)a.Arg0, (UINT64)a.Arg1);
  PhysFree (p);
  return (a.Arg0 != 0) ? a.Arg0 : a.Arg1;
}

UINTN ScmAssignToModem(UINT64 Addr, UINT64 Size)
{
  STATIC CONST UINT32 v[] = { VMID_HLOS, VMID_MSS_MSA, VMID_NAV };
  return ScmAssign (Addr, Size, v, ARRAY_SIZE (v));
}

STATIC BOOLEAN PhdrLoadable(CONST Elf32_Phdr *P)
{
  return P->p_type == PT_LOAD && P->p_memsz != 0 && (P->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH;
}


/*
 * After auth_and_reset: is the modem really running? Read SMEM (0x46000000, 2 MiB, mapped
 * uncached by the UEFI memory map) the way Linux smem.c does, find the modem's SMP2P item
 * (modem -> apps, item 435 in the host 0/1 partition; apps -> modem would be 428) and watch
 * its "slave-kernel" entry: bit0 fatal, bit1 ready, bit2 handover, bit3 stop-ack,
 * bit7 shutdown-ack (DTB remoteproc-mss interrupts-extended). On fatal, print the crash
 * reason string (SMEM item 421, global). Also sample the modem watchdog SPI 0x133 pending bit.
 */
#pragma pack(1)
typedef struct { UINT32 Magic; UINT8 Version, Features[3]; UINT16 LocalPid, RemotePid, Total, Valid; UINT32 Flags; } SMP2P_HDR;
typedef struct { CHAR8 Name[16]; UINT32 Value; } SMP2P_ENTRY;
#pragma pack()
#define SMEM_ITEM_SMP2P_IN 435
#define SMEM_ITEM_SMP2P_OUT 428
#define SMEM_ITEM_MSS_CRASH 421
#define MPSS_SMP2P_IRQ_BIT 14                 /* smp2p-modem mboxes = <&apcs_glb 14> */
#define SMP2P_MAGIC        0x504D5324u        /* "$SMP" */
#define SMP2P_MAX_ENTRY    16
#define SMP2P_FEATURES     1                  /* SSR_ACK */
#define MSS_WDOG_INTID     (32 + 0x133)

STATIC VOID Smp2pKick(VOID)
{
  ApcsKick (MPSS_SMP2P_IRQ_BIT);
}

/* qcom_smp2p.c qcom_smp2p_alloc_outbound_item(): our (apps -> modem) item, no entries yet */
STATIC SMP2P_HDR *Smp2pCreateOut(SMEM_PART_HDR *P)
{
  SMP2P_HDR *o = SmemPrivAlloc (P, SMEM_ITEM_SMP2P_OUT, sizeof (SMP2P_HDR) + SMP2P_MAX_ENTRY * sizeof (SMP2P_ENTRY));

  if (o == NULL || o->Version != 0) {
    return o;
  }
  o->Magic       = SMP2P_MAGIC;
  o->LocalPid    = 0;
  o->RemotePid   = 1;
  o->Total       = SMP2P_MAX_ENTRY;
  o->Valid       = 0;
  o->Features[0] = SMP2P_FEATURES;
  MemoryFence ();
  o->Version     = 1;                 /* validates the item */
  MemoryFence ();
  Smp2pKick ();
  return o;
}

STATIC VOID Smp2pDump(CONST CHAR8 *Tag, SMP2P_HDR *H)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(H + 1);
  UINT32 i;

  Out ("  %a: magic %08x v%u feat %02x pid %u->%u entries %u/%u\r\n", Tag, H->Magic, H->Version,
       H->Features[0], H->LocalPid, H->RemotePid, H->Valid, H->Total);
  for (i = 0; i < H->Valid && i < 16; i++) {
    CHAR8 n[17];
    CopyMem (n, e[i].Name, 16);
    n[16] = 0;
    Out ("    [%u] %-16a = %08x\r\n", i, n, e[i].Value);
  }
}

STATIC UINT32 *Smp2pFind(SMP2P_HDR *H, CONST CHAR8 *Name)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(H + 1);
  UINT32 i;

  for (i = 0; i < H->Valid && i < 16; i++) {
    if (AsciiStrnCmp (e[i].Name, Name, 16) == 0) {
      return &e[i].Value;
    }
  }
  return NULL;
}

/*
 * IPA power query (Linux drivers/net/ipa/ipa_smp2p.c, downstream ipa3 smp2p): our outbound "ipa"
 * entry has bit 0 = "enabled" answer valid, bit 1 = AP IPA power on; the modem's inbound "ipa"
 * entry raises bit 0 = power query, bit 1 = GSI setup ready. "The modem will poll the valid bit
 * until it is set" - with nobody answering it spins once the SIM brings up data services
 * (suspected cause of "Task starvation: modem_cfg", TopazWifi v0.7..v0.9). IPA is not set up on
 * the AP yet, so the answer is "valid, power off".
 */
#define IPA_OUT_VALID    (1u << 0)
#define IPA_OUT_ENABLED  (1u << 1)
#define IPA_IN_QUERY     (1u << 0)
#define IPA_IN_SETUP     (1u << 1)

STATIC SMP2P_HDR *mSmpIn, *mSmpOut;
STATIC UINT32    *mIpaIn, *mIpaOut, mIpaLast = 0xFFFFFFFF;
STATIC UINTN      mIpaAnswers;

STATIC UINT32 *Smp2pAddOut(SMP2P_HDR *O, CONST CHAR8 *Name, UINT32 Value)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(O + 1);
  UINT32 *v = Smp2pFind (O, Name);

  if (v != NULL || O->Valid >= O->Total || O->Valid >= SMP2P_MAX_ENTRY) {
    return v;
  }
  ZeroMem (e[O->Valid].Name, sizeof (e[O->Valid].Name));
  CopyMem (e[O->Valid].Name, Name, AsciiStrLen (Name));
  e[O->Valid].Value = Value;
  MemoryFence ();
  O->Valid++;
  MemoryFence ();
  Smp2pKick ();
  return &e[O->Valid - 1].Value;
}

VOID Smp2pIpaPoll(VOID)
{
  UINT32 v;

  if (mSmpIn == NULL || mIpaOut == NULL) {
    return;
  }
  if (mIpaIn == NULL) {
    mIpaIn = Smp2pFind (mSmpIn, "ipa");
    if (mIpaIn == NULL) {
      return;
    }
  }
  v = *mIpaIn;
  if (v == mIpaLast) {
    return;
  }
  Out ("  t=%u.%03u smp2p: modem ipa entry %08x%a%a\r\n", (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000),
       v, (v & IPA_IN_QUERY) ? " POWER-QUERY" : "", (v & IPA_IN_SETUP) ? " GSI-SETUP-READY" : "");
  if ((v & IPA_IN_QUERY) && !(mIpaLast != 0xFFFFFFFF && (mIpaLast & IPA_IN_QUERY))) {
    *mIpaOut = IPA_OUT_VALID;                   /* power off, answer valid */
    MemoryFence ();
    Smp2pKick ();
    mIpaAnswers++;
    Out ("  smp2p: answered IPA power query: valid, AP IPA power off (%u)\r\n", (UINT32)mIpaAnswers);
  }
  mIpaLast = v;
}

STATIC VOID ModemWatch(UINTN Seconds, EFI_FILE_PROTOCOL *Root)
{
  SMEM_PART_HDR *part;
  SMP2P_HDR *in = NULL, *out;
  UINT32 *sk = NULL, last = 0xFFFFFFFF, sz = 0, wdog, lastWdog = 2, crashLen = 0;
  UINTN t;
  CHAR8 *crash;
  BOOLEAN negotiated = FALSE;

  Out ("  SMEM version %u, ptable %a\r\n", SmemVersion (),
       CompareMem ((VOID *)(UINTN)(SMEM_BASE + SMEM_SIZE - SIZE_4KB), "$TOC", 4) == 0 ? "ok" : "MISSING");
  part = SmemPartition (0, 1);
  Out ("  apps<->modem partition: %p\r\n", part);
  if (part == NULL) {
    SMEM_PT *pt = (SMEM_PT *)(UINTN)(SMEM_BASE + SMEM_SIZE - SIZE_4KB);
    SMEM_PT_ENTRY *e = (SMEM_PT_ENTRY *)(pt + 1);
    UINT32 i;
    for (i = 0; i < pt->NumEntries && i < 24; i++) {
      Out ("   pt[%u] %x/%x off %x size %x\r\n", i, e[i].Host0, e[i].Host1, e[i].Offset, e[i].Size);
    }
  }
  out = (SMP2P_HDR *)SmemPrivGet (part, SMEM_ITEM_SMP2P_OUT, NULL);
  if (out == NULL) {
    out = Smp2pCreateOut (part);
    Out ("  created apps->modem item 428: %p\r\n", out);
  }
  if (out != NULL) {
    mSmpOut = out;
    mIpaOut = Smp2pAddOut (out, "ipa", 0);      /* before the modem boots, like Linux at probe */
    Smp2pDump ("apps->modem (428)", out);
  }

  for (t = 0; t <= Seconds * 4; t++) {
    wdog = (MmioRead32 (gGicdVa + 0x200 + (MSS_WDOG_INTID / 32) * 4) >> (MSS_WDOG_INTID % 32)) & 1;
    if (wdog != lastWdog) {
      Out ("  t=%lu.%02us modem wdog SPI pending=%u\r\n", (UINT64)(t / 4), (UINT32)(t % 4) * 25, wdog);
      lastWdog = wdog;
    }
    if (in == NULL) {
      in = (SMP2P_HDR *)SmemPrivGet (part, SMEM_ITEM_SMP2P_IN, &sz);
      if (in != NULL) {
        Out ("  t=%lu.%02us modem SMP2P item appeared (%u bytes)\r\n", (UINT64)(t / 4), (UINT32)(t % 4) * 25, sz);
      }
    }
    if (in != NULL && out != NULL && !negotiated && in->Version == out->Version) {
      out->Features[0] &= in->Features[0];
      negotiated = TRUE;
      Smp2pKick ();
      Out ("  t=%lu.%02us SMP2P negotiated, features %02x\r\n", (UINT64)(t / 4), (UINT32)(t % 4) * 25, out->Features[0]);
    }
    if (in != NULL && sk == NULL) {
      sk = Smp2pFind (in, "slave-kernel");
    }
    if (sk != NULL && *sk != last) {
      last = *sk;
      Out ("  t=%lu.%02us slave-kernel=%08x%a%a%a%a\r\n", (UINT64)(t / 4), (UINT32)(t % 4) * 25, last,
           (last & 1) ? " FATAL" : "", (last & 2) ? " READY" : "", (last & 4) ? " handover" : "",
           (last & 0x88) ? " stop/shutdown-ack" : "");
      if (last & 3) {                           /* fatal, or ready: go on to GLINK */
        break;
      }
    }
    gBS->Stall (250 * 1000);
  }
  if (in == NULL) {
    Out ("  modem SMP2P item 435 never appeared\r\n");
  } else if ((last & 3) == 2) {
    gModemState = sk;
    mSmpIn = in;
    Smp2pDump ("modem->apps (435)", in);
    GlinkQrtrSpike (0, Root);                  /* until driver stop or FATAL */
    Out ("  after P2: slave-kernel=%08x%a, wdog pending=%u\r\n", *sk, (*sk & 1) ? " FATAL" : "",
         (MmioRead32 (gGicdVa + 0x200 + (MSS_WDOG_INTID / 32) * 4) >> (MSS_WDOG_INTID % 32)) & 1);
  } else {
    Smp2pDump ("modem->apps (435)", in);
  }
  crash = (CHAR8 *)SmemGlobalGet (SMEM_ITEM_MSS_CRASH, &crashLen);
  if (crash != NULL && crashLen != 0 && crash[0] != 0) {
    CHAR8 msg[256];
    AsciiStrnCpyS (msg, sizeof (msg), crash, MIN (crashLen, sizeof (msg) - 1));
    Out ("  crash reason (421): %a\r\n", msg);
  } else {
    Out ("  crash reason (421): %a\r\n", crash == NULL ? "item absent" : "empty");
  }
}

/*
 * GSI firmware for IPA (SIM / mobile data stage 2, docs/P9_sim.md). Like Linux ipa_main.c
 * ipa_firmware_load(): qcom_mdt_load(ipa_fws, PAS id 15) into the DT memory region
 * ipa_fw_region 0x55B00000 (64 KiB, inside our "PIL Reserved" 0x4AB00000..0x56300000), segments
 * relocated (p_paddr - min) into it, then auth_and_reset. Android does this at 10.7 s, before the
 * modem (19.4 s). Needs the IPA clock: only after TopazRpm published its acked vote for this boot
 * (volatile key Services\TopazRpm\State IpaClockKhz). Opt-in: C:\topaz\fw\ipa.on.
 */
#define PAS_ID_IPA        15
#define IPA_FW_PA         0x55B00000u
#define IPA_FW_SIZE       0x00010000u
#define GSI_STATUS_PA     (0x05804000u + 0x1F000u)   /* GSI_STATUS, EE 0: bit 0 ENABLED */

BOOLEAN gIpaFwRunning;

/* Linux ipa_mem_config(): 0xdeadbeef canaries just below the SRAM regions that have them
   (data/ipa_data-v4.2.c canary_count 2 / END_MARKER 1). SRAM = ipa-shared 0x5847000. */
#define IPA_SRAM_PA       0x05847000u
STATIC VOID IpaCanaries(VOID)
{
  STATIC CONST UINT16 two[] = { 0x288, 0x290, 0x310, 0x318, 0x398, 0x3A0, 0x420, 0x428, 0x4A8, 0x5F0, 0x9F8, 0xA50 };
  UINT32 *sram = MapPhys (IPA_SRAM_PA, 0x2000, FALSE);
  UINTN i;

  if (sram == NULL) {
    return;
  }
  for (i = 0; i < ARRAY_SIZE (two); i++) {
    sram[two[i] / 4 - 1] = 0xDEADBEEF;
    sram[two[i] / 4 - 2] = 0xDEADBEEF;
  }
  sram[0x2000 / 4 - 1] = 0xDEADBEEF;
  MemoryFence ();
  Out ("  ipa: SRAM canaries written (%u regions + end marker), word@0x290-4 = %08x\r\n",
       (UINT32)ARRAY_SIZE (two), sram[0x290 / 4 - 1]);
  LogHardFlush ();
  UnmapPhys (sram, 0x2000);
}

/*
 * Linux gsi_setup() for IPA v4.2: zero ERROR_LOG, then gsi_channel_setup() allocates the modem's
 * channels on its behalf ("hardware quirk on IPA v4.2"): GENERIC_CMD ALLOCATE_CHANNEL with EE 1
 * for every modem endpoint channel of data/ipa_data-v4.2.c (0..3). Without it the modem asserts
 * after INIT_DRIVER: "ipa_hal.c: Failed to initialize GSI channel: CHID 1" (TopazWifi v0.14).
 * Completion = global IRQ GP_INT1 (polled here), result = SCRATCH_0 GENERIC_EE_RESULT (bits 7:5).
 * Registers: reg/gsi_reg-v4.0.c, EE 0 page at gsi + 0x1f000 = 0x5823000.
 */
#define GSI_EE0_PA            0x05823000u
#define GSI_GENERIC_CMD       0x018
#define GSI_GLOB_IRQ_STTS     0x100
#define GSI_GLOB_IRQ_EN       0x108
#define GSI_GLOB_IRQ_CLR      0x110
#define GSI_ERROR_LOG         0x200
#define GSI_SCRATCH_0         0x400
#define GSI_ERROR_INT         (1u << 0)
#define GSI_GP_INT1           (1u << 1)
#define GSI_ALLOCATE_CHANNEL  2
#define GSI_EE_MODEM          1

/*
 * v0.15: the TopazWifi modem thread (pinned to core 0) never came back from this function - no
 * line after the canaries, Wi-Fi dead, the next GPU boot hung. v0.16: one-shot flag
 * C:\topaz\fw\gsi.alloc (deleted before the first access) and a log line before every
 * register access, to find the access that stalls.
 */
STATIC BOOLEAN FlagTake(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name)
{
  EFI_FILE_PROTOCOL *f = NULL;
  WCHAR path[64];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  IO_STATUS_BLOCK iosb;
  HANDLE h;

  if (EFI_ERROR (Root->Open (Root, &f, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0))) {
    return FALSE;
  }
  f->Close (f);
  RtlStringCbPrintfW (path, sizeof (path), L"\\??\\C:\\topaz\\fw%s", Name);
  RtlInitUnicodeString (&us, path);
  InitializeObjectAttributes (&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
  if (NT_SUCCESS (ZwCreateFile (&h, DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0))) {
    ZwClose (h);
  }
  return TRUE;
}

#define GSTEP(...)  do { Out ("  ipa: gsi step " __VA_ARGS__); LogHardFlush (); KeStallExecutionProcessor (50000); } while (0)

STATIC VOID GsiAllocModemChannels(EFI_FILE_PROTOCOL *Root)
{
  volatile UINT32 *g;
  UINT32 ch, t, st = 0, res, v;

  if (!FlagTake (Root, L"\\gsi.alloc")) {
    Out ("  ipa: C:\\topaz\\fw\\gsi.alloc absent: modem GSI channels not allocated\r\n");
    return;
  }
  GSTEP ("map %08x\r\n", GSI_EE0_PA);
  g = MapPhys (GSI_EE0_PA, SIZE_4KB, FALSE);
  if (g == NULL) {
    return;
  }
  GSTEP ("read STATUS %08x\r\n", g[0]);
  GSTEP ("read ERROR_LOG %08x\r\n", g[GSI_ERROR_LOG / 4]);
  GSTEP ("read GLOB_IRQ_EN %08x STTS %08x SCRATCH_0 %08x\r\n", g[GSI_GLOB_IRQ_EN / 4], g[GSI_GLOB_IRQ_STTS / 4],
         g[GSI_SCRATCH_0 / 4]);
  /*
   * v0.20: Linux gsi_irq_setup() before any command - CNTXT_INTSET = 1 (IRQ, not MSI: in MSI mode
   * the completion is a memory write to an unprogrammed address), every interrupt mask 0.
   * v0.17..v0.19 skipped it and the SoC reset on the first GENERIC_CMD (v0.19 with grey screen
   * garbage = a stray write).
   */
  GSTEP ("read INTSET %08x TYPE_IRQ_MSK %08x\r\n", g[0x180 / 4], g[0x088 / 4]);
  g[0x180 / 4] = 1;                             /* CNTXT_INTSET: IRQ */
  g[0x088 / 4] = 0;                             /* CNTXT_TYPE_IRQ_MSK */
  g[0x098 / 4] = 0;                             /* CNTXT_SRC_CH_IRQ_MSK */
  g[0x09C / 4] = 0;                             /* CNTXT_SRC_EV_CH_IRQ_MSK */
  g[GSI_GLOB_IRQ_EN / 4] = 0;
  g[0x0B8 / 4] = 0;                             /* CNTXT_SRC_IEOB_IRQ_MSK */
  g[0x120 / 4] = 0;                             /* CNTXT_GSI_IRQ_EN */
  {
    volatile UINT32 *ie = MapPhys (0x05804000u + 0xC000u, SIZE_4KB, FALSE);   /* inter-EE, EE 0 */
    if (ie != NULL) {
      ie[0x20 / 4] = 0;                         /* INTER_EE_SRC_CH_IRQ_MSK */
      ie[0x24 / 4] = 0;                         /* INTER_EE_SRC_EV_CH_IRQ_MSK */
      UnmapPhys ((VOID *)ie, SIZE_4KB);
    }
  }
  MemoryFence ();
  GSTEP ("irq setup done: INTSET %08x TYPE_IRQ_MSK %08x\r\n", g[0x180 / 4], g[0x088 / 4]);
  g[0x088 / 4] = 1u << 2;                       /* TYPE_IRQ_MSK: GLOB_EE only (gsi_irq_enable) */
  GSTEP ("write ERROR_LOG 0\r\n");
  g[GSI_ERROR_LOG / 4] = 0;
  for (ch = 0; ch < 4; ch++) {
    GSTEP ("ch %u: clear + enable GP_INT1\r\n", ch);
    g[GSI_GLOB_IRQ_CLR / 4] = GSI_GP_INT1;
    g[GSI_GLOB_IRQ_EN / 4]  = GSI_ERROR_INT | GSI_GP_INT1;
    GSTEP ("ch %u: scratch_0 rmw\r\n", ch);
    v = g[GSI_SCRATCH_0 / 4];
    g[GSI_SCRATCH_0 / 4] = v & ~(7u << 5);
    MemoryFence ();
    GSTEP ("ch %u: GENERIC_CMD %08x\r\n", ch, GSI_ALLOCATE_CHANNEL | (ch << 5) | (GSI_EE_MODEM << 10));
    g[GSI_GENERIC_CMD / 4] = GSI_ALLOCATE_CHANNEL | (ch << 5) | (GSI_EE_MODEM << 10);
    for (t = 0; t < 500 && ((st = g[GSI_GLOB_IRQ_STTS / 4]) & GSI_GP_INT1) == 0; t++) {
      KeStallExecutionProcessor (100);
    }
    res = (g[GSI_SCRATCH_0 / 4] >> 5) & 7;
    g[GSI_GLOB_IRQ_CLR / 4] = GSI_GP_INT1;
    g[GSI_GLOB_IRQ_EN / 4]  = GSI_ERROR_INT;
    Out ("  ipa: GSI allocate modem channel %u: %a after %u x 100 us, result %u%a (glob stts %08x, error log %08x)\r\n", ch,
         (st & GSI_GP_INT1) ? "done" : "TIMEOUT", t, res,
         res == 1 ? " SUCCESS" : res == 2 ? " (already: incorrect state)" : res == 7 ? " NO RESOURCES" : "",
         st, g[GSI_ERROR_LOG / 4]);
  }
  UnmapPhys ((VOID *)g, SIZE_4KB);
}

STATIC UINT32 IpaClockKhz(VOID)
{
  UNICODE_STRING key = RTL_CONSTANT_STRING (L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\TopazRpm\\State");
  UNICODE_STRING val = RTL_CONSTANT_STRING (L"IpaClockKhz");
  OBJECT_ATTRIBUTES oa;
  HANDLE h;
  UCHAR buf[sizeof (KEY_VALUE_PARTIAL_INFORMATION) + 8];
  ULONG len;
  UINT32 khz = 0;

  InitializeObjectAttributes (&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
  if (NT_SUCCESS (ZwOpenKey (&h, KEY_QUERY_VALUE, &oa))) {
    if (NT_SUCCESS (ZwQueryValueKey (h, &val, KeyValuePartialInformation, buf, sizeof (buf), &len)) &&
        ((KEY_VALUE_PARTIAL_INFORMATION *)buf)->DataLength == 4) {
      khz = *(UINT32 *)((KEY_VALUE_PARTIAL_INFORMATION *)buf)->Data;
    }
    ZwClose (h);
  }
  return khz;
}

STATIC VOID IpaFwLoad(EFI_FILE_PROTOCOL *Root)
{
  UINT8 *mdt = NULL, *meta, *region;
  UINTN mdtSize = 0, metaSize, res = 0, st, i, sz, t;
  Elf32_Ehdr *eh;
  Elf32_Phdr *ph;
  UINT32 minAddr = MAX_UINT32, maxAddr = 0, hashIdx = MAX_UINT32, khz = 0;
  UINT64 metaPa = 0;
  CHAR16 name[32];
  EFI_FILE_PROTOCOL *f = NULL;
  EFI_STATUS s;

  if (EFI_ERROR (Root->Open (Root, &f, L"\\ipa.on", EFI_FILE_MODE_READ, 0))) {
    Out ("  ipa: C:\\topaz\\fw\\ipa.on absent, GSI firmware not loaded\r\n");
    return;
  }
  f->Close (f);
  SmmuIpaMap ();                               /* before anything in IPA/GSI can DMA */
  LogHardFlush ();
  for (t = 0; t < 150 && (khz = IpaClockKhz ()) == 0; t++) {
    gBS->Stall (100 * 1000);
  }
  Out ("  ipa: RPM IPA clock vote %u kHz after %u ms\r\n", khz, (UINT32)(t * 100));
  if (khz == 0) {
    Out ("  ipa: no IPA clock vote this boot (TopazRpm, C:\\topaz\\rpm.on?): not touching IPA\r\n");
    return;
  }
  s = ReadFile (Root, L"\\image\\ipa_fws.mdt", (VOID **)&mdt, &mdtSize, NULL, 0);
  if (EFI_ERROR (s) || mdtSize < sizeof (Elf32_Ehdr)) {
    Out ("  ipa: ipa_fws.mdt %r\r\n", s);
    return;
  }
  eh = (Elf32_Ehdr *)mdt;
  ph = (Elf32_Phdr *)(mdt + eh->e_phoff);
  if (eh->e_phoff + (UINTN)eh->e_phnum * sizeof (Elf32_Phdr) > mdtSize) {
    Out ("  ipa: bad ELF\r\n");
    FreePool (mdt);
    return;
  }
  for (i = 0; i < eh->e_phnum; i++) {
    if ((ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
      hashIdx = (UINT32)i;
    }
    if (PhdrLoadable (&ph[i])) {
      minAddr = MIN (minAddr, ph[i].p_paddr);
      maxAddr = MAX (maxAddr, ph[i].p_paddr + ph[i].p_memsz);
    }
  }
  Out ("  ipa: ipa_fws.mdt %u phdrs, hash %u, image %08x..%08x\r\n", eh->e_phnum, hashIdx, minAddr, maxAddr);
  if (hashIdx == MAX_UINT32 || maxAddr <= minAddr || maxAddr - minAddr > IPA_FW_SIZE) {
    FreePool (mdt);
    return;
  }
  sz = eh->e_phoff + eh->e_phnum * sizeof (Elf32_Phdr);
  metaSize = sz + ph[hashIdx].p_filesz;
  meta = PhysAlloc (metaSize, &metaPa);
  if (meta == NULL) {
    FreePool (mdt);
    return;
  }
  CopyMem (meta, mdt, sz);
  if (ph[hashIdx].p_offset + ph[hashIdx].p_filesz <= mdtSize) {
    CopyMem (meta + sz, mdt + ph[hashIdx].p_offset, ph[hashIdx].p_filesz);
  } else {
    UINTN got = 0;
    UnicodeSPrint (name, sizeof (name), L"\\image\\ipa_fws.b%02u", hashIdx);
    s = ReadFile (Root, name, NULL, &got, meta + sz, ph[hashIdx].p_filesz);
    if (EFI_ERROR (s) || got != ph[hashIdx].p_filesz) {
      Out ("  ipa: hash segment ipa_fws.b%02u: %r\r\n", hashIdx, s);
      goto Out;
    }
  }
  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_INIT_IMAGE), SCM_ARGS (2) | (SCM_ARG_RW << 6), PAS_ID_IPA, (UINTN)metaPa, 0, &res);
  Out ("  ipa: PAS init_image(15): ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
  if (st != 0 || res != 0) {
    goto Out;
  }
  /*
   * v0.12: mem_setup with the exact image size 0x41C0 -> ret ffcfffba. Downstream PIL rounds the
   * region to 4 KiB pages; try that.
   */
  sz = ALIGN_VALUE (maxAddr - minAddr, SIZE_4KB);
  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_MEM_SETUP), SCM_ARGS (3), PAS_ID_IPA, IPA_FW_PA, sz, &res);
  Out ("  ipa: PAS mem_setup(15, %08x, %lx): ret=%lx res=%lx\r\n", IPA_FW_PA, (UINT64)sz, (UINT64)st, (UINT64)res);
  if (st != 0 || res != 0) {
    st = Scm (SCM_FN (SCM_SVC_PIL, PIL_SHUTDOWN), SCM_ARGS (1), PAS_ID_IPA, 0, 0, &res);
    Out ("  ipa: PAS shutdown(15): ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
    goto Out;
  }
  region = MapPhys (IPA_FW_PA, IPA_FW_SIZE, TRUE);
  if (region == NULL) {
    goto Out;
  }
  ZeroMem (region, maxAddr - minAddr);
  for (i = 0; i < eh->e_phnum; i++) {
    UINT8 *dst;
    if (!PhdrLoadable (&ph[i])) {
      continue;
    }
    dst = region + (ph[i].p_paddr - minAddr);
    sz = 0;
    if (ph[i].p_filesz != 0) {
      UnicodeSPrint (name, sizeof (name), L"\\image\\ipa_fws.b%02u", (UINT32)i);
      s = ReadFile (Root, name, NULL, &sz, dst, ph[i].p_memsz);
      if (EFI_ERROR (s) || sz != ph[i].p_filesz) {
        Out ("  ipa: ipa_fws.b%02u: %r (%lu of %u)\r\n", (UINT32)i, s, (UINT64)sz, ph[i].p_filesz);
        UnmapPhys (region, IPA_FW_SIZE);
        goto Out;
      }
    }
  }
  MemoryFence ();
  UnmapPhys (region, IPA_FW_SIZE);
  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_AUTH_RESET), SCM_ARGS (1), PAS_ID_IPA, 0, 0, &res);
  Out ("  ipa: PAS auth_and_reset(15): ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
  if (st == 0 && res == 0) {
    UINT32 *gs = MapPhys (GSI_STATUS_PA & ~0xFFFu, SIZE_4KB, FALSE);
    if (gs != NULL) {
      Out ("  ipa: GSI_STATUS = %08x%a\r\n", gs[(GSI_STATUS_PA & 0xFFF) / 4],
           (gs[(GSI_STATUS_PA & 0xFFF) / 4] & 1) ? " (ENABLED: GSI firmware running)" : "");
      gIpaFwRunning = (gs[(GSI_STATUS_PA & 0xFFF) / 4] & 1) != 0;
      LogHardFlush ();
      UnmapPhys (gs, SIZE_4KB);
    }
    if (gIpaFwRunning) {
      IpaCanaries ();
      GsiAllocModemChannels (Root);
    }
  }
Out:
  PhysFree (meta);
  FreePool (mdt);
}

EFI_STATUS ModemPasTest(VOID)
{
  EFI_FILE_PROTOCOL *root;
  UINT8 *mdt = NULL, *meta;
  UINTN mdtSize = 0, metaSize, res = 0, st, i, sz;
  Elf32_Ehdr *eh;
  Elf32_Phdr *ph;
  UINT32 minAddr = MAX_UINT32, maxAddr = 0, hashIdx = MAX_UINT32;
  BOOLEAN relocate = FALSE;
  UINT64 metaPa = 0;
  CHAR16 name[32];
  EFI_STATUS s;

  Out ("\r\n  ==== Wi-Fi P1: modem (MPSS) PAS boot ====\r\n");
  if (EFI_ERROR (ModemMapInit ())) {
    return EFI_OUT_OF_RESOURCES;
  }
  SmmuProbe (TRUE);
  SmmuWlanMap ();
  PmicProbe ("before modem boot");             /* WLAN/RF rails, read-only (P4 diagnostics) */
  {
    /* The modem must start cold: its GLINK/QRTR peer state from an earlier boot is gone. */
    SMEM_PART_HDR *part = SmemPartition (0, 1);
    UINT32 sz = 0;
    Out ("  SMEM version %u, apps<->modem partition %p\r\n", SmemVersion (), part);
    if (part != NULL && SmemPrivGet (part, SMEM_ITEM_SMP2P_IN, &sz) != NULL) {
      Out ("  modem SMP2P item 435 already exists: the modem is running (UEFI \"Modem test\"?).\r\n"
           "  Refusing. Power the phone off fully and boot Windows without the modem test.\r\n");
      return EFI_ALREADY_STARTED;
    }
  }

  {
    STATIC CONST UINT8 cmds[] = { PIL_INIT_IMAGE, PIL_MEM_SETUP, PIL_AUTH_RESET, PIL_IS_SUPPORTED };
    UINTN k, av[4];
    for (k = 0; k < 4; k++) {
      av[k] = 0xEE;
      Scm (SCM_FN (0x06, 0x01), SCM_ARGS (1), 0x02000000u | (SCM_SVC_PIL << 8) | cmds[k], 0, 0, &av[k]);
    }
    Out ("  call available: init=%lu mem=%lu auth=%lu is_sup=%lu\r\n",
         (UINT64)av[0], (UINT64)av[1], (UINT64)av[2], (UINT64)av[3]);
  }
  for (i = 0; i < 16; i++) {
    st = Scm (SCM_FN (SCM_SVC_PIL, PIL_IS_SUPPORTED), SCM_ARGS (1), i, 0, 0, &res);
    if (st == 0 && res == 1) {
      Out ("  PAS id %lu supported\r\n", (UINT64)i);
    }
  }

  root = OpenFwDir ();
  if (root == NULL) {
    return EFI_NOT_FOUND;
  }
  IpaFwLoad (root);                            /* GSI firmware first, like Android (C:\topaz\fw\ipa.on) */
  s = ReadFile (root, L"\\image\\modem.mdt", (VOID **)&mdt, &mdtSize, NULL, 0);
  Out ("  modem.mdt: %r, %lu bytes\r\n", s, (UINT64)mdtSize);
  if (EFI_ERROR (s)) {
    return s;
  }
  eh = (Elf32_Ehdr *)mdt;
  if (mdtSize < sizeof (*eh) || CompareMem (eh->e_ident, "\x7f" "ELF", 4) != 0 ||
      eh->e_phoff + (UINTN)eh->e_phnum * sizeof (Elf32_Phdr) > mdtSize) {
    Out ("  bad ELF header\r\n");
    return EFI_LOAD_ERROR;
  }
  ph = (Elf32_Phdr *)(mdt + eh->e_phoff);
  for (i = 0; i < eh->e_phnum; i++) {
    if ((ph[i].p_flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
      hashIdx = (UINT32)i;
    }
    if (!PhdrLoadable (&ph[i])) {
      continue;
    }
    if (ph[i].p_flags & MDT_RELOCATABLE) {
      relocate = TRUE;
    }
    minAddr = MIN (minAddr, ph[i].p_paddr);
    maxAddr = MAX (maxAddr, ph[i].p_paddr + ph[i].p_memsz);
  }
  Out ("  phdrs=%u hash=%u region %08x..%08x relocate=%u\r\n", eh->e_phnum, hashIdx, minAddr, maxAddr, relocate);
  if (hashIdx == MAX_UINT32) {
    Out ("  no hash segment\r\n");
    return EFI_LOAD_ERROR;
  }

  /* metadata = ELF header + phdrs + hash segment, physically contiguous below 4 GiB */
  sz = eh->e_phoff + eh->e_phnum * sizeof (Elf32_Phdr);
  metaSize = sz + ph[hashIdx].p_filesz;
  meta = PhysAlloc (metaSize, &metaPa);
  if (meta == NULL) {
    Out ("  metadata alloc (%lu bytes) failed\r\n", (UINT64)metaSize);
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (meta, mdt, sz);
  if (ph[hashIdx].p_offset + ph[hashIdx].p_filesz <= mdtSize) {
    CopyMem (meta + sz, mdt + ph[hashIdx].p_offset, ph[hashIdx].p_filesz);
  } else {
    UINTN got = 0;
    /* split firmware: the hash segment is its own .bNN file (Linux qcom_mdt_read_metadata) */
    UnicodeSPrint (name, sizeof (name), L"\\image\\modem.b%02u", hashIdx);
    s = ReadFile (root, name, NULL, &got, meta + sz, ph[hashIdx].p_filesz);
    Out ("  hash from modem.b%02u: %r, %lu of %u bytes\r\n", hashIdx, s, (UINT64)got, ph[hashIdx].p_filesz);
    if (EFI_ERROR (s) || got != ph[hashIdx].p_filesz) {
      return EFI_LOAD_ERROR;
    }
  }

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_INIT_IMAGE), SCM_ARGS (2) | (SCM_ARG_RW << 6), PAS_ID_MODEM, (UINTN)metaPa, 0, &res);
  Out ("  PAS init_image: ret=%lx res=%lx resumes=%lu (meta %lu bytes @%lx)\r\n", (UINT64)st, (UINT64)res, (UINT64)mScmResumes, (UINT64)metaSize, (UINT64)metaPa);
  if (st != 0 || res != 0) {
    return EFI_SECURITY_VIOLATION;
  }
  if (relocate) {
    st = Scm (SCM_FN (SCM_SVC_PIL, PIL_MEM_SETUP), SCM_ARGS (3), PAS_ID_MODEM, minAddr, maxAddr - minAddr, &res);
    Out ("  PAS mem_setup: ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
  }

  for (i = 0; i < eh->e_phnum; i++) {
    UINT8 *dst;
    if (!PhdrLoadable (&ph[i])) {
      continue;
    }
    /* carveout 0x4AB00000.. is "PIL Reserved" (not Windows RAM): map it only while copying,
       TZ locks it at auth_and_reset */
    dst = MapPhys (ph[i].p_paddr, ph[i].p_memsz, TRUE);
    if (dst == NULL) {
      Out ("  segment %u: cannot map %08x+%x\r\n", (UINT32)i, ph[i].p_paddr, ph[i].p_memsz);
      return EFI_OUT_OF_RESOURCES;
    }
    sz = 0;
    if (ph[i].p_filesz != 0) {
      UnicodeSPrint (name, sizeof (name), L"\\image\\modem.b%02u", (UINT32)i);
      s = ReadFile (root, name, NULL, &sz, dst, ph[i].p_memsz);
      if (EFI_ERROR (s) || sz != ph[i].p_filesz) {
        Out ("  modem.b%02u: %r (%lu of %u bytes)\r\n", (UINT32)i, s, (UINT64)sz, ph[i].p_filesz);
        UnmapPhys (dst, ph[i].p_memsz);
        return EFI_LOAD_ERROR;
      }
    }
    if (ph[i].p_memsz > sz) {
      ZeroMem (dst + sz, ph[i].p_memsz - sz);
    }
    MemoryFence ();
    UnmapPhys (dst, ph[i].p_memsz);
  }
  Out ("  segments loaded\r\n");

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_AUTH_RESET), SCM_ARGS (1), PAS_ID_MODEM, 0, 0, &res);
  Out ("  PAS auth_and_reset: ret=%lx res=%lx resumes=%lu (0/0 = TZ verified and started the modem)\r\n", (UINT64)st, (UINT64)res, (UINT64)mScmResumes);
  PhysFree (meta);
  FreePool (mdt);
  if (st != 0 || res != 0) {
    return EFI_SECURITY_VIOLATION;
  }
  ModemWatch (20, root);
  return EFI_SUCCESS;
}
