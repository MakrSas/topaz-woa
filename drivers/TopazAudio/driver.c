/*
 * TopazAudio: boots the ADSP (LPASS) of Redmi Note 12 4G through TrustZone PAS and talks to it
 * over SMP2P + GLINK (QRTR with pd-mapper, GPR for AudioReach). Step 1 of sound (docs/P9_audio.md).
 * Root-enumerated KMDF driver, one polling system thread (see Audio.h).
 *
 * Safety: the thread only boots the ADSP when C:\topaz\audio.arm exists and deletes it first,
 * so a crash cannot repeat on every boot.
 */
#include <ntddk.h>
#include <wdf.h>
#include "Audio.h"

#define TOPAZ_AUDIO_VERSION "v0.5.2"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD TopazEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD TopazEvtDriverUnload;
static EVT_WDF_DEVICE_D0_ENTRY TopazEvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT TopazEvtD0Exit;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TopazEvtIoctl;


static PKTHREAD g_Thread;
static BOOLEAN  g_Started;

/* Consume a one-shot file (C:\topaz\audio.arm, C:\topaz\amp.probe): TRUE if it existed. */
static BOOLEAN ArmConsume(PCWSTR Path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN,
                                 FILE_DELETE_ON_CLOSE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static VOID AudioThread(PVOID Context)
{
    EFI_STATUS s;

    UNREFERENCED_PARAMETER(Context);
    /* TZ resumes (INTERRUPTED -> x0 = 1) and the polling loop stay on one core; core 0 is
       TopazWifi's modem thread */
    KeSetSystemAffinityThreadEx((KAFFINITY)2);
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    if (ArmConsume(L"\\??\\C:\\topaz\\amp.probe")) {
        AmpProbe();
        LogSetLazy(FALSE);
    }
    if (!ArmConsume(L"\\??\\C:\\topaz\\audio.arm")) {
        LogPrint("C:\\topaz\\audio.arm missing: ADSP not started\r\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
    LogPrint("armed: booting the ADSP\r\n");
    ExSetTimerResolution(10000, TRUE);                 /* 1 ms sleeps in the polling loop */
    s = AdspBoot();
    ModemOut("  AdspBoot: %r%a\r\n", s, gModemStop ? " (driver stop)" : "");
    LogSetLazy(FALSE);
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("\r\n==== TopazAudio " TOPAZ_AUDIO_VERSION " ====\r\n");
    LabInit();

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
    WDF_IO_QUEUE_CONFIG qcfg;
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\TopazAudio");
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = TopazEvtD0Entry;
    pnp.EvtDeviceD0Exit = TopazEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    LogPrint("WdfDeviceCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&qcfg, WdfIoQueueDispatchSequential);
    qcfg.EvtIoDeviceControl = TopazEvtIoctl;
    status = WdfIoQueueCreate(device, &qcfg, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (NT_SUCCESS(status)) {
        status = WdfDeviceCreateSymbolicLink(device, &link);
    }
    LogPrint("lab interface \\\\.\\TopazAudio: %08x\r\n", status);
    return STATUS_SUCCESS;                             /* the ADSP part works without the lab */
}

static VOID TopazEvtIoctl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLen, size_t InLen, ULONG Code)
{
    PVOID buf = NULL;
    size_t len = 0;
    UINT32 info = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);
    /* METHOD_BUFFERED: one system buffer of max(in, out) bytes */
    if (InLen != 0 || OutLen != 0) {
        status = (InLen != 0) ? WdfRequestRetrieveInputBuffer(Request, 1, &buf, &len)
                              : WdfRequestRetrieveOutputBuffer(Request, 1, &buf, &len);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
    }
    status = LabIoctl(Code, buf, (UINT32)InLen, (UINT32)OutLen, &info);
    WdfRequestCompleteWithInformation(Request, status, info);
}

static NTSTATUS TopazEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    if (g_Started) {                                   /* the ADSP is booted once per driver load */
        return STATUS_SUCCESS;
    }
    g_Started = TRUE;
    gModemStop = FALSE;
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, AudioThread, NULL);
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
