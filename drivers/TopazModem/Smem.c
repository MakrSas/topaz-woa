/*
 * Minimal Qualcomm SMEM (v12, partitioned) for the Wi-Fi bring-up spike, after Linux
 * drivers/soc/qcom/smem.c: find items, allocate uncached items under TCSR hwlock 3.
 * Windows copy of uefi/TopazOtgDxe/Smem.c (SMEM_BASE is the VA from ModemMapInit()).
 */
#include "Modem.h"

#define Out ModemOut
#define TCSR_MUTEX_BASE    0x00340000u        /* qcom,tcsr-mutex, stride 0x1000 */
#define SMEM_HWLOCK        3
#define HWLOCK_APPS_ID     1                  /* Linux QCOM_MUTEX_APPS_PROC_ID */

SMEM_PART_HDR *SmemPartition(UINT16 A, UINT16 B)
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
VOID *SmemPrivGet(SMEM_PART_HDR *P, UINT16 Item, UINT32 *Size)
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

UINT32 SmemVersion(VOID)
{
  /* smem_header: proc_comm[4] (64 B), version[32]; SBL version is index 7, major in [31:16] */
  return MmioRead32 (SMEM_BASE + 64 + 7 * 4) >> 16;
}

VOID *SmemGlobalGet(UINT16 Item, UINT32 *Size)
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


STATIC UINTN mMutexVa;

/* Map the TCSR mutex block (device memory) on first use. */
STATIC BOOLEAN MapTcsrMutex(VOID)
{
  if (mMutexVa == 0) {
    mMutexVa = (UINTN)MapPhys (TCSR_MUTEX_BASE, SIZE_16KB, FALSE);
    Out ("  map TCSR mutex: %p\r\n", (VOID *)mMutexVa);
  }
  return mMutexVa != 0;
}

STATIC BOOLEAN SmemLock(VOID)
{
  UINTN reg = mMutexVa + SMEM_HWLOCK * 0x1000, n;

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
  MmioWrite32 (mMutexVa + SMEM_HWLOCK * 0x1000, 0);
}

/* smem.c qcom_smem_alloc_private(): append an uncached entry under hwlock 3 */
VOID *SmemPrivAlloc(SMEM_PART_HDR *P, UINT16 Item, UINT32 Size)
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

VOID ApcsKick(UINT32 Bit)
{
  MmioWrite32 (APCS_IPC, 1u << Bit);
}
