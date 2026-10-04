#include "driver.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD TopazEvtDriverUnload;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazTouch v0.14 (default controller rate, 2 ms polling, count-0 release) ====\n");

    WDF_DRIVER_CONFIG_INIT(&config, TopazEvtDeviceAdd);
    config.EvtDriverUnload = TopazEvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    LogPrint("WdfDriverCreate: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID TopazEvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogPrint("unload\n");
    LogClose();
}

NTSTATUS TopazEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = TopazEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = TopazEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = TopazEvtD0Entry;
    pnp.EvtDeviceD0Exit = TopazEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    LogPrint("WdfDeviceCreate: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;

    /* VHF must be created from AddDevice/PrepareHardware at PASSIVE_LEVEL */
    status = TouchVhfCreate(ctx);
    if (!NT_SUCCESS(status)) {
        LogPrint("VHF failed, touch reports disabled: %08x\n", status);
    }
    return STATUS_SUCCESS;
}

NTSTATUS TopazEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);

    status = TouchHwInit(ctx);
    LogPrint("TouchHwInit: %08x\n", status);
    /* Keep the device started even on failure so the log stays readable. */
    return STATUS_SUCCESS;
}

NTSTATUS TopazEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    TouchVhfDelete(ctx);
    TouchHwDeinit(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS TopazEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);
    if (!ctx->HwReady) {
        return STATUS_SUCCESS;
    }
    return TouchThreadStart(ctx);
}

NTSTATUS TopazEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    TouchThreadStop(DeviceGetContext(Device));
    return STATUS_SUCCESS;
}
