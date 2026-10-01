/*
 * EFI_FILE_PROTOCOL over ZwCreateFile/ZwReadFile. Root = \??\C:\topaz\fw, names are absolute
 * EFI paths ("\image\modem.mdt"). Read-only. Reads go through a pool bounce buffer, so the
 * destination may be a write-combined mapping (modem carveout, rmtfs buffer).
 */
#include "Modem.h"

#define FW_ROOT     L"\\??\\C:\\topaz\\fw"
#define BOUNCE_SIZE 0x40000

CONST GUID gEfiFileInfoGuid = { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

STATIC EFI_STATUS FOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr);
STATIC EFI_STATUS FClose(EFI_FILE_PROTOCOL *This);
STATIC EFI_STATUS FRead(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf);
STATIC EFI_STATUS FSetPosition(EFI_FILE_PROTOCOL *This, UINT64 Pos);
STATIC EFI_STATUS FGetInfo(EFI_FILE_PROTOCOL *This, CONST GUID *Type, UINTN *Size, VOID *Buf);

STATIC EFI_FILE_PROTOCOL mRoot = { FOpen, FClose, FRead, FSetPosition, FGetInfo, NULL, 0 };

EFI_FILE_PROTOCOL *FwRoot(VOID)
{
  return &mRoot;
}

STATIC EFI_STATUS FOpen(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr)
{
  WCHAR path[260];
  UNICODE_STRING us;
  OBJECT_ATTRIBUTES oa;
  IO_STATUS_BLOCK iosb;
  EFI_FILE_PROTOCOL *f;
  HANDLE h;
  NTSTATUS st;

  UNREFERENCED_PARAMETER (This);
  UNREFERENCED_PARAMETER (Attr);
  if (Mode != EFI_FILE_MODE_READ || Name == NULL || Name[0] != L'\\') {
    return EFI_NOT_FOUND;
  }
  if (!NT_SUCCESS (RtlStringCbPrintfW (path, sizeof (path), L"%s%s", FW_ROOT, Name))) {
    return EFI_NOT_FOUND;
  }
  RtlInitUnicodeString (&us, path);
  InitializeObjectAttributes (&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
  st = ZwCreateFile (&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                     FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
  if (!NT_SUCCESS (st)) {
    return EFI_NOT_FOUND;
  }
  f = AllocateZeroPool (sizeof (*f));
  if (f == NULL) {
    ZwClose (h);
    return EFI_OUT_OF_RESOURCES;
  }
  *f = mRoot;
  f->Handle = h;
  f->Pos    = 0;
  *New = f;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS FClose(EFI_FILE_PROTOCOL *This)
{
  if (This == &mRoot) {
    return EFI_SUCCESS;
  }
  if (This->Handle != NULL) {
    ZwClose (This->Handle);
  }
  FreePool (This);
  return EFI_SUCCESS;
}

/* EFI semantics: reading at/after EOF returns success with *Size = 0. */
STATIC EFI_STATUS FRead(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf)
{
  UINT8 *bounce, *dst = Buf;
  UINTN want = *Size, got = 0;
  IO_STATUS_BLOCK iosb;
  LARGE_INTEGER off;
  NTSTATUS st = STATUS_SUCCESS;

  *Size = 0;
  if (This->Handle == NULL) {
    return EFI_DEVICE_ERROR;
  }
  bounce = AllocatePool (BOUNCE_SIZE);
  if (bounce == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  while (got < want) {
    ULONG chunk = (ULONG)MIN (want - got, (UINTN)BOUNCE_SIZE);
    off.QuadPart = (LONGLONG)(This->Pos + got);
    st = ZwReadFile (This->Handle, NULL, NULL, NULL, &iosb, bounce, chunk, &off, NULL);
    if (st == STATUS_END_OF_FILE) {
      st = STATUS_SUCCESS;
      break;
    }
    if (!NT_SUCCESS (st)) {
      break;
    }
    CopyMem (dst + got, bounce, iosb.Information);
    got += iosb.Information;
    if (iosb.Information < chunk) {
      break;
    }
  }
  FreePool (bounce);
  This->Pos += got;
  *Size = got;
  return NT_SUCCESS (st) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS FSetPosition(EFI_FILE_PROTOCOL *This, UINT64 Pos)
{
  This->Pos = Pos;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS FGetInfo(EFI_FILE_PROTOCOL *This, CONST GUID *Type, UINTN *Size, VOID *Buf)
{
  FILE_STANDARD_INFORMATION fsi;
  IO_STATUS_BLOCK iosb;
  EFI_FILE_INFO *info = Buf;

  UNREFERENCED_PARAMETER (Type);
  if (*Size < sizeof (EFI_FILE_INFO)) {
    *Size = sizeof (EFI_FILE_INFO);
    return EFI_BUFFER_TOO_SMALL;
  }
  if (This->Handle == NULL ||
      !NT_SUCCESS (ZwQueryInformationFile (This->Handle, &iosb, &fsi, sizeof (fsi), FileStandardInformation))) {
    return EFI_DEVICE_ERROR;
  }
  ZeroMem (info, sizeof (*info));
  info->Size         = sizeof (*info);
  info->FileSize     = (UINT64)fsi.EndOfFile.QuadPart;
  info->PhysicalSize = (UINT64)fsi.AllocationSize.QuadPart;
  return EFI_SUCCESS;
}
