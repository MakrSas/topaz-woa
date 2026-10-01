/*
 * TopazButtons - Power, Vol- and Vol+ keys of Redmi Note 12 4G (topaz, SM6225) as HID.
 *
 * Power and Vol- are PM6125 PON inputs (KPDPWR, RESIN): PON INT_RT_STS (SID 0, 0x810),
 * bit0 KPDPWR, bit1 RESIN. Vol+ is PM6125 GPIO5 (stock DT gpio_keys/vol_up, active low,
 * pull-up): GPIO5 RT_STS (SID 0, 0xC410) bit0 = input level. The driver only READS them
 * through SPMI PMIC arbiter v5 observer channels (no PMIC writes) every 30 ms and reports:
 * Power -> Generic Desktop System Power Down, Vol+/Vol- -> Consumer Volume Increment /
 * Decrement, through VHF. Log: C:\TopazButtons.log.
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

#define GPIO5_PPID          0x00C4          /* SID 0, peripheral 0xC4 = PM6125 GPIO5 (0xC000 + 4 * 0x100) */
#define GPIO_RT_STS         0x10            /* pinctrl-spmi-gpio PMIC_MPP_REG_RT_STS, bit0 = level */

#define POLL_MS             30

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE        Device;
    VHFHANDLE        Vhf;
    volatile UCHAR  *Obs;                   /* observer channel registers of PON */
    volatile UCHAR  *GpioObs;               /* observer channel of PM6125 GPIO5 (Vol+), may be NULL */
    BOOLEAN          VolUpSeenHigh;         /* Vol+ counts only after a released (high) level was seen */
    UCHAR            LastVol;               /* report 2 bits: 1 = Vol+, 2 = Vol- */
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
static ULONG FindChannel(volatile UCHAR *Core, ULONG Ppid)
{
    ULONG n, v;

    for (n = 0; n < (SPMI_CORE_SIZE - SPMI_APID_MAP) / 4; n++) {
        v = READ_REGISTER_ULONG((volatile ULONG *)(Core + SPMI_APID_MAP + 4 * n));
        if (v != 0 && ((v >> 8) & 0xFFF) == Ppid) {
            return n;
        }
    }
    return MAXULONG;
}

static NTSTATUS ObsRead(volatile UCHAR *Obs, UCHAR Offset, PUCHAR Value)
{
    ULONG i, st;

    WRITE_REGISTER_ULONG((volatile ULONG *)(Obs + ARB_CMD), (1u << 27) | ((ULONG)Offset << 4));
    for (i = 0; i < 1000; i++) {
        st = READ_REGISTER_ULONG((volatile ULONG *)(Obs + ARB_STATUS));
        if (st & ARB_DONE) {
            if (st & ARB_ERR) {
                return STATUS_DEVICE_PROTOCOL_ERROR;
            }
            *Value = (UCHAR)READ_REGISTER_ULONG((volatile ULONG *)(Obs + ARB_RDATA0));
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS PonRead(PDEVICE_CONTEXT Ctx, UCHAR Offset, PUCHAR Value)
{
    return ObsRead(Ctx->Obs, Offset, Value);
}

static volatile UCHAR *MapChannel(ULONG Apid)
{
    PHYSICAL_ADDRESS pa;

    pa.QuadPart = (LONGLONG)(SPMI_OBSRVR_BASE + Apid * SPMI_OBS_STRIDE);
    return (volatile UCHAR *)MmMapIoSpaceEx(pa, SPMI_OBS_STRIDE, PAGE_READWRITE | PAGE_NOCACHE);
}

static NTSTATUS HwInit(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *core;
    ULONG ch, gch;
    UCHAR rt = 0, lvl = 0;
    NTSTATUS status;

    pa.QuadPart = (LONGLONG)SPMI_CORE_BASE;
    core = (volatile UCHAR *)MmMapIoSpaceEx(pa, SPMI_CORE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (core == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    LogPrint("arbiter version %08x\n", READ_REGISTER_ULONG((volatile ULONG *)core));
    ch = FindChannel(core, PON_PPID);
    gch = FindChannel(core, GPIO5_PPID);
    MmUnmapIoSpace((PVOID)core, SPMI_CORE_SIZE);
    LogPrint("PON channel %u, GPIO5 (Vol+) channel %u\n", ch, gch);
    if (ch > 511) {
        return STATUS_NOT_FOUND;
    }

    Ctx->Obs = MapChannel(ch);
    if (Ctx->Obs == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Vol+ is optional: without it Power/Vol- still work */
    Ctx->GpioObs = gch <= 511 ? MapChannel(gch) : NULL;
    if (Ctx->GpioObs != NULL) {
        UCHAR regs[0x50];
        ULONG i;
        for (i = 0; i < sizeof(regs); i++) {
            regs[i] = 0xEE;
            ObsRead(Ctx->GpioObs, (UCHAR)i, &regs[i]);
        }
        /* 0x04 TYPE 0x10 = GPIO, 0x05 subtype, 0x08 STATUS1, 0x10 RT_STS, 0x40 MODE_CTL,
           0x41 DIG_VIN, 0x42 DIG_PULL, 0x43 DIG_IN_CTL, 0x46 EN_CTL */
        LogHex("GPIO5 00-0f:", regs, 0x10);
        LogHex("GPIO5 10-1f:", regs + 0x10, 0x10);
        LogHex("GPIO5 40-4f:", regs + 0x40, 0x10);
        if (NT_SUCCESS(ObsRead(Ctx->GpioObs, GPIO_RT_STS, &lvl))) {
            Ctx->VolUpSeenHigh = (lvl & 1) != 0;
        }
        LogPrint("Vol+ level=%u (1 = released), reporting %s\n", lvl & 1,
                 Ctx->VolUpSeenHigh ? "now" : "after the first released level");
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
    Ctx->LastVol = (rt & RT_RESIN) ? 2 : 0;
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
    if (Ctx->GpioObs != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->GpioObs, SPMI_OBS_STRIDE);
        Ctx->GpioObs = NULL;
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
    UCHAR rt, lvl, vol, changed;
    ULONG errors = 0;

    period.QuadPart = -10000LL * POLL_MS;
    LogPrint("poll thread started, rt=%02x vol=%02x\n", ctx->LastRt, ctx->LastVol);
    while (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        rt = 0;
        if (!NT_SUCCESS(PonRead(ctx, PON_INT_RT_STS, &rt))) {
            if (errors++ < 10) {
                LogPrint("PON read failed\n");
            }
            continue;
        }
        rt &= RT_KPDPWR | RT_RESIN;

        /* Vol+: GPIO5 level, active low. Ignore it until a released (high) level was seen once,
           so an unconfigured pin reading 0 forever never looks like a held key. */
        vol = (rt & RT_RESIN) ? 2 : 0;
        lvl = 1;
        if (ctx->GpioObs != NULL && NT_SUCCESS(ObsRead(ctx->GpioObs, GPIO_RT_STS, &lvl))) {
            if (lvl & 1) {
                if (!ctx->VolUpSeenHigh) {
                    LogPrint("Vol+ released level seen, Vol+ enabled\n");
                }
                ctx->VolUpSeenHigh = TRUE;
            } else if (ctx->VolUpSeenHigh) {
                vol |= 1;
            }
        }

        changed = rt ^ ctx->LastRt;
        if (changed & RT_KPDPWR) {
            LogPrint("keys: power=%u\n", (rt & RT_KPDPWR) ? 1 : 0);
            SendReport(ctx, 1, (rt & RT_KPDPWR) ? 1 : 0);
        }
        ctx->LastRt = rt;
        if (vol != ctx->LastVol) {
            LogPrint("keys: vol+=%u vol-=%u\n", vol & 1, (vol >> 1) & 1);
            SendReport(ctx, 2, vol);           /* report 2: bit0 Volume Increment, bit1 Decrement */
            ctx->LastVol = vol;
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
    LogPrint("==== TopazButtons v0.4 (Vol+) ====\n");
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
