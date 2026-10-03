/*
 * TopazRpm - RPM requests of Redmi Note 12 4G (topaz/tapas, SM6225 "khaje") under Windows.
 *
 * The RPM (resource power manager) owns the clocks and rails that the AP votes for: the IPA clock
 * (needed before IPA/GSI registers can be touched - SIM / mobile data, docs/P9_sim.md), the GPU
 * rails, bus bandwidth. Stock DT: rpm-glink (qcom,glink-rpm) over the RPM message RAM
 * memory@045f0000 (0x7000 bytes), interrupt SPI 0xC2 from the RPM, doorbell = APCS IPC bit 0
 * (mboxes = <&apcs 0>), channel "rpm_requests" (qcom,rpm-smd).
 * Layout (Linux drivers/rpmsg/qcom_glink_rpm.c): a 256-byte TOC at the end of the message RAM,
 * magic "grt0", entries {id, offset, size}; "ap2r" = AP->RPM FIFO, "r2ap" = RPM->AP FIFO, each
 * {u32 tail, u32 head, data[size]} at offset. GLINK native protocol on top, intentless.
 *
 * v0.1: read only - logs the TOC, both FIFOs' indices and the bytes waiting in them
 * (C:\TopazRpm.log). Nothing is written to the RPM.
 */
#include "driver.h"

#define TOPAZ_RPM_VERSION   "v0.1"

#define MSG_RAM_PA          0x045F0000ULL
#define MSG_RAM_SIZE        0x7000
#define TOC_SIZE            256
#define TOC_MAGIC           0x67727430u     /* "grt0" */
#define FIFO_AP2R           0x61703272u     /* "ap2r" */
#define FIFO_R2AP           0x72326170u     /* "r2ap" */

typedef struct _DEVICE_CONTEXT {
    volatile UCHAR *Ram;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

static ULONG Rd(PDEVICE_CONTEXT Ctx, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Ram + Off));
}

static PCSTR FifoName(ULONG Id)
{
    return Id == FIFO_AP2R ? "ap2r (AP->RPM)" : Id == FIFO_R2AP ? "r2ap (RPM->AP)" : "?";
}

/* log up to 64 bytes from a FIFO starting at its tail, word reads only */
static VOID DumpFifo(PDEVICE_CONTEXT Ctx, ULONG Off, ULONG Size)
{
    ULONG tail = Rd(Ctx, Off), head = Rd(Ctx, Off + 4), avail, i, n;
    UCHAR buf[64];

    avail = (head >= tail) ? head - tail : Size - tail + head;
    LogPrint("    tail %x head %x size %x -> %u bytes pending\n", tail, head, Size, avail);
    if (tail >= Size || head >= Size) {
        LogPrint("    indices out of range, not dumping\n");
        return;
    }
    n = min(avail, (ULONG)sizeof(buf)) & ~3u;
    for (i = 0; i < n; i += 4) {
        ULONG pos = (tail + i) % Size;
        *(ULONG *)(buf + i) = Rd(Ctx, Off + 8 + pos);
    }
    if (n != 0) {
        LogHex("    data:", buf, n);
    }
}

static NTSTATUS Survey(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    ULONG toc = MSG_RAM_SIZE - TOC_SIZE, magic, count, i;

    pa.QuadPart = MSG_RAM_PA;
    Ctx->Ram = (volatile UCHAR *)MmMapIoSpaceEx(pa, MSG_RAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->Ram == NULL) {
        LogPrint("map %llx failed\n", MSG_RAM_PA);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    magic = Rd(Ctx, toc);
    count = Rd(Ctx, toc + 4);
    LogPrint("msg ram %llx: TOC magic %08x (%s), %u entries\n", MSG_RAM_PA, magic,
             magic == TOC_MAGIC ? "grt0" : "BAD", count);
    if (magic != TOC_MAGIC || count > (TOC_SIZE - 8) / 12) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (i = 0; i < count; i++) {
        ULONG id = Rd(Ctx, toc + 8 + 12 * i), off = Rd(Ctx, toc + 12 + 12 * i), size = Rd(Ctx, toc + 16 + 12 * i);
        LogPrint("  [%u] id %08x %s off %x size %x\n", i, id, FifoName(id), off, size);
        if ((id == FIFO_AP2R || id == FIFO_R2AP) && off + 8 + size <= MSG_RAM_SIZE) {
            DumpFifo(Ctx, off, size);
        }
    }
    return STATUS_SUCCESS;
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazRpm " TOPAZ_RPM_VERSION " ====\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(DeviceGetContext(device), sizeof(DEVICE_CONTEXT));
    }
    return status;
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    LogPrint("survey: %08x\n", Survey(DeviceGetContext(Device)));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Ram != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Ram, MSG_RAM_SIZE);
        ctx->Ram = NULL;
    }
    return STATUS_SUCCESS;
}
