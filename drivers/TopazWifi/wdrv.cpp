/*
 * TopazWifi: driver/device setup (WDK wificx sample driver.cpp + device.cpp) and the modem thread
 * (TopazModem driver.c): the WLAN core boots the modem DSP through TrustZone and then runs its
 * polling loop forever on one thread. Safety as in TopazModem: the modem only boots when
 * C:\topaz\modem.arm exists, and the driver deletes it first.
 */
#include "wpch.h"

TOPAZ_WIFI_DEVICE *g_Wifi;
const UCHAR g_WifiMac[6] = { 0x02, 0x54, 0x4F, 0x50, 0x41, 0x5A };

extern "C" DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
static EVT_WDF_DRIVER_UNLOAD EvtDriverUnload;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

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
    LONG_PTR s;

    UNREFERENCED_PARAMETER(Context);
    KeSetSystemAffinityThreadEx((KAFFINITY)1);         /* TZ resumes + polling stay on one core */
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    if (!ArmConsume()) {
        WLOG("C:\\topaz\\modem.arm missing: modem not started (run install.cmd to arm)\r\n");
        PsTerminateSystemThread(STATUS_SUCCESS);
    }
    WLOG("armed: booting the modem\r\n");
    ExSetTimerResolution(10000, TRUE);
    s = ModemPasTest();                                /* returns only on gModemStop or a failure */
    WLOG("ModemPasTest returned %llx%s\r\n", (ULONGLONG)s, gModemStop ? " (driver stop)" : "");
    LogSetLazy(FALSE);
    ExSetTimerResolution(0, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    WLOG("\r\n==== TopazWifi " TOPAZ_WIFI_VERSION " ====\r\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    config.DriverPoolTag = TOPAZ_WIFI_TAG;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    WLOG("WdfDriverCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    WLOG("unload\r\n");
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WIFI_DEVICE_CONFIG wifiConfig;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    status = NetDeviceInitConfig(DeviceInit);          /* data path: NetAdapterCx */
    WLOG("NetDeviceInitConfig: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WifiDeviceInitConfig(DeviceInit);         /* control path: WiFiCx */
    WLOG("WifiDeviceInitConfig: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, TOPAZ_WIFI_DEVICE);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    WLOG("WdfDeviceCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WIFI_DEVICE_CONFIG_INIT(&wifiConfig, WDI_VERSION_LATEST, EvtWifiSendCommand, EvtWifiCreateAdapter,
                            EvtWifiCreateWifiDirectDevice);
    status = WifiDeviceInitialize(device, &wifiConfig);
    WLOG("WifiDeviceInitialize: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    g_Wifi = WifiDev(device);
    g_Wifi->WdfTriageInfoPtr = WdfGetTriageInfo();
    g_Wifi->Device = device;
    g_Wifi->Tlv.AllocationContext = 0;
    g_Wifi->Tlv.PeerVersion = WifiDeviceGetOsWdiVersion(device);
    WLOG("OS WDI version %08x (driver %08x)\r\n", g_Wifi->Tlv.PeerVersion, (ULONG)WDI_VERSION_LATEST);
    return STATUS_SUCCESS;
}

/* WiFiCx creates the station adapter (EvtWifiCreateAdapter) after this succeeds, so the Wi-Fi
   capabilities must be set here. They are static (WCN3950: 1x1, 2.4 + 5 GHz); the firmware boots
   in the background and scans wait for it (WlanOnReady). */
static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    HANDLE h;
    NTSTATUS status;
    TOPAZ_WIFI_DEVICE *dev = WifiDev(Device);

    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    status = WifiSetCapabilities(Device);
    WLOG("WifiSetCapabilities: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (dev->ModemThread == NULL) {
        gModemStop = FALSE;
        status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, ModemThread, NULL);
        if (NT_SUCCESS(status)) {
            ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&dev->ModemThread,
                                      NULL);
            ZwClose(h);
        } else {
            WLOG("PsCreateSystemThread: %08x\r\n", status);
        }
    }
    return STATUS_SUCCESS;
}

/* Device removal: stop the polling loop (the modem itself keeps running; reboot to retry). */
static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    TOPAZ_WIFI_DEVICE *dev = WifiDev(Device);

    UNREFERENCED_PARAMETER(Translated);
    WLOG("ReleaseHardware\r\n");
    if (dev->ModemThread != NULL) {
        gModemStop = TRUE;
        KeWaitForSingleObject(dev->ModemThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(dev->ModemThread);
        dev->ModemThread = NULL;
    }
    return STATUS_SUCCESS;
}
