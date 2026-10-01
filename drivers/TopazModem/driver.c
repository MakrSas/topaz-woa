/*
 * TopazModem: boots the modem DSP (MPSS) of Redmi Note 12 4G through TrustZone PAS and serves
 * what it needs from the apps side (SMP2P, GLINK/QRTR, pd-mapper, tftp, rmtfs, WLFW handshake),
 * so the WCN3990 WLAN firmware comes up. Root-enumerated KMDF driver, one polling system thread.
 * The modem code is ported from uefi/TopazOtgDxe (see compat.h / Modem.h).
 *
 * Safety: the thread only boots the modem when C:\topaz\modem.arm exists and deletes it first,
 * so a crash cannot repeat on every boot. install.cmd creates it.
 */
#include <ntddk.h>
#include <wdf.h>
#include "Modem.h"

#define TOPAZ_MODEM_VERSION "v0.6"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD TopazEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD TopazEvtDriverUnload;
static EVT_WDF_DEVICE_D0_ENTRY TopazEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT TopazEvtD0Exit;

static PKTHREAD g_Thread;
static BOOLEAN  g_Started;

/* Consume C:\topaz\modem.arm: TRUE if it existed (and is now deleted). */
static BOOLEAN ArmConsume(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\modem.arm");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                 FILE_DELETE_ON_CLOSE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static VOID ModemThread(PVOID Context)
{
    EFI_STATUS s;

    UNREFERENCED_PARAMETER(Context);
    /* TZ resumes (INTERRUPTED -> x0 = 1) and the polling loop stay on one core */
    KeSetSystemAffinityThreadEx((KAFFINITY)1);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    if (!ArmConsume()) {
        LogPrint("C:\\topaz\\modem.arm missing: modem not started (run install.cmd to arm)\r\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
    LogPrint("armed: booting the modem\r\n");
    ExSetTimerResolution(10000, TRUE);                 /* 1 ms sleeps in the polling loop */
    s = ModemPasTest();
    ModemOut("  ModemPasTest: %r%a\r\n", s, gModemStop ? " (driver stop)" : "");
    LogSetLazy(FALSE);
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("\r\n==== TopazModem " TOPAZ_MODEM_VERSION " ====\r\n");

    WDF_DRIVER_CONFIG_INIT(&config, TopazEvtDeviceAdd);
    config.EvtDriverUnload = TopazEvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    LogPrint("WdfDriverCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID TopazEvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogPrint("unload\r\n");
    LogClose();
}

static NTSTATUS TopazEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = TopazEvtD0Entry;
    pnp.EvtDeviceD0Exit = TopazEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    LogPrint("WdfDeviceCreate: %08x\r\n", status);
    return status;
}

static NTSTATUS TopazEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    if (g_Started) {                                   /* the modem is booted once per Windows boot */
        return STATUS_SUCCESS;
    }
    g_Started = TRUE;
    gModemStop = FALSE;
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, ModemThread, NULL);
    if (!NT_SUCCESS(status)) {
        LogPrint("PsCreateSystemThread: %08x\r\n", status);
        return STATUS_SUCCESS;
    }
    ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&g_Thread, NULL);
    ZwClose(h);
    return STATUS_SUCCESS;
}

static NTSTATUS TopazEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Device);
    LogPrint("D0Exit target=%u\r\n", (ULONG)TargetState);
    if (g_Thread != NULL) {
        gModemStop = TRUE;
        KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Thread);
        g_Thread = NULL;
    }
    return STATUS_SUCCESS;
}
