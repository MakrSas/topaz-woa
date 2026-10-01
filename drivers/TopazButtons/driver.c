/*
 * TopazButtons - Power and Vol- keys of Redmi Note 12 4G (topaz, SM6225) as HID.
 *
 * Both keys are PM6125 PON inputs (KPDPWR, RESIN). Their live state is PON
 * INT_RT_STS (SID 0, 0x810): bit0 KPDPWR, bit1 RESIN. The driver only READS it
 * through the SPMI PMIC arbiter v5 observer channel (no PMIC writes) every 30 ms
 * and reports: Power -> Generic Desktop System Power Down, Vol- -> Consumer
 * Volume Decrement, through VHF. Log: C:\TopazButtons.log.
 */
#include "driver.h"

#define SPMI_CORE_BASE      0x01C40000ULL
#define SPMI_CORE_SIZE      0x1100
#define SPMI_APID_MAP       0x900
#define SPMI_OBSRVR_BASE    0x03E00000ULL
#define SPMI_OBS_STRIDE     0x80ULL          /* v5: obsrvr + 0x10000*ee + 0x80*apid, ee 0 = apps */

#define ARB_CMD             0x00
#define ARB_STATUS          0x08
#define ARB_RDATA0          0x18
#define ARB_DONE            (1u << 0)
#define ARB_ERR             (7u << 1)

#define PON_PPID            0x0008          /* SID 0, peripheral 0x08 */
#define PON_INT_RT_STS      0x10
#define RT_KPDPWR           (1u << 0)
#define RT_RESIN            (1u << 1)

#define POLL_MS             30

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE        Device;
    VHFHANDLE        Vhf;
    volatile UCHAR  *Obs;                   /* observer channel registers of PON */
    BOOLEAN          HwReady;
    PKTHREAD         Thread;
    KEVENT           StopEvent;
    UCHAR            LastRt;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY         EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT          EvtD0Exit;

/* Report 1: System Power Down. Report 2: Volume Increment/Decrement. */
static const UCHAR g_ReportDescriptor[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x80,        /* Usage (System Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID 1 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,
    0x09, 0x81,        /*   Usage (System Power Down) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x95, 0x07, 0x81, 0x03,
    0xC0,
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,
    0x85, 0x02,        /*   Report ID 2 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x09, 0xE9,        /*   Volume Increment */
    0x09, 0xEA,        /*   Volume Decrement */
    0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    0xC0,
};

/* ---- SPMI (read only) ------------------------------------------------------ */

/*
 * Arbiter v5: APID map in core space, core + 0x900 + 4*apid, PPID in bits [19:8].
 * (The bit-tree mapping table of v1-v3 reads as zeros here.)
 */
static ULONG FindPonChannel(volatile UCHAR *Core)
{
    ULONG ver = READ_REGISTER_ULONG((volatile ULONG *)Core);
    ULONG n, v, logged = 0;

    LogPrint("arbiter version %08x\n", ver);
    for (n = 0; n < (SPMI_CORE_SIZE - SPMI_APID_MAP) / 4; n++) {
        v = READ_REGISTER_ULONG((volatile ULONG *)(Core + SPMI_APID_MAP + 4 * n));
        if (v != 0 && logged < 40) {
            LogPrint("apid[%u]=%08x ppid=%03x\n", n, v, (v >> 8) & 0xFFF);
            logged++;
        }
        if (((v >> 8) & 0xFFF) == PON_PPID) {
            return n;
        }
    }
    return MAXULONG;
}

static NTSTATUS PonRead(PDEVICE_CONTEXT Ctx, UCHAR Offset, PUCHAR Value)
{
    ULONG i, st;

    WRITE_REGISTER_ULONG((volatile ULONG *)(Ctx->Obs + ARB_CMD), (1u << 27) | ((ULONG)Offset << 4));
    for (i = 0; i < 1000; i++) {
        st = READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Obs + ARB_STATUS));
        if (st & ARB_DONE) {
            if (st & ARB_ERR) {
                return STATUS_DEVICE_PROTOCOL_ERROR;
            }
            *Value = (UCHAR)READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Obs + ARB_RDATA0));
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS HwInit(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *core;
    ULONG ch;
    UCHAR rt = 0;
    NTSTATUS status;

    pa.QuadPart = (LONGLONG)SPMI_CORE_BASE;
    core = (volatile UCHAR *)MmMapIoSpaceEx(pa, SPMI_CORE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (core == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    ch = FindPonChannel(core);
    MmUnmapIoSpace((PVOID)core, SPMI_CORE_SIZE);
    LogPrint("PON channel %u\n", ch);
    if (ch > 511) {
        return STATUS_NOT_FOUND;
    }

    pa.QuadPart = (LONGLONG)(SPMI_OBSRVR_BASE + ch * SPMI_OBS_STRIDE);
    Ctx->Obs = (volatile UCHAR *)MmMapIoSpaceEx(pa, SPMI_OBS_STRIDE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->Obs == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    {
        UCHAR regs[0x20];
        ULONG i;
        for (i = 0; i < sizeof(regs); i++) {
            regs[i] = 0xEE;
            PonRead(Ctx, (UCHAR)i, &regs[i]);
        }
        LogHex("PON 00-1f:", regs, sizeof(regs));   /* 0x04 TYPE should be 01 (PON) */
    }
    status = PonRead(Ctx, PON_INT_RT_STS, &rt);
    LogPrint("PON INT_RT_STS=%02x (%08x)\n", rt, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Ctx->LastRt = rt & (RT_KPDPWR | RT_RESIN);
    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

static VOID HwDeinit(PDEVICE_CONTEXT Ctx)
{
    Ctx->HwReady = FALSE;
    if (Ctx->Obs != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Obs, SPMI_OBS_STRIDE);
        Ctx->Obs = NULL;
    }
}

/* ---- Reports ---------------------------------------------------------------- */

static VOID SendReport(PDEVICE_CONTEXT Ctx, UCHAR Id, UCHAR Bits)
{
    UCHAR buf[2] = { Id, Bits };
    HID_XFER_PACKET pkt;

    if (Ctx->Vhf == NULL) {
        return;
    }
    pkt.reportBuffer = buf;
    pkt.reportBufferLen = sizeof(buf);
    pkt.reportId = Id;
    VhfReadReportSubmit(Ctx->Vhf, &pkt);
}

static KSTART_ROUTINE PollThread;

static VOID PollThread(PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;
    UCHAR rt, changed;
    ULONG errors = 0;

    period.QuadPart = -10000LL * POLL_MS;
    LogPrint("poll thread started, rt=%02x\n", ctx->LastRt);
    while (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        rt = 0;
        if (!NT_SUCCESS(PonRead(ctx, PON_INT_RT_STS, &rt))) {
            if (errors++ < 10) {
                LogPrint("PON read failed\n");
            }
            continue;
        }
        rt &= RT_KPDPWR | RT_RESIN;
        changed = rt ^ ctx->LastRt;
        if (changed == 0) {
            continue;
        }
        ctx->LastRt = rt;
        LogPrint("keys: power=%u vol-=%u\n", (rt & RT_KPDPWR) ? 1 : 0, (rt & RT_RESIN) ? 1 : 0);
        if (changed & RT_KPDPWR) {
            SendReport(ctx, 1, (rt & RT_KPDPWR) ? 1 : 0);
        }
        if (changed & RT_RESIN) {
            SendReport(ctx, 2, (rt & RT_RESIN) ? 2 : 0);
        }
    }
    LogPrint("poll thread exit\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazButtons v0.3 ====\n");
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
    PDEVICE_CONTEXT ctx;
    VHF_CONFIG cfg;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->Device = device;

    VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(device),
                    (USHORT)sizeof(g_ReportDescriptor), (PUCHAR)g_ReportDescriptor);
    cfg.VhfClientContext = ctx;
    cfg.VendorID = 0x05C6;   /* Qualcomm */
    cfg.ProductID = 0x6125;
    status = VhfCreate(&cfg, &ctx->Vhf);
    LogPrint("VhfCreate: %08x\n", status);
    if (NT_SUCCESS(status)) {
        status = VhfStart(ctx->Vhf);
        LogPrint("VhfStart: %08x\n", status);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    LogPrint("HwInit: %08x\n", HwInit(DeviceGetContext(Device)));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Vhf != NULL) {
        VhfDelete(ctx->Vhf, TRUE);
        ctx->Vhf = NULL;
    }
    HwDeinit(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);
    if (!ctx->HwReady) {
        return STATUS_SUCCESS;
    }
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
    ZwClose(h);
    return status;
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);
    if (ctx->Thread != NULL) {
        KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->Thread);
        ctx->Thread = NULL;
    }
    return STATUS_SUCCESS;
}
