#pragma once
#include <Uefi.h>

#define AB_PRIO(a)    ((UINT32)RShiftU64 ((a), 48) & 3)
#define AB_ACTIVE(a)  ((UINT32)RShiftU64 ((a), 50) & 1)
#define AB_RETRY(a)   ((UINT32)RShiftU64 ((a), 51) & 7)
#define AB_SUCCESS(a) ((UINT32)RShiftU64 ((a), 54) & 1)
#define AB_UNBOOT(a)  ((UINT32)RShiftU64 ((a), 55) & 1)

typedef struct {
  BOOLEAN    FoundA, FoundB;
  UINT64     AttrA, AttrB;
  UINT32     IndexA, IndexB;
  UINT32     Entries, EntrySize, BlockSize;
  EFI_HANDLE Handle;
} AB_SLOT_INFO;

EFI_STATUS AbSlotRead(OUT AB_SLOT_INFO *Info);

#define AB_PRIO_MASK   LShiftU64 (3, 48)
#define AB_ACTIVE_BIT  LShiftU64 (1, 50)
#define AB_RETRY_MASK  LShiftU64 (7, 51)
#define AB_SUCCESS_BIT LShiftU64 (1, 54)
#define AB_UNBOOT_BIT  LShiftU64 (1, 55)

EFI_STATUS AbSlotWrite(IN CONST AB_SLOT_INFO *Info, IN UINT64 NewA, IN UINT64 NewB);
EFI_STATUS AbSlotSetActive(IN OUT AB_SLOT_INFO *Info, IN BOOLEAN SlotB);
EFI_STATUS AbSlotMarkBSuccessful(IN OUT AB_SLOT_INFO *Info);
EFI_STATUS AbSlotRequestA(IN OUT AB_SLOT_INFO *Info);
