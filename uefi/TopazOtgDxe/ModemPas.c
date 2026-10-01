/*
 * Wi-Fi P1 spike: boot the modem DSP (MPSS, it runs the WCN3990 WLAN firmware) the way
 * Linux qcom_mdt_load() + qcom_q6v5_pas do: TrustZone PAS init_image with the .mdt
 * metadata, copy the .bNN segments into the modem carveout, PAS auth_and_reset.
 * Firmware comes from the modem_a partition (FAT, \image\modem.*).
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/IoLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Guid/FileInfo.h>
#include "ModemPas.h"

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

STATIC VOID Out(CONST CHAR8 *Fmt, ...)
{
  CHAR8   a[200];
  CHAR16  w[200];
  VA_LIST ap;

  VA_START (ap, Fmt);
  AsciiVSPrint (a, sizeof (a), Fmt, ap);
  VA_END (ap);
  UnicodeSPrintAsciiFormat (w, sizeof (w), "%a", a);
  gST->ConOut->OutputString (gST->ConOut, w);
}

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

/* Find the volume that has \image\modem.mdt (modem_a, NON-HLOS). */
STATIC EFI_FILE_PROTOCOL *OpenFwDir(VOID)
{
  EFI_HANDLE *h = NULL;
  UINTN n = 0, i;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root, *f;

  /* BDS only connects the boot path: bind PartitionDxe/FatDxe to every block device first. */
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &n, &h))) {
    for (i = 0; i < n; i++) {
      gBS->ConnectController (h[i], NULL, NULL, TRUE);
    }
    Out ("  connected %u block devices\r\n", (UINT32)n);
    FreePool (h);
    h = NULL;
  }
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &n, &h))) {
    Out ("  no file systems at all\r\n");
    return NULL;
  }
  for (i = 0; i < n; i++) {
    if (EFI_ERROR (gBS->HandleProtocol (h[i], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&fs)) ||
        EFI_ERROR (fs->OpenVolume (fs, &root))) {
      continue;
    }
    if (!EFI_ERROR (root->Open (root, &f, L"\\image\\modem.mdt", EFI_FILE_MODE_READ, 0))) {
      f->Close (f);
      Out ("  firmware volume: handle %u of %u\r\n", (UINT32)i, (UINT32)n);
      FreePool (h);
      return root;
    }
    root->Close (root);
  }
  Out ("  \\image\\modem.mdt not found on %u volumes\r\n", (UINT32)n);
  FreePool (h);
  return NULL;
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
#define SMEM_BASE          0x46000000u
#define SMEM_SIZE          0x00200000u
#define SMEM_GLOBAL_HOST   0xFFFE
#define SMEM_ITEM_SMP2P_IN 435
#define SMEM_ITEM_SMP2P_OUT 428
#define SMEM_ITEM_MSS_CRASH 421
#define GICD_BASE          0x0F200000u
#define TCSR_MUTEX_BASE    0x00340000u        /* qcom,tcsr-mutex, stride 0x1000; not in the UEFI map */
#define SMEM_HWLOCK        3
#define HWLOCK_APPS_ID     1                  /* Linux QCOM_MUTEX_APPS_PROC_ID */
#define APCS_IPC           (0x0F111000u + 8)  /* apcs_glb mailbox, Linux offset 8 */
#define MPSS_SMP2P_IRQ_BIT 14                 /* smp2p-modem mboxes = <&apcs_glb 14> */
#define SMP2P_MAGIC        0x504D5324u        /* "$SMP" */
#define SMP2P_MAX_ENTRY    16
#define SMP2P_FEATURES     1                  /* SSR_ACK */
#define MSS_WDOG_INTID     (32 + 0x133)

#pragma pack(1)
typedef struct { UINT32 Offset, Size, Flags; UINT16 Host0, Host1; UINT32 Cacheline, Rsvd[7]; } SMEM_PT_ENTRY;
typedef struct { UINT8 Magic[4]; UINT32 Version, NumEntries, Rsvd[5]; } SMEM_PT;
typedef struct { UINT8 Magic[4]; UINT16 Host0, Host1; UINT32 Size, FreeUncached, FreeCached, Rsvd[3]; } SMEM_PART_HDR;
typedef struct { UINT16 Canary, Item; UINT32 Size; UINT16 PadData, PadHdr; UINT32 Rsvd; } SMEM_PRIV_ENTRY;
typedef struct { UINT32 Allocated, Offset, Size, AuxBase; } SMEM_GLOBAL_ENTRY;
typedef struct { UINT32 Magic; UINT8 Version, Features[3]; UINT16 LocalPid, RemotePid, Total, Valid; UINT32 Flags; } SMP2P_HDR;
typedef struct { CHAR8 Name[16]; UINT32 Value; } SMP2P_ENTRY;
#pragma pack()

STATIC SMEM_PART_HDR *SmemPartition(UINT16 A, UINT16 B)
{
  SMEM_PT *pt = (SMEM_PT *)(UINTN)(SMEM_BASE + SMEM_SIZE - SIZE_4KB);
  SMEM_PT_ENTRY *e = (SMEM_PT_ENTRY *)(pt + 1);
  UINT32 i;

  if (CompareMem (pt->Magic, "$TOC", 4) != 0 || pt->NumEntries > 64) {
    return NULL;
  }
  for (i = 0; i < pt->NumEntries; i++) {
    if (e[i].Offset != 0 && e[i].Size != 0 && e[i].Offset < SMEM_SIZE &&
        ((e[i].Host0 == A && e[i].Host1 == B) || (e[i].Host0 == B && e[i].Host1 == A))) {
      return (SMEM_PART_HDR *)(UINTN)(SMEM_BASE + e[i].Offset);
    }
  }
  return NULL;
}

/* Uncached items of a partition (smem.c qcom_smem_get_private) */
STATIC VOID *SmemPrivGet(SMEM_PART_HDR *P, UINT16 Item, UINT32 *Size)
{
  UINT8 *p = (UINT8 *)P, *e = p + sizeof (*P), *end;
  SMEM_PRIV_ENTRY *h;

  if (P == NULL || CompareMem (P->Magic, "$PRT", 4) != 0 || P->FreeUncached > P->Size) {
    return NULL;
  }
  end = p + P->FreeUncached;
  while (e + sizeof (*h) <= end) {
    h = (SMEM_PRIV_ENTRY *)e;
    if (h->Canary != 0xA5A5) {
      return NULL;
    }
    if (h->Item == Item) {
      if (Size != NULL) {
        *Size = h->Size - h->PadData;
      }
      return e + sizeof (*h) + h->PadHdr;
    }
    e += sizeof (*h) + h->PadHdr + h->Size;
  }
  return NULL;
}

STATIC UINT32 SmemVersion(VOID)
{
  /* smem_header: proc_comm[4] (64 B), version[32]; SBL version is index 7, major in [31:16] */
  return MmioRead32 (SMEM_BASE + 64 + 7 * 4) >> 16;
}

STATIC VOID *SmemGlobalGet(UINT16 Item, UINT32 *Size)
{
  SMEM_GLOBAL_ENTRY *g;

  if (SmemVersion () >= 12) {
    return SmemPrivGet (SmemPartition (SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST), Item, Size);
  }
  /* legacy: toc[] after proc_comm[4], version[32], initialized, available, reserved */
  g = (SMEM_GLOBAL_ENTRY *)(UINTN)(SMEM_BASE + 204) + Item;
  if (Item >= 512 || g->Allocated == 0 || g->Offset >= SMEM_SIZE) {
    return NULL;
  }
  *Size = g->Size;
  return (VOID *)(UINTN)(SMEM_BASE + g->Offset);
}


STATIC BOOLEAN mMutexMapped;

/* Map the TCSR mutex block (device memory) on first use: it is not in the platform memory map. */
STATIC BOOLEAN MapTcsrMutex(VOID)
{
  EFI_STATUS s;

  if (mMutexMapped) {
    return TRUE;
  }
  s = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, TCSR_MUTEX_BASE, SIZE_16KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME);
  Out ("  map TCSR mutex: add %r", s);
  s = gDS->SetMemorySpaceAttributes (TCSR_MUTEX_BASE, SIZE_16KB, EFI_MEMORY_UC);
  Out (", attr %r\r\n", s);
  mMutexMapped = !EFI_ERROR (s);
  return mMutexMapped;
}

STATIC BOOLEAN SmemLock(VOID)
{
  UINTN reg = TCSR_MUTEX_BASE + SMEM_HWLOCK * 0x1000, n;

  for (n = 0; n < 100000; n++) {
    MmioWrite32 (reg, HWLOCK_APPS_ID);
    if (MmioRead32 (reg) == HWLOCK_APPS_ID) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  Out ("  SMEM hwlock busy: owner %u\r\n", MmioRead32 (reg));
  return FALSE;
}

STATIC VOID SmemUnlock(VOID)
{
  MmioWrite32 (TCSR_MUTEX_BASE + SMEM_HWLOCK * 0x1000, 0);
}

/* smem.c qcom_smem_alloc_private(): append an uncached entry under hwlock 3 */
STATIC VOID *SmemPrivAlloc(SMEM_PART_HDR *P, UINT16 Item, UINT32 Size)
{
  SMEM_PRIV_ENTRY *h;
  UINT32 aligned = ALIGN_VALUE (Size, 8), need = sizeof (*h) + aligned;
  VOID *v;

  v = SmemPrivGet (P, Item, NULL);
  if (v != NULL || P == NULL || !MapTcsrMutex () || !SmemLock ()) {
    return v;
  }
  v = SmemPrivGet (P, Item, NULL);
  if (v == NULL && P->FreeUncached + need <= P->FreeCached) {
    h = (SMEM_PRIV_ENTRY *)((UINT8 *)P + P->FreeUncached);
    h->Canary  = 0xA5A5;
    h->Item    = Item;
    h->Size    = aligned;
    h->PadData = (UINT16)(aligned - Size);
    h->PadHdr  = 0;
    h->Rsvd    = 0;
    v = h + 1;
    ZeroMem (v, aligned);
    MemoryFence ();
    P->FreeUncached += need;
    MemoryFence ();
  } else if (v == NULL) {
    Out ("  SMEM partition full: free %x..%x need %x\r\n", P->FreeUncached, P->FreeCached, need);
  }
  SmemUnlock ();
  return v;
}

STATIC VOID Smp2pKick(VOID)
{
  MmioWrite32 (APCS_IPC, 1u << MPSS_SMP2P_IRQ_BIT);
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

STATIC VOID ModemWatch(UINTN Seconds)
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
    Smp2pDump ("apps->modem (428)", out);
  }

  for (t = 0; t <= Seconds * 4; t++) {
    wdog = (MmioRead32 (GICD_BASE + 0x200 + (MSS_WDOG_INTID / 32) * 4) >> (MSS_WDOG_INTID % 32)) & 1;
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
      if (last & 1) {
        break;
      }
    }
    gBS->Stall (250 * 1000);
  }
  if (in != NULL) {
    Smp2pDump ("modem->apps (435)", in);
  } else {
    Out ("  modem SMP2P item 435 never appeared\r\n");
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
  EFI_PHYSICAL_ADDRESS metaPa = 0xFFFFFFFF;
  CHAR16 name[32];
  EFI_STATUS s;

  Out ("\r\n  ==== Wi-Fi P1: modem (MPSS) PAS boot ====\r\n");

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
  s = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, EFI_SIZE_TO_PAGES (metaSize), &metaPa);
  if (EFI_ERROR (s)) {
    Out ("  metadata alloc: %r\r\n", s);
    return s;
  }
  meta = (UINT8 *)(UINTN)metaPa;
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
    dst = (UINT8 *)(UINTN)ph[i].p_paddr;
    sz = 0;
    if (ph[i].p_filesz != 0) {
      UnicodeSPrint (name, sizeof (name), L"\\image\\modem.b%02u", (UINT32)i);
      s = ReadFile (root, name, NULL, &sz, dst, ph[i].p_memsz);
      if (EFI_ERROR (s) || sz != ph[i].p_filesz) {
        Out ("  modem.b%02u: %r (%lu of %u bytes)\r\n", (UINT32)i, s, (UINT64)sz, ph[i].p_filesz);
        return EFI_LOAD_ERROR;
      }
    }
    if (ph[i].p_memsz > sz) {
      ZeroMem (dst + sz, ph[i].p_memsz - sz);
    }
  }
  Out ("  segments loaded\r\n");

  st = Scm (SCM_FN (SCM_SVC_PIL, PIL_AUTH_RESET), SCM_ARGS (1), PAS_ID_MODEM, 0, 0, &res);
  Out ("  PAS auth_and_reset: ret=%lx res=%lx resumes=%lu (0/0 = TZ verified and started the modem)\r\n", (UINT64)st, (UINT64)res, (UINT64)mScmResumes);
  if (st != 0 || res != 0) {
    return EFI_SECURITY_VIOLATION;
  }
  ModemWatch (20);
  return EFI_SUCCESS;
}
