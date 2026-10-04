/*
 * Boot the ADSP through TrustZone PAS (Linux qcom_mdt_load() + qcom_q6v5_pas), like
 * drivers/TopazModem/ModemPas.c does for the modem: init_image with the .mdt metadata, copy the
 * adsp.bNN segments into the carveout, auth_and_reset. Then the SMP2P handshake (apps item 429,
 * adsp item 443 in the apps<->adsp SMEM partition) and the GLINK/QRTR/GPR loop.
 * Firmware: C:\topaz\fw\image\adsp.* (NON-HLOS of modem_a, already copied for Wi-Fi).
 */
#include "Audio.h"

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
typedef struct { UINT32 Magic; UINT8 Version, Features[3]; UINT16 LocalPid, RemotePid, Total, Valid; UINT32 Flags; } SMP2P_HDR;
typedef struct { CHAR8 Name[16]; UINT32 Value; } SMP2P_ENTRY;
#pragma pack()

#define PT_LOAD           1
#define SCM_FN(svc, cmd)  (0x42000000u | ((UINT32)(svc) << 8) | (UINT32)(cmd))
#define SCM_SVC_PIL       0x02
#define PIL_INIT_IMAGE    0x01
#define PIL_MEM_SETUP     0x02
#define PIL_AUTH_RESET    0x05
#define PIL_SHUTDOWN      0x06
#define SCM_ARG_RW        2
#define SCM_ARGS(n)       (n)
#define MDT_TYPE_MASK     (7u << 24)
#define MDT_TYPE_HASH     (2u << 24)
#define MDT_RELOCATABLE   (1u << 27)

#define ADSP_CARVE_PA     0x53800000u        /* DT pil_adsp_region */
#define ADSP_CARVE_SIZE   0x02300000u
#define SMP2P_MAGIC       0x504D5324u        /* "$SMP" */
#define SMP2P_MAX_ENTRY   16
#define SMEM_ITEM_ADSP_CRASH 423             /* Linux qcom_q6v5 crash_reason_smem for adsp */

volatile UINT32 *gAdspState;
STATIC UINTN     mScmResumes, mT0;

UINTN AudMs(VOID)
{
  return (UINTN)(GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000) - mT0;
}

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
  /* QCOM_SCM_INTERRUPTED (1): resume with x0 = 1 and the returned x6 (Linux __scm_smc_do_quirk) */
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

/* Read a whole file into *Buf (pool) or into Dest when Dest != NULL. */
STATIC EFI_STATUS ReadFile(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Name, VOID **Buf, UINTN *Size, VOID *Dest, UINTN DestMax)
{
  EFI_FILE_PROTOCOL *f;
  EFI_FILE_INFO info;
  UINTN isz = sizeof (info), sz;
  EFI_STATUS s;
  VOID *p;

  s = Root->Open (Root, &f, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (s)) {
    return s;
  }
  s = f->GetInfo (f, &gEfiFileInfoGuid, &isz, &info);
  sz = EFI_ERROR (s) ? 0 : (UINTN)info.FileSize;
  if (EFI_ERROR (s) || (Dest != NULL && sz > DestMax)) {
    f->Close (f);
    return EFI_ERROR (s) ? s : EFI_BUFFER_TOO_SMALL;
  }
  p = (Dest != NULL) ? Dest : AllocatePool (sz);
  if (p == NULL) {
    f->Close (f);
    return EFI_OUT_OF_RESOURCES;
  }
  s = f->Read (f, &sz, p);
  f->Close (f);
  if (Buf != NULL) {
    *Buf = p;
  }
  *Size = sz;
  return s;
}

STATIC BOOLEAN PhdrLoadable(CONST Elf32_Phdr *P)
{
  return P->p_type == PT_LOAD && P->p_memsz != 0 && (P->p_flags & MDT_TYPE_MASK) != MDT_TYPE_HASH;
}

/* ---------------- SMP2P (Linux qcom_smp2p.c, downstream entry names) ---------------- */

STATIC SMP2P_HDR *mOut, *mIn;
STATIC UINT32    *mStopEntry;

STATIC UINT32 *Smp2pFind(SMP2P_HDR *H, CONST CHAR8 *Name)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(H + 1);
  UINT32 i;

  for (i = 0; i < H->Valid && i < SMP2P_MAX_ENTRY; i++) {
    if (AsciiStrnCmp (e[i].Name, Name, 16) == 0) {
      return &e[i].Value;
    }
  }
  return NULL;
}

/* Add an outbound entry (qcom_smp2p_add_outbound_entry) unless it exists. */
STATIC UINT32 *Smp2pAdd(SMP2P_HDR *H, CONST CHAR8 *Name, UINT32 Value)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(H + 1);
  UINT32 *v = Smp2pFind (H, Name);

  if (v != NULL || H->Valid >= H->Total) {
    return v;
  }
  ZeroMem (e[H->Valid].Name, 16);
  CopyMem (e[H->Valid].Name, Name, AsciiStrLen (Name));
  e[H->Valid].Value = Value;
  MemoryFence ();
  H->Valid++;
  MemoryFence ();
  return &e[H->Valid - 1].Value;
}

STATIC VOID Smp2pDump(CONST CHAR8 *Tag, SMP2P_HDR *H)
{
  SMP2P_ENTRY *e = (SMP2P_ENTRY *)(H + 1);
  UINT32 i;

  Out ("  %a: magic %08x v%u feat %02x pid %u->%u entries %u/%u\r\n", Tag, H->Magic, H->Version,
       H->Features[0], H->LocalPid, H->RemotePid, H->Valid, H->Total);
  for (i = 0; i < H->Valid && i < SMP2P_MAX_ENTRY; i++) {
    CHAR8 n[17];
    CopyMem (n, e[i].Name, 16);
    n[16] = 0;
    Out ("    [%u] %-16a = %08x\r\n", i, n, e[i].Value);
  }
}

/* Our apps -> adsp item 429 with the entries the stock kernel creates at probe. */
STATIC BOOLEAN Smp2pOutInit(SMEM_PART_HDR *P)
{
  mOut = SmemPrivAlloc (P, ADSP_SMP2P_OUT, sizeof (SMP2P_HDR) + SMP2P_MAX_ENTRY * sizeof (SMP2P_ENTRY));
  if (mOut == NULL) {
    return FALSE;
  }
  if (mOut->Version == 0) {
    mOut->Magic       = SMP2P_MAGIC;
    mOut->LocalPid    = 0;
    mOut->RemotePid   = ADSP_PID;
    mOut->Total       = SMP2P_MAX_ENTRY;
    mOut->Valid       = 0;
    mOut->Features[0] = 1;                      /* SSR_ACK */
    MemoryFence ();
    mOut->Version     = 1;
  }
  mStopEntry = Smp2pAdd (mOut, "master-kernel", 0);   /* bit0 = stop request */
  Smp2pAdd (mOut, "sleepstate", 1);                   /* bit0 = apps awake (smp2p_sleepstate) */
  MemoryFence ();
  ApcsKick (ADSP_SMP2P_BIT);
  Smp2pDump ("apps->adsp (429)", mOut);
  return TRUE;
}

/* ---------------- PAS load ---------------- */

STATIC EFI_STATUS AdspLoad(EFI_FILE_PROTOCOL *Root)
{
  UINT8 *mdt = NULL, *meta;
  UINTN mdtSize = 0, metaSize, res = 0, st, i, sz;
  Elf32_Ehdr *eh;
  Elf32_Phdr *ph;
  UINT32 minAddr = MAX_UINT32, maxAddr = 0, hashIdx = MAX_UINT32;
  BOOLEAN relocate = FALSE;
  UINT64 metaPa = 0;
  CHAR16 name[32];
  EFI_STATUS s;

  s = ReadFile (Root, L"\\image\\adsp.mdt", (VOID **)&mdt, &mdtSize, NULL, 0);
  Out ("  adsp.mdt: %r, %lu bytes\r\n", s, (UINT64)mdtSize);
  if (EFI_ERROR (s)) {
    return s;
  }
  eh = (Elf32_Ehdr *)mdt;
  if (mdtSize < sizeof (*eh) || CompareMem (eh->e_ident, "\x7f" "ELF", 4) != 0 ||
      eh->e_phoff + (UINTN)eh->e_phnum * sizeof (Elf32_Phdr) > mdtSize) {
    Out ("  bad ELF header\r\n");
    FreePool (mdt);
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
    relocate = relocate || (ph[i].p_flags & MDT_RELOCATABLE) != 0;
    minAddr = MIN (minAddr, ph[i].p_paddr);
    maxAddr = MAX (maxAddr, ph[i].p_paddr + ph[i].p_memsz);
  }
  Out ("  phdrs=%u hash=%u region %08x..%08x relocate=%u\r\n", eh->e_phnum, hashIdx, minAddr, maxAddr, relocate);
  /* never write outside the DT carveout (it is "PIL Reserved", not Windows RAM) */
  if (hashIdx == MAX_UINT32 || minAddr < ADSP_CARVE_PA || maxAddr > ADSP_CARVE_PA + ADSP_CARVE_SIZE) {
    Out ("  no hash segment or segments outside the carveout %08x+%x\r\n", ADSP_CARVE_PA, ADSP_CARVE_SIZE);
    FreePool (mdt);
    return EFI_LOAD_ERROR;
  }

  /* metadata = ELF header + phdrs + hash segment, physically contiguous below 4 GiB */
  sz = eh->e_phoff + eh->e_phnum * sizeof (Elf32_Phdr);
  metaSize = sz + ph[hashIdx].p_filesz;
  meta = PhysAlloc (metaSize, &metaPa);
  if (meta == NULL) {
    FreePool (mdt);
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (meta, mdt, sz);
  if (ph[hashIdx].p_offset + ph[hashIdx].p_filesz <= mdtSize) {
    CopyMem (meta + sz, mdt + ph[hashIdx].p_offset, ph[hashIdx].p_filesz);
  } else {
    UINTN got = 0;
    UnicodeSPrint (name, sizeof (name), L"\\image\\adsp.b%02u", hashIdx);
    s = ReadFile (Root, name, NULL, &got, meta + sz, ph[hashIdx].p_filesz);
    Out ("  hash from adsp.b%02u: %r, %lu of %u bytes\r\n", hashIdx, s, (UINT64)got, ph[hashIdx].p_filesz);
    if (EFI_ERROR (s) || got != ph[hashIdx].p_filesz) {
      s = EFI_LOAD_ERROR;
      goto done;
    }
  }

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_INIT_IMAGE), SCM_ARGS (2) | (SCM_ARG_RW << 6), ADSP_PAS_ID, (UINTN)metaPa, 0, &res);
  Out ("  PAS init_image: ret=%lx res=%lx resumes=%lu (meta %lu bytes)\r\n", (UINT64)st, (UINT64)res, (UINT64)mScmResumes, (UINT64)metaSize);
  if (st != 0 || res != 0) {
    s = EFI_SECURITY_VIOLATION;
    goto done;
  }
  if (relocate) {
    /* TopazGpu lesson: the PAS mem_setup size must be 4 KiB aligned */
    st = Scm (SCM_FN (SCM_SVC_PIL, PIL_MEM_SETUP), SCM_ARGS (3), ADSP_PAS_ID, minAddr,
              ALIGN_VALUE (maxAddr - minAddr, SIZE_4KB), &res);
    Out ("  PAS mem_setup: ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
  }

  for (i = 0; i < eh->e_phnum; i++) {
    UINT8 *dst;
    if (!PhdrLoadable (&ph[i])) {
      continue;
    }
    dst = MapPhys (ph[i].p_paddr, ph[i].p_memsz, TRUE);
    if (dst == NULL) {
      Out ("  segment %u: cannot map %08x+%x\r\n", (UINT32)i, ph[i].p_paddr, ph[i].p_memsz);
      s = EFI_OUT_OF_RESOURCES;
      goto done;
    }
    sz = 0;
    if (ph[i].p_filesz != 0) {
      UnicodeSPrint (name, sizeof (name), L"\\image\\adsp.b%02u", (UINT32)i);
      s = ReadFile (Root, name, NULL, &sz, dst, ph[i].p_memsz);
      if (EFI_ERROR (s) || sz != ph[i].p_filesz) {
        Out ("  adsp.b%02u: %r (%lu of %u bytes)\r\n", (UINT32)i, s, (UINT64)sz, ph[i].p_filesz);
        UnmapPhys (dst, ph[i].p_memsz);
        s = EFI_LOAD_ERROR;
        goto done;
      }
    }
    if (ph[i].p_memsz > sz) {
      ZeroMem (dst + sz, ph[i].p_memsz - sz);
    }
    MemoryFence ();
    UnmapPhys (dst, ph[i].p_memsz);
  }
  Out ("  segments loaded\r\n");

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_AUTH_RESET), SCM_ARGS (1), ADSP_PAS_ID, 0, 0, &res);
  Out ("  PAS auth_and_reset: ret=%lx res=%lx resumes=%lu\r\n", (UINT64)st, (UINT64)res, (UINT64)mScmResumes);
  s = (st != 0 || res != 0) ? EFI_SECURITY_VIOLATION : EFI_SUCCESS;
done:
  PhysFree (meta);
  FreePool (mdt);
  return s;
}

STATIC UINTN PasShutdown(VOID)
{
  UINTN res = 0, st;

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_SHUTDOWN), SCM_ARGS (1), ADSP_PAS_ID, 0, 0, &res);
  Out ("  PAS shutdown(adsp): ret=%lx res=%lx\r\n", (UINT64)st, (UINT64)res);
  return st != 0 ? st : res;
}

/* ---------------- flow ---------------- */

/* List the items in a private SMEM partition (uncached entries grow up from the header). */
STATIC UINT32 PartDump(CONST CHAR8 *Tag, SMEM_PART_HDR *P, BOOLEAN Print)
{
  UINT8 *e = (UINT8 *)(P + 1), *end = (UINT8 *)P + P->FreeUncached;
  UINT32 n = 0;

  if (Print) {
    Out ("  %a: partition %u<->%u size %x free %x..%x\r\n", Tag, P->Host0, P->Host1, P->Size, P->FreeUncached, P->FreeCached);
  }
  while (e + sizeof (SMEM_PRIV_ENTRY) <= end && n < 64) {
    SMEM_PRIV_ENTRY *h = (SMEM_PRIV_ENTRY *)e;
    if (h->Canary != 0xA5A5) {
      Out ("   bad canary %04x at +%x\r\n", h->Canary, (UINT32)(e - (UINT8 *)P));
      break;
    }
    if (Print) {
      Out ("   item %u size %x\r\n", h->Item, h->Size);
    }
    e += sizeof (*h) + h->PadHdr + h->Size;
    n++;
  }
  return n;
}

STATIC UINT32 WdogPending(VOID)
{
  return (MmioRead32 (gGicdVa + 0x200 + (ADSP_WDOG_INTID / 32) * 4) >> (ADSP_WDOG_INTID % 32)) & 1;
}

STATIC VOID CrashReason(VOID)
{
  UINT32 len = 0;
  CHAR8 *crash = (CHAR8 *)SmemGlobalGet (SMEM_ITEM_ADSP_CRASH, &len);

  if (crash != NULL && len != 0 && crash[0] != 0) {
    CHAR8 msg[160];
    AsciiStrnCpyS (msg, sizeof (msg), crash, MIN (len, sizeof (msg) - 1));
    Out ("  crash reason (%u): %a\r\n", SMEM_ITEM_ADSP_CRASH, msg);
  } else {
    Out ("  crash reason (%u): %a\r\n", SMEM_ITEM_ADSP_CRASH, crash == NULL ? "item absent" : "empty");
  }
}

/* Wait for "slave-kernel" ready (bit1) or fatal (bit0) in the adsp's item 443. */
STATIC UINT32 WaitReady(SMEM_PART_HDR *Part, UINTN Ms, UINT32 Stale)
{
  UINT32 last = 0xFFFFFFFF, sz = 0, wd = 2;
  BOOLEAN negotiated = FALSE;
  UINTN lastDump = 0;
  UINT32 items = PartDump (NULL, Part, FALSE);

  for (mT0 = 0, mT0 = AudMs (); (Ms == 0 || AudMs () < Ms) && !gModemStop; gBS->Stall (10 * 1000)) {
    if (PartDump (NULL, Part, FALSE) != items) {
      items = PartDump (NULL, Part, FALSE);
      Out ("  t=%lu ms partition now has %u items\r\n", (UINT64)AudMs (), items);
      PartDump ("changed", Part, TRUE);
    }
    if (AudMs () - lastDump >= 5000) {
      lastDump = AudMs ();
      Out ("  t=%lu ms waiting: item %u %a, wdog %u\r\n", (UINT64)lastDump, ADSP_SMP2P_IN, mIn != NULL ? "present" : "absent", WdogPending ());
      if (lastDump % 30000 < 5000) {
        CrashReason ();
      }
    }
    if (WdogPending () != wd) {
      wd = WdogPending ();
      Out ("  t=%lu ms adsp wdog SPI pending=%u\r\n", (UINT64)AudMs (), wd);
    }
    if (mIn == NULL) {
      mIn = SmemPrivGet (Part, ADSP_SMP2P_IN, &sz);
      if (mIn == NULL) {
        continue;
      }
      Out ("  t=%lu ms adsp SMP2P item %u appeared (%u bytes)\r\n", (UINT64)AudMs (), ADSP_SMP2P_IN, sz);
    }
    if (!negotiated && mIn->Version == mOut->Version) {
      mOut->Features[0] &= mIn->Features[0];
      negotiated = TRUE;
      ApcsKick (ADSP_SMP2P_BIT);
    }
    if (gAdspState == NULL) {
      gAdspState = Smp2pFind (mIn, "slave-kernel");
      continue;
    }
    if (*gAdspState != last) {
      last = *gAdspState;
      Out ("  t=%lu ms slave-kernel=%08x%a%a%a%a\r\n", (UINT64)AudMs (), last, (last & 1) ? " FATAL" : "",
           (last & 2) ? " READY" : "", (last & 4) ? " handover" : "", (last & 8) ? " stop-ack" : "");
      if (last == Stale) {                     /* restart: value left by the previous run */
        Out ("  (stale value from the previous run, waiting for a change)\r\n");
        continue;
      }
      Stale = 0xFFFFFFFF;
      if (last & 3) {
        break;
      }
    }
  }
  return (gAdspState != NULL) ? *gAdspState : 0;
}

/* Linux qcom_q6v5_request_stop(): stop bit in "master-kernel", wait stop-ack, then PAS shutdown. */
STATIC VOID AdspStop(VOID)
{
  UINTN t;

  if (mStopEntry != NULL && gAdspState != NULL && !(*gAdspState & 1)) {
    *mStopEntry |= 1;
    MemoryFence ();
    ApcsKick (ADSP_SMP2P_BIT);
    for (t = 0; t < 150 && !(*gAdspState & 8); t++) {
      gBS->Stall (10 * 1000);
    }
    Out ("  stop request: slave-kernel=%08x after %lu ms\r\n", *gAdspState, (UINT64)t * 10);
  }
  PasShutdown ();
  if (mStopEntry != NULL) {
    *mStopEntry = 0;
  }
}

EFI_STATUS AdspBoot(VOID)
{
  SMEM_PART_HDR *part;
  UINT32 st, stale = 0xFFFFFFFF;
  UINTN lastStatus = 0;
  EFI_STATUS s;

  Out ("\r\n  ==== ADSP (LPASS) PAS boot ====\r\n");
  if (EFI_ERROR (ModemMapInit ())) {
    return EFI_OUT_OF_RESOURCES;
  }
  part = SmemPartition (0, ADSP_PID);
  Out ("  SMEM version %u, apps<->adsp partition %p\r\n", SmemVersion (), part);
  if (part == NULL) {
    return EFI_NOT_FOUND;
  }
  PartDump ("before boot", part, TRUE);
  CrashReason ();
  /*
   * Any ADSP item in the partition means the ADSP already ran since SMEM was set up (earlier
   * driver run). v0.2 restarted it with PAS shutdown + boot over that state and the whole SoC
   * reset a few seconds later, so never do that: a cold boot is needed to try again.
   */
  if (SmemPrivGet (part, ADSP_SMP2P_IN, NULL) != NULL || SmemPrivGet (part, 480, NULL) != NULL) {
    Out ("  ADSP state from an earlier run is in SMEM (item %u or 480): refusing to boot it again.\r\n"
         "  Power the phone off fully (not restart) and boot Windows again.\r\n", ADSP_SMP2P_IN);
    return EFI_ALREADY_STARTED;
  }
  if (!Smp2pOutInit (part) || mStopEntry == NULL) {
    Out ("  SMP2P item %u alloc failed\r\n", ADSP_SMP2P_OUT);
    return EFI_OUT_OF_RESOURCES;
  }
  *mStopEntry = 0;
  ApcsKick (ADSP_SMP2P_BIT);

  s = AdspLoad (FwRoot ());
  if (EFI_ERROR (s)) {
    return s;
  }
  st = WaitReady (part, 0, stale);         /* until READY/FATAL or driver stop */
  if (mIn != NULL) {
    Smp2pDump ("adsp->apps (443)", mIn);
  }
  if ((st & 3) != 2) {
    Out ("  ADSP did not report READY (slave-kernel %08x)\r\n", st);
    CrashReason ();
    PartDump ("after wait", part, TRUE);
    if (!(st & 1)) {
      AdspStop ();
    }
    return EFI_DEVICE_ERROR;
  }

  GlinkInit (part);
  QrtrInit ();
  GprInit ();
  LogSetLazy (TRUE);
  while (!gModemStop) {
    BOOLEAN busy = GlinkPoll ();
    QrtrPoll ();
    GprPoll ();
    LabPoll ();
    if (*gAdspState & 1) {
      Out ("  t=%lu ms *** ADSP FATAL (slave-kernel %08x, wdog %u) ***\r\n", (UINT64)AudMs (), *gAdspState, WdogPending ());
      CrashReason ();
      break;
    }
    if (AudMs () - lastStatus >= 30000) {
      lastStatus = AudMs ();
      Out ("  t=%lu ms status: slave-kernel %08x wdog %u\r\n", (UINT64)lastStatus, *gAdspState, WdogPending ());
      GlinkSummary ();
    }
    ModemIdle (busy);
  }
  LogSetLazy (FALSE);
  GlinkSummary ();
  QrtrSummary ();
  GprSummary ();
  if (!(*gAdspState & 1)) {
    AdspStop ();
  }
  return EFI_SUCCESS;
}
