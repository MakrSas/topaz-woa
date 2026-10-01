/*
 * The EDK2 bits the modem code uses, on top of the Windows kernel (see compat.h).
 */
#include "Modem.h"

volatile BOOLEAN gModemStop;
UINTN gSmemVa, gApcsVa, gGicdVa;

/* ---------------- pool ---------------- */

VOID *AllocatePool(UINTN Size)
{
  return ExAllocatePool2 (POOL_FLAG_NON_PAGED | POOL_FLAG_UNINITIALIZED, Size ? Size : 1, TOPAZ_POOL_TAG);
}

VOID *AllocateZeroPool(UINTN Size)
{
  return ExAllocatePool2 (POOL_FLAG_NON_PAGED, Size ? Size : 1, TOPAZ_POOL_TAG);
}

VOID *AllocateCopyPool(UINTN Size, CONST VOID *Src)
{
  VOID *p = AllocatePool (Size);

  if (p != NULL) {
    CopyMem (p, Src, Size);
  }
  return p;
}

VOID FreePool(VOID *P)
{
  if (P != NULL) {
    ExFreePoolWithTag (P, TOPAZ_POOL_TAG);
  }
}

VOID *AllocatePages(UINTN Pages)
{
  return AllocatePool (Pages * SIZE_4KB);
}

VOID FreePages(VOID *P, UINTN Pages)
{
  UNREFERENCED_PARAMETER (Pages);
  FreePool (P);
}

/* ---------------- physical memory ---------------- */

/*
 * Buffers TrustZone / the modem read by physical address: contiguous, below 4 GiB,
 * write-combined (= Normal non-cacheable on ARM64: no cache maintenance, unaligned access ok).
 */
#define PHYS_MAX 16
STATIC struct { UINT8 *Va; UINT64 Pa; UINTN Size; } mPhys[PHYS_MAX];

VOID *PhysAlloc(UINTN Size, UINT64 *Pa)
{
  PHYSICAL_ADDRESS lo, hi, bound;
  UINT8 *va;
  UINTN i;

  lo.QuadPart = 0;
  /* the modem computes end = addr + size in 32 bits: a buffer ending at 4 GiB (0xFFD00000 +
     3 MiB in v0.2) wraps to 0 and rmtfs dies with "xpu lock failed". Stay well below. */
  hi.QuadPart = 0xEFFFFFFF;
  bound.QuadPart = 0;
  Size = ALIGN_VALUE (Size, SIZE_4KB);
  for (i = 0; i < PHYS_MAX && mPhys[i].Va != NULL; i++) {
  }
  if (i == PHYS_MAX) {
    return NULL;
  }
  va = MmAllocateContiguousMemorySpecifyCache (Size, lo, hi, bound, MmWriteCombined);
  if (va == NULL) {
    return NULL;
  }
  RtlZeroMemory (va, Size);
  mPhys[i].Va   = va;
  mPhys[i].Pa   = (UINT64)MmGetPhysicalAddress (va).QuadPart;
  mPhys[i].Size = Size;
  *Pa = mPhys[i].Pa;
  return va;
}

VOID PhysFree(VOID *Va)
{
  UINTN i;

  for (i = 0; i < PHYS_MAX; i++) {
    if (mPhys[i].Va == Va && Va != NULL) {
      MmFreeContiguousMemorySpecifyCache (Va, mPhys[i].Size, MmWriteCombined);
      mPhys[i].Va = NULL;
      return;
    }
  }
}

VOID *PaToVa(UINT64 Pa)
{
  UINTN i;

  for (i = 0; i < PHYS_MAX; i++) {
    if (mPhys[i].Va != NULL && Pa >= mPhys[i].Pa && Pa < mPhys[i].Pa + mPhys[i].Size) {
      return mPhys[i].Va + (Pa - mPhys[i].Pa);
    }
  }
  return NULL;
}

VOID *MapPhys(UINT64 Pa, UINTN Size, BOOLEAN Wc)
{
  PHYSICAL_ADDRESS p;

  p.QuadPart = (LONGLONG)Pa;
  return MmMapIoSpaceEx (p, Size, PAGE_READWRITE | (Wc ? PAGE_WRITECOMBINE : PAGE_NOCACHE));
}

VOID UnmapPhys(VOID *Va, UINTN Size)
{
  if (Va != NULL) {
    MmUnmapIoSpace (Va, Size);
  }
}

EFI_STATUS ModemMapInit(VOID)
{
  if (gSmemVa == 0) {
    gSmemVa = (UINTN)MapPhys (SMEM_PA, SMEM_SIZE, TRUE);
  }
  if (gApcsVa == 0) {
    gApcsVa = (UINTN)MapPhys (APCS_PA, SIZE_4KB, FALSE);
  }
  if (gGicdVa == 0) {
    gGicdVa = (UINTN)MapPhys (GICD_PA, SIZE_4KB, FALSE);
  }
  ModemOut ("  map: SMEM %p APCS %p GICD %p\r\n", (VOID *)gSmemVa, (VOID *)gApcsVa, (VOID *)gGicdVa);
  return (gSmemVa != 0 && gApcsVa != 0 && gGicdVa != 0) ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

/* ---------------- strings ---------------- */

UINTN AsciiStrDecimalToUintn(CONST CHAR8 *S)
{
  UINTN v = 0;

  while (*S == ' ') {
    S++;
  }
  for (; *S >= '0' && *S <= '9'; S++) {
    v = v * 10 + (UINTN)(*S - '0');
  }
  return v;
}

UINTN UnicodeSPrint(CHAR16 *Buf, UINTN Size, CONST CHAR16 *Fmt, ...)
{
  va_list ap;
  size_t n = 0;

  va_start (ap, Fmt);
  RtlStringCbVPrintfW (Buf, Size, Fmt, ap);
  va_end (ap);
  RtlStringCbLengthW (Buf, Size, &n);
  return n / sizeof (CHAR16);
}

/* AsciiSPrint() and the EDK2 -> MSVC format translation live in log.c */

/* ---------------- time ---------------- */

UINT64 GetPerformanceCounter(VOID)
{
  ULONG64 qpc;                                   /* out param is mandatory: NULL bugchecked v0.1 */

  return KeQueryInterruptTimePrecise (&qpc);     /* 100 ns units */
}

UINT64 GetTimeInNanoSecond(UINT64 Ticks)
{
  return Ticks * 100;
}

/* ---------------- gBS ---------------- */

STATIC VOID BsStall(UINTN Us)
{
  LARGE_INTEGER t;

  if (Us >= 2000) {                              /* long waits: sleep, don't spin */
    t.QuadPart = -(LONGLONG)Us * 10;
    KeDelayExecutionThread (KernelMode, FALSE, &t);
    return;
  }
  KeStallExecutionProcessor ((ULONG)Us);
}

STATIC EFI_STATUS BsCrc32(VOID *Data, UINTN Size, UINT32 *Crc)
{
  CONST UINT8 *p = Data;
  UINT32 c = 0xFFFFFFFF, k;

  while (Size-- != 0) {
    c ^= *p++;
    for (k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
  }
  *Crc = ~c;
  return EFI_SUCCESS;
}

STATIC TOPAZ_BS mBs = { BsStall, BsCrc32 };
TOPAZ_BS *gBS = &mBs;

/* Polling loop pacing: the modem has no interrupt to us, so poll every ~1 ms when idle. */
VOID ModemIdle(BOOLEAN Busy)
{
  STATIC UINTN busyRun;
  LARGE_INTEGER t;

  if (Busy && busyRun++ < 200) {
    KeStallExecutionProcessor (20);
    return;
  }
  busyRun = 0;
  LogFlush ();
  t.QuadPart = -10000;                           /* 1 ms (ExSetTimerResolution in driver.c) */
  KeDelayExecutionThread (KernelMode, FALSE, &t);
}
