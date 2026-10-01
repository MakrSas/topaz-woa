/*
 * A/B slot attributes of boot_a/boot_b, read straight from the GPT of every UFS LUN.
 * Qualcomm layout (ABL PartitionTableUpdate.h): priority 49:48, active 50, retry 53:51,
 * successful 54, unbootable 55. Read-only for now.
 */
#include <Uefi.h>
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include "AbSlot.h"

STATIC EFI_STATUS ReadBlocks(EFI_BLOCK_IO_PROTOCOL *Bio, EFI_LBA Lba, UINTN Bytes, VOID **Out)
{
  UINTN bs = Bio->Media->BlockSize;
  UINTN n  = ALIGN_VALUE (Bytes, bs);
  VOID *buf = AllocateZeroPool (n);
  EFI_STATUS s;

  if (buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  s = Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lba, n, buf);
  if (EFI_ERROR (s)) {
    FreePool (buf);
    return s;
  }
  *Out = buf;
  return EFI_SUCCESS;
}

EFI_STATUS AbSlotRead(OUT AB_SLOT_INFO *Info)
{
  EFI_HANDLE *handles = NULL;
  UINTN count = 0, h, i;
  EFI_STATUS s;

  ZeroMem (Info, sizeof (*Info));
  s = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &count, &handles);
  if (EFI_ERROR (s)) {
    return s;
  }
  for (h = 0; h < count && !(Info->FoundA && Info->FoundB); h++) {
    EFI_BLOCK_IO_PROTOCOL *bio;
    EFI_PARTITION_TABLE_HEADER *hdr = NULL;
    UINT8 *ents = NULL;

    if (EFI_ERROR (gBS->HandleProtocol (handles[h], &gEfiBlockIoProtocolGuid, (VOID **)&bio)) ||
        bio->Media->LogicalPartition || !bio->Media->MediaPresent) {
      continue;
    }
    if (EFI_ERROR (ReadBlocks (bio, 1, sizeof (*hdr), (VOID **)&hdr))) {
      continue;
    }
    if (hdr->Header.Signature == EFI_PTAB_HEADER_ID && hdr->SizeOfPartitionEntry >= sizeof (EFI_PARTITION_ENTRY) &&
        hdr->NumberOfPartitionEntries <= 256 &&
        !EFI_ERROR (ReadBlocks (bio, hdr->PartitionEntryLBA,
                                hdr->NumberOfPartitionEntries * hdr->SizeOfPartitionEntry, (VOID **)&ents))) {
      for (i = 0; i < hdr->NumberOfPartitionEntries; i++) {
        EFI_PARTITION_ENTRY *e = (EFI_PARTITION_ENTRY *)(ents + i * hdr->SizeOfPartitionEntry);
        if (StrCmp (e->PartitionName, L"boot_a") == 0) {
          Info->FoundA = TRUE; Info->AttrA = e->Attributes; Info->IndexA = (UINT32)i; Info->Handle = handles[h];
        } else if (StrCmp (e->PartitionName, L"boot_b") == 0) {
          Info->FoundB = TRUE; Info->AttrB = e->Attributes; Info->IndexB = (UINT32)i; Info->Handle = handles[h];
        }
      }
      Info->Entries = hdr->NumberOfPartitionEntries;
      Info->EntrySize = hdr->SizeOfPartitionEntry;
      Info->BlockSize = bio->Media->BlockSize;
      FreePool (ents);
    }
    FreePool (hdr);
  }
  FreePool (handles);
  return (Info->FoundA && Info->FoundB) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

/* ---- write ------------------------------------------------------------------ */

STATIC UINT32 Crc32(VOID *Buf, UINTN Len)
{
  UINT32 crc = 0;
  gBS->CalculateCrc32 (Buf, Len, &crc);
  return crc;
}

/*
 * Update the attributes of entries IdxA/IdxB in one GPT copy (header at HdrLba).
 * Refuses to write unless the existing header and entry-array CRCs verify with our
 * own CRC code, and re-verifies after writing.
 */
STATIC EFI_STATUS PatchCopy(EFI_BLOCK_IO_PROTOCOL *Bio, EFI_LBA HdrLba, CONST AB_SLOT_INFO *Info,
                            UINT64 NewA, UINT64 NewB, OUT EFI_LBA *AltLba)
{
  EFI_PARTITION_TABLE_HEADER *hdr = NULL;
  UINT8 *ents = NULL;
  UINTN  entBytes, bs = Bio->Media->BlockSize;
  UINT32 crc;
  EFI_PARTITION_ENTRY *ea, *eb;
  EFI_STATUS s;

  s = ReadBlocks (Bio, HdrLba, sizeof (*hdr), (VOID **)&hdr);
  if (EFI_ERROR (s)) {
    return s;
  }
  s = EFI_VOLUME_CORRUPTED;
  if (hdr->Header.Signature != EFI_PTAB_HEADER_ID || hdr->Header.HeaderSize > bs ||
      hdr->NumberOfPartitionEntries != Info->Entries || hdr->SizeOfPartitionEntry != Info->EntrySize) {
    goto out;
  }
  crc = hdr->Header.CRC32;
  hdr->Header.CRC32 = 0;
  if (Crc32 (hdr, hdr->Header.HeaderSize) != crc) {
    goto out;
  }
  entBytes = (UINTN)Info->Entries * Info->EntrySize;
  if (EFI_ERROR (ReadBlocks (Bio, hdr->PartitionEntryLBA, entBytes, (VOID **)&ents))) {
    goto out;
  }
  if (Crc32 (ents, entBytes) != hdr->PartitionEntryArrayCRC32) {
    goto out;
  }
  ea = (EFI_PARTITION_ENTRY *)(ents + (UINTN)Info->IndexA * Info->EntrySize);
  eb = (EFI_PARTITION_ENTRY *)(ents + (UINTN)Info->IndexB * Info->EntrySize);
  if (StrCmp (ea->PartitionName, L"boot_a") != 0 || StrCmp (eb->PartitionName, L"boot_b") != 0) {
    goto out;
  }
  ea->Attributes = NewA;
  eb->Attributes = NewB;
  hdr->PartitionEntryArrayCRC32 = Crc32 (ents, entBytes);
  hdr->Header.CRC32 = 0;
  hdr->Header.CRC32 = Crc32 (hdr, hdr->Header.HeaderSize);

  s = Bio->WriteBlocks (Bio, Bio->Media->MediaId, hdr->PartitionEntryLBA, ALIGN_VALUE (entBytes, bs), ents);
  if (!EFI_ERROR (s)) {
    s = Bio->WriteBlocks (Bio, Bio->Media->MediaId, HdrLba, bs, hdr);
  }
  if (!EFI_ERROR (s)) {
    Bio->FlushBlocks (Bio);
  }
  if (AltLba != NULL) {
    *AltLba = hdr->AlternateLBA;
  }
out:
  if (ents != NULL) {
    FreePool (ents);
  }
  FreePool (hdr);
  return s;
}

/* Verify one copy: header + array CRC and the two attribute values. */
STATIC EFI_STATUS VerifyCopy(EFI_BLOCK_IO_PROTOCOL *Bio, EFI_LBA HdrLba, CONST AB_SLOT_INFO *Info, UINT64 A, UINT64 B)
{
  EFI_PARTITION_TABLE_HEADER *hdr = NULL;
  UINT8 *ents = NULL;
  UINT32 crc;
  EFI_STATUS s = EFI_VOLUME_CORRUPTED;

  if (EFI_ERROR (ReadBlocks (Bio, HdrLba, sizeof (*hdr), (VOID **)&hdr))) {
    return EFI_DEVICE_ERROR;
  }
  crc = hdr->Header.CRC32;
  hdr->Header.CRC32 = 0;
  if (Crc32 (hdr, hdr->Header.HeaderSize) == crc &&
      !EFI_ERROR (ReadBlocks (Bio, hdr->PartitionEntryLBA, (UINTN)Info->Entries * Info->EntrySize, (VOID **)&ents))) {
    if (Crc32 (ents, (UINTN)Info->Entries * Info->EntrySize) == hdr->PartitionEntryArrayCRC32 &&
        ((EFI_PARTITION_ENTRY *)(ents + (UINTN)Info->IndexA * Info->EntrySize))->Attributes == A &&
        ((EFI_PARTITION_ENTRY *)(ents + (UINTN)Info->IndexB * Info->EntrySize))->Attributes == B) {
      s = EFI_SUCCESS;
    }
    FreePool (ents);
  }
  FreePool (hdr);
  return s;
}

EFI_STATUS AbSlotWrite(IN CONST AB_SLOT_INFO *Info, IN UINT64 NewA, IN UINT64 NewB)
{
  EFI_BLOCK_IO_PROTOCOL *bio;
  EFI_LBA alt = 0;
  EFI_STATUS s;

  s = gBS->HandleProtocol (Info->Handle, &gEfiBlockIoProtocolGuid, (VOID **)&bio);
  if (EFI_ERROR (s) || bio->Media->ReadOnly) {
    return EFI_WRITE_PROTECTED;
  }
  s = PatchCopy (bio, 1, Info, NewA, NewB, &alt);                 /* primary */
  if (EFI_ERROR (s)) {
    return s;
  }
  if (alt == 0 || alt > bio->Media->LastBlock) {
    return EFI_VOLUME_CORRUPTED;
  }
  s = PatchCopy (bio, alt, Info, NewA, NewB, NULL);                /* backup */
  if (EFI_ERROR (s)) {
    return s;
  }
  s = VerifyCopy (bio, 1, Info, NewA, NewB);
  if (!EFI_ERROR (s)) {
    s = VerifyCopy (bio, alt, Info, NewA, NewB);
  }
  return s;
}

/* Same bit changes as ABL MarkPtnActive(): target max priority/retry, active, clear
 * successful+unbootable; the other slot loses active and drops to priority 2. */
EFI_STATUS AbSlotSetActive(IN OUT AB_SLOT_INFO *Info, IN BOOLEAN SlotB)
{
  UINT64 on  = AB_PRIO_MASK | AB_ACTIVE_BIT | AB_RETRY_MASK;
  UINT64 clr = AB_SUCCESS_BIT | AB_UNBOOT_BIT;
  UINT64 a = Info->AttrA, b = Info->AttrB;
  EFI_STATUS s;

  if (SlotB) {
    b = (b | on) & ~clr;
    a = (a & ~(AB_PRIO_MASK | AB_ACTIVE_BIT)) | LShiftU64 (2, 48);
  } else {
    a = (a | on) & ~clr;
    b = (b & ~(AB_PRIO_MASK | AB_ACTIVE_BIT)) | LShiftU64 (2, 48);
  }
  s = AbSlotWrite (Info, a, b);
  if (!EFI_ERROR (s)) {
    Info->AttrA = a;
    Info->AttrB = b;
  }
  return s;
}

/* Mark slot b successful (what Android's markBootSuccessful does for its slot). */
EFI_STATUS AbSlotMarkBSuccessful(IN OUT AB_SLOT_INFO *Info)
{
  EFI_STATUS s;
  UINT64 b = Info->AttrB | AB_SUCCESS_BIT;

  if (b == Info->AttrB) {
    return EFI_SUCCESS;
  }
  s = AbSlotWrite (Info, Info->AttrA, b);
  if (!EFI_ERROR (s)) {
    Info->AttrB = b;
  }
  return s;
}

/*
 * Ask ABL to switch to slot a by itself: mark b unbootable (retry 0, not successful) and
 * make a bootable, attributes only. On the next boot ABL's FindBootableSlot() sees the
 * current slot b unbootable and runs its own SetActiveSlot(a), which also swaps the
 * partition type GUIDs of all _a/_b pairs (we never touch GUIDs ourselves: switching only
 * the active bit leaves the GUIDs on the old slot and ABL can't boot anything).
 */
EFI_STATUS AbSlotRequestA(IN OUT AB_SLOT_INFO *Info)
{
  UINT64 a = (Info->AttrA | AB_RETRY_MASK) & ~(AB_UNBOOT_BIT | AB_SUCCESS_BIT);
  UINT64 b = (Info->AttrB | AB_UNBOOT_BIT) & ~(AB_RETRY_MASK | AB_SUCCESS_BIT);
  EFI_STATUS s = AbSlotWrite (Info, a, b);

  if (!EFI_ERROR (s)) {
    Info->AttrA = a;
    Info->AttrB = b;
  }
  return s;
}
