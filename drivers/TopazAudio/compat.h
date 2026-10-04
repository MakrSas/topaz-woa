/*
 * Just enough of the UEFI/EDK2 API on top of the Windows kernel so the modem code from
 * uefi/TopazOtgDxe (Smem.c, Glink.c, ModemSvc.c, Wlfw.c) builds here nearly unchanged.
 * Physical addresses are NOT identity-mapped here: use the gSmemVa/... mappings and PaToVa().
 */
#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>

#pragma warning(disable: 4201 4214 4100 4189 4244 4245 4267 4389 4706 4127 4057 4152)

typedef ULONG_PTR  UINTN;
typedef LONG_PTR   INTN;
typedef CHAR       CHAR8;
typedef WCHAR      CHAR16;
typedef INTN       EFI_STATUS;
typedef UINT64     EFI_PHYSICAL_ADDRESS;

#ifndef STATIC
#define STATIC static
#endif

#define EFIERR(n)              ((EFI_STATUS)(0x8000000000000000ULL | (n)))
#define EFI_SUCCESS            0
#define EFI_LOAD_ERROR         EFIERR (1)
#define EFI_BUFFER_TOO_SMALL   EFIERR (5)
#define EFI_DEVICE_ERROR       EFIERR (7)
#define EFI_OUT_OF_RESOURCES   EFIERR (9)
#define EFI_NOT_FOUND          EFIERR (14)
#define EFI_SECURITY_VIOLATION EFIERR (26)
#define EFI_ABORTED            EFIERR (21)
#define EFI_ALREADY_STARTED    EFIERR (20)
#define EFI_ERROR(s)           (((INTN)(s)) < 0)

#define MIN(a, b)              (((a) < (b)) ? (a) : (b))
#define MAX(a, b)              (((a) > (b)) ? (a) : (b))
#define ARRAY_SIZE(a)          (sizeof (a) / sizeof ((a)[0]))
#define ALIGN_VALUE(v, a)      (((v) + ((a) - 1)) & ~((a) - 1))
#define SIZE_4KB               0x00001000
#define SIZE_16KB              0x00004000
#define SIZE_4MB               0x00400000
#define MAX_UINT32             0xFFFFFFFFu
#define EFI_SIZE_TO_PAGES(s)   (((s) + 0xFFF) >> 12)

#define TOPAZ_POOL_TAG         'duoT'

/* ---- memory ---- */
#define CopyMem(d, s, n)       RtlCopyMemory ((d), (s), (n))
#define ZeroMem(d, n)          RtlZeroMemory ((d), (n))
#define CompareMem(a, b, n)    ((RtlCompareMemory ((a), (b), (n)) == (SIZE_T)(n)) ? 0 : 1)
#define MemoryFence()          KeMemoryBarrier ()
#define MmioRead32(a)          READ_REGISTER_ULONG ((volatile ULONG *)(UINTN)(a))
#define MmioWrite32(a, v)      WRITE_REGISTER_ULONG ((volatile ULONG *)(UINTN)(a), (ULONG)(v))
#define WriteBackDataCacheRange(p, n)            /* buffers shared with the modem are non-cached */
#define WriteBackInvalidateDataCacheRange(p, n)

VOID *AllocatePool(UINTN Size);
VOID *AllocateZeroPool(UINTN Size);
VOID *AllocateCopyPool(UINTN Size, CONST VOID *Src);
VOID  FreePool(VOID *P);
VOID *AllocatePages(UINTN Pages);
VOID  FreePages(VOID *P, UINTN Pages);

/* Physically contiguous, non-cached memory below 4 GiB for TrustZone / the modem. */
VOID *PhysAlloc(UINTN Size, UINT64 *Pa);
VOID  PhysFree(VOID *Va);
VOID *PaToVa(UINT64 Pa);                         /* only for PhysAlloc() buffers */

/* ---- strings ---- */
#define AsciiStrLen(s)               strlen (s)
#define AsciiStrCmp(a, b)            strcmp ((a), (b))
#define AsciiStrnCmp(a, b, n)        strncmp ((a), (b), (n))
#define AsciiStriCmp(a, b)           _stricmp ((a), (b))
#define AsciiStrCpyS(d, n, s)        RtlStringCbCopyA ((d), (n), (s))
#define AsciiStrnCpyS(d, n, s, c)    RtlStringCbCopyNA ((d), (n), (s), (c))
#define StrLen(s)                    wcslen (s)
#define StrCmp(a, b)                 wcscmp ((a), (b))
UINTN AsciiStrDecimalToUintn(CONST CHAR8 *S);
UINTN AsciiSPrint(CHAR8 *Buf, UINTN Size, CONST CHAR8 *Fmt, ...);   /* EDK2 format (%a, %lu, %r) */
UINTN UnicodeSPrint(CHAR16 *Buf, UINTN Size, CONST CHAR16 *Fmt, ...); /* plain %u/%x only, Size in bytes */

/* ---- time ---- */
UINT64 GetPerformanceCounter(VOID);
UINT64 GetTimeInNanoSecond(UINT64 Ticks);

/* ---- a tiny gBS ---- */
typedef struct {
  VOID       (*Stall)(UINTN Microseconds);
  EFI_STATUS (*CalculateCrc32)(VOID *Data, UINTN Size, UINT32 *Crc);
} TOPAZ_BS;
extern TOPAZ_BS *gBS;

/* ---- files: EFI_FILE_PROTOCOL emulation, root = \??\C:\topaz\fw ---- */
#define EFI_FILE_MODE_READ     1
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct _EFI_FILE_PROTOCOL {
  EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **New, CHAR16 *Name, UINT64 Mode, UINT64 Attr);
  EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *This);
  EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *This, UINTN *Size, VOID *Buf);
  EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Pos);
  EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *This, CONST GUID *Type, UINTN *Size, VOID *Buf);
  HANDLE      Handle;
  UINT64      Pos;
};
typedef struct {
  UINT64 Size, FileSize, PhysicalSize, Attribute;
} EFI_FILE_INFO;
#define SIZE_OF_EFI_FILE_INFO  sizeof (EFI_FILE_INFO)
extern CONST GUID gEfiFileInfoGuid;
EFI_FILE_PROTOCOL *FwRoot(VOID);

/* ---- SMC ---- */
typedef struct {
  UINTN Arg0, Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7;
} ARM_SMC_ARGS;
VOID TopazArmCallSmc(ARM_SMC_ARGS *Args);       /* smc.asm */
#define ArmCallSmc(a)          TopazArmCallSmc (a)

/* ---- log ---- */
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(PCSTR Fmt, ...);                  /* MSVC format, unbuffered */
