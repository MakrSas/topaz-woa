#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include <ntstrsafe.h>

#include "hw.h"

#define TOPAZ_POOL_TAG 'zpoT'

/* ---- Mapped I/O ---------------------------------------------------------- */

typedef struct _MMIO_RANGE {
    PHYSICAL_ADDRESS Pa;
    SIZE_T           Size;
    volatile UCHAR  *Va;
} MMIO_RANGE, *PMMIO_RANGE;

NTSTATUS MmioMap(_Inout_ PMMIO_RANGE Range, _In_ ULONGLONG Pa, _In_ SIZE_T Size);
VOID     MmioUnmap(_Inout_ PMMIO_RANGE Range);

__forceinline ULONG MmioRead32(_In_ PMMIO_RANGE R, _In_ ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(R->Va + Off));
}

__forceinline VOID MmioWrite32(_In_ PMMIO_RANGE R, _In_ ULONG Off, _In_ ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(R->Va + Off), Val);
}

/* ---- TLMM ---------------------------------------------------------------- */

typedef struct _TLMM_PIN {
    ULONG      Pin;
    MMIO_RANGE Regs;
} TLMM_PIN, *PTLMM_PIN;

NTSTATUS TlmmPinMap(_Out_ PTLMM_PIN P, _In_ ULONGLONG Tile, _In_ ULONG Pin);
VOID     TlmmPinUnmap(_Inout_ PTLMM_PIN P);
VOID     TlmmConfig(_In_ PTLMM_PIN P, _In_ ULONG Func, _In_ ULONG Pull, _In_ ULONG DriveMa, _In_ BOOLEAN Output);
VOID     TlmmSetOutput(_In_ PTLMM_PIN P, _In_ BOOLEAN High);
BOOLEAN  TlmmGetInput(_In_ PTLMM_PIN P);

/* ---- GENI I2C (FIFO mode, polled) ----------------------------------------- */

typedef struct _GENI_I2C {
    MMIO_RANGE Se;
    ULONG      TxDepth;
    ULONG      Proto;
    ULONG      LastIrqStatus;
} GENI_I2C, *PGENI_I2C;

NTSTATUS GeniI2cInit(_Inout_ PGENI_I2C Bus, _In_ ULONGLONG SeBase);
VOID     GeniI2cDeinit(_Inout_ PGENI_I2C Bus);
NTSTATUS GeniI2cWrite(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len, _In_ BOOLEAN Stop);
NTSTATUS GeniI2cRead(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS GeniI2cReadReg(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);

/* ---- GCC ----------------------------------------------------------------- */

NTSTATUS GccEnableQup0Se2(VOID);

/* ---- Log file (C:\TopazTouch.log) ---------------------------------------- */

VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);

/* ---- Device -------------------------------------------------------------- */

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE    Device;
    VHFHANDLE    Vhf;
    BOOLEAN      VhfStarted;

    GENI_I2C     Bus;
    TLMM_PIN     PinSda, PinScl, PinIrq, PinReset, PinAvdd;
    BOOLEAN      HwReady;

    HANDLE       ThreadHandle;
    PKTHREAD     Thread;
    KEVENT       StopEvent;

    ULONG        LogicalMaxX, LogicalMaxY;
    ULONG        RawLogged;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

EVT_WDF_DRIVER_DEVICE_ADD        TopazEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  TopazEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  TopazEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY          TopazEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT           TopazEvtD0Exit;

NTSTATUS TouchHwInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchHwDeinit(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS TouchVhfCreate(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchVhfDelete(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS TouchThreadStart(_In_ PDEVICE_CONTEXT Ctx);
VOID     TouchThreadStop(_In_ PDEVICE_CONTEXT Ctx);
