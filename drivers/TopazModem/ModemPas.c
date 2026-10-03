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
    CHAR8 msg[120];
    AsciiStrnCpyS (msg, sizeof (msg), crash, MIN (crashLen, sizeof (msg) - 1));
    Out ("  crash reason (421): %a\r\n", msg);
  } else {
    Out ("  crash reason (421): %a\r\n", crash == NULL ? "item absent" : "empty");
  }
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
