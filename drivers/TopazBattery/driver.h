#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <batclass.h>
#include <ntstrsafe.h>

#include "hw.h"

#define TOPAZ_POOL_TAG 'bpoT'
#define TOPAZ_BATTERY_TAG 1

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

NTSTATUS GccEnableQup0Se1(_Out_ PULONG ClkSel);
NTSTATUS GeniI2cInit(_Inout_ PGENI_I2C Bus, _In_ ULONGLONG SeBase, _In_ ULONG ClkSel);
VOID     GeniI2cDeinit(_Inout_ PGENI_I2C Bus);
NTSTATUS GeniI2cWrite(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len, _In_ BOOLEAN Stop);
NTSTATUS GeniI2cRead(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS GeniI2cReadReg(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_writes_(Len) UCHAR *Buf, _In_ ULONG Len);
NTSTATUS I2cReadWord(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_ PUSHORT Val);
NTSTATUS I2cReadByte(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _Out_ PUCHAR Val);
NTSTATUS I2cWriteByte(_In_ PGENI_I2C Bus, _In_ UCHAR Addr, _In_ UCHAR Reg, _In_ UCHAR Val);

/* ---- Log file (C:\TopazBattery.log) --------------------------------------- */

VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);

/* ---- Battery state (filled by the poll thread, read by battc callbacks) --- */

typedef struct _BATT_SNAPSHOT {
    BOOLEAN Valid;
    BOOLEAN Present;
    ULONG   SocTenths;          /* 0..1000 */
    ULONG   VoltageMv;
    LONG    CurrentMa;          /* + charging, - discharging */
    LONG    TempTenthsC;
    ULONG   FullMah;
    ULONG   Cycles;
    UCHAR   ChgReg03, ChgReg0B;
    BOOLEAN OnLine, Charging, ChargeDone;
    UCHAR   CcStatus;           /* rt1711h CC_STATUS, 0xFF if unread */
} BATT_SNAPSHOT, *PBATT_SNAPSHOT;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE    Device;
    WDFWAITLOCK  ClassInitLock;
    PVOID        ClassHandle;

    GENI_I2C     Bus;
    TLMM_PIN     PinSda, PinScl;
    BOOLEAN      HwReady;

    KSPIN_LOCK   SnapLock;
    BATT_SNAPSHOT Snap;
    ULONG        LastNotifiedState, LastNotifiedSoc;

    HANDLE       ThreadHandle;
    PKTHREAD     Thread;
    KEVENT       StopEvent;
    ULONG        Polls;
    ULONG        LastVbusStat;      /* charger REG0B VBUS_STAT at the last poll (~0 = none yet) */
    UCHAR        LastCcStatus;      /* rt1711h CC_STATUS at the last poll */
    ULONG        JeitaZone;         /* index into the JEITA table, ~0 = none yet */
    BOOLEAN      TcpcOk;            /* rt1711h answered and is in sink Rd/Rd: CC states usable */
    BOOLEAN      CcOtgOffPending;   /* OTG was dropped because CC showed a source: verify next poll */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

EVT_WDF_DRIVER_DEVICE_ADD        TopazEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE  TopazEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE  TopazEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY          TopazEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT           TopazEvtD0Exit;
EVT_WDFDEVICE_WDM_IRP_PREPROCESS TopazWdmPreprocessDeviceControl;

NTSTATUS BattHwInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattHwDeinit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattPoll(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS BattThreadStart(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattThreadStop(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattOtgOff(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS BattClassInit(_In_ PDEVICE_CONTEXT Ctx);
VOID     BattClassUnload(_In_ PDEVICE_CONTEXT Ctx);
