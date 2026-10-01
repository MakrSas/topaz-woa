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
  return (st == 0 && res == 0) ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}
