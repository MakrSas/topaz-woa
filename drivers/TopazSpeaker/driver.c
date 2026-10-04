/*
 * TopazSpeaker driver / device: KMDF + ACX 1.0 (Acx01000.sys is in-box on 22621). Root-enumerated
 * (Root\TopazSpeaker), one static render circuit "Speaker0" (matches the INF interface reference).
 */
#include "tspk.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD SpkEvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD SpkEvtDriverUnload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE SpkEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE SpkEvtReleaseHardware;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG cfg;
    ACX_DRIVER_CONFIG acxCfg;
    WDFDRIVER driver;
    NTSTATUS status;

    LogOpen();
    LogPrint("\r\n==== TopazSpeaker " TSPK_VERSION " ====\r\n");
    WDF_DRIVER_CONFIG_INIT(&cfg, SpkEvtDeviceAdd);
    cfg.EvtDriverUnload = SpkEvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &cfg, &driver);
    if (!NT_SUCCESS(status)) {
        LogPrint("WdfDriverCreate: %08x\r\n", status);
        LogClose();
        return status;
    }
    ACX_DRIVER_CONFIG_INIT(&acxCfg);
    status = AcxDriverInitialize(driver, &acxCfg);
    LogPrint("AcxDriverInitialize: %08x\r\n", status);
    return status;
}

static VOID SpkEvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogPrint("unload\r\n");
    LogClose();
}

static NTSTATUS SpkEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    ACX_DEVICEINIT_CONFIG initCfg;
    ACX_DEVICE_CONFIG devCfg;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDFDEVICE device;
    SPK_DEVICE_CONTEXT *ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    ACX_DEVICEINIT_CONFIG_INIT(&initCfg);
    status = AcxDeviceInitInitialize(DeviceInit, &initCfg);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxDeviceInitInitialize: %08x\r\n", status);
        return status;
    }
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = SpkEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = SpkEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SPK_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) {
        LogPrint("WdfDeviceCreate: %08x\r\n", status);
        return status;
    }
    ctx = SpkGetDevice(device);
    ctx->VolumeLevel = SPK_VOL_DEF;
    ctx->Mute = 0;

    ACX_DEVICE_CONFIG_INIT(&devCfg);
    status = AcxDeviceInitialize(device, &devCfg);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxDeviceInitialize: %08x\r\n", status);
        return status;
    }
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &caps);

    status = SpkCreateRenderCircuit(device, &ctx->Circuit);
    LogPrint("render circuit: %08x\r\n", status);
    return status;
}

static NTSTATUS SpkEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    SPK_DEVICE_CONTEXT *ctx = SpkGetDevice(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    status = AcxDeviceAddCircuit(Device, ctx->Circuit);
    LogPrint("AcxDeviceAddCircuit: %08x\r\n", status);
    return status;
}

static NTSTATUS SpkEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    SPK_DEVICE_CONTEXT *ctx = SpkGetDevice(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Circuit != NULL) {
        (VOID)AcxDeviceRemoveCircuit(Device, ctx->Circuit);
    }
    LogPrint("release hardware\r\n");
    return STATUS_SUCCESS;
}
