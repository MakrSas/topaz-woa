/*
 * TopazBacklight - panel brightness of Redmi Note 12 4G (topaz, SM6225) under Windows.
 *
 * The panel (stock DT qcom,mdss_dsi_panel_m7_38_0c_0a_fhdp_video, AMOLED, DSC, video mode) takes
 * its brightness as a DCS command: bl_ctrl_dcs, levels 1..0x7FF, bl-inverted-dbv = value sent MSB
 * first (DT doze example: 39 01 00 00 00 00 03 51 00 18). Nothing in this port sends it, so the
 * level stays where the bootloader left it.
 *
 * The command goes out through DSI0 (0x5E94000, stock DT qcom,mdss_dsi_ctrl0) in the "TPG DMA FIFO"
 * mode of Qualcomm's DSI 6G controller (downstream dsi_ctrl_hw_cmn_kickoff_fifo_command): the
 * packet is written into a register FIFO, so no DMA buffer and no SMMU mapping are needed. Packet
 * layout as in Linux dsi_cmd_dma_add: header bytes WC lo, WC hi, DT | VC << 6, flags (BIT7 last,
 * BIT6 long), payload padded with 0xff to 4 bytes.
 *
 * v0.2 experiments: C:\topaz\dsicmd = hex bytes "<DT> <payload...>" (05 = DCS short write, 15 = short
 * write + 1 parameter, 39 = DCS long write), sent once, then the file is deleted.
 *
 * v0.3: the TPG FIFO path reports CMD_DMA_DONE but nothing reaches the panel (display off/on had no
 * effect). With C:\topaz\dsi.dma present the packet goes the way Linux sends it instead: a
 * contiguous buffer below 4 GB, DSI_DMA_CMD_OFFSET = its physical address (MDSS must reach it
 * through the apps SMMU; its GFSR is logged before/after).
 *
 * Control: C:\topaz\brightness = 0..100 (text). Polled 3x a second; nothing is sent while the
 * file is absent (v0.1 then only logs the DSI registers). Log: C:\TopazBacklight.log.
 */
#include "driver.h"

#define TOPAZ_BL_VERSION    "v0.4"

#define DSI0_PA             0x05E94000ULL
#define DSI0_SIZE           0x400
#define DSI_HW_VERSION      0x000
#define DSI_CTRL            0x004
#define DSI_STATUS          0x008
#define DSI_VIDEO_MODE_CTRL 0x010
#define DSI_CMD_DMA_CTRL    0x03C           /* DSI_COMMAND_MODE_DMA_CTRL */
#define DSI_DMA_CMD_OFFSET  0x048
#define DSI_DMA_CMD_LENGTH  0x04C
#define DSI_TRIG_CTRL       0x084
#define DSI_DMA_SW_TRIGGER  0x090           /* DSI_CMD_MODE_DMA_SW_TRIGGER */
#define DSI_INT_CTRL        0x110
#define DSI_TPG_CTRL        0x15C           /* DSI_TEST_PATTERN_GEN_CTRL */
#define DSI_TPG_DMA_INIT    0x17C           /* DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL */
#define DSI_TPG_FIFO_STATUS 0x1DC
#define DSI_TPG_FIFO_RESET  0x1EC

#define CTRL_ENABLE         (1u << 0)
#define CTRL_CMD_MODE_EN    (1u << 2)
#define DMA_CTRL_LOW_POWER  (1u << 26)
#define INT_CMD_DMA_DONE    (1u << 0)
#define TRIG_DMA_SW         4u

#define BL_MAX              0x7FF
#define POLL_MS             330

#define APPS_SMMU_PA        0x0C600000ULL
#define SMMU_GFSR           0x048

typedef struct _DEVICE_CONTEXT {
    volatile UCHAR *Dsi;
    volatile UCHAR *Smmu;                   /* apps SMMU, read only (GFSR) */
    PUCHAR     DmaVa;                       /* v0.3 DMA command buffer */
    PHYSICAL_ADDRESS DmaPa;
    BOOLEAN    UseDma;
    BOOLEAN    HwReady;
    LONG       LastPct;                     /* -1 = nothing sent yet */
    PKTHREAD   Thread;
    KEVENT     StopEvent;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY         EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT          EvtD0Exit;

static ULONG Rd(PDEVICE_CONTEXT C, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(C->Dsi + Off));
}

static VOID Wr(PDEVICE_CONTEXT C, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(C->Dsi + Off), Val);
}

/* Brightness percent from C:\topaz\brightness, -1 if absent or unreadable. */
static LONG ReadPercent(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\brightness");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[16];
    LONG v = -1;
    ULONG i;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return -1;
    }
    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
        for (i = 0; i < iosb.Information; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                v = (v < 0 ? 0 : v * 10) + (buf[i] - '0');
                if (v > 100) {
                    v = 100;
                }
            } else if (v >= 0) {
                break;
            }
        }
    }
    ZwClose(h);
    return v;
}

/* One DSI packet through the TPG DMA FIFO. Long (DT 0x39/0x29) or short (0x05/0x15/0x03/0x13/0x23).
   Returns TRUE when the controller reported CMD_DMA_DONE. */
static BOOLEAN DsiSend(PDEVICE_CONTEXT C, UCHAR Dt, const UCHAR *Payload, ULONG Len)
{
    BOOLEAN isLong = (Dt == 0x39 || Dt == 0x29);
    UCHAR pkt[16];
    ULONG size, i, ctrl, dmaCtrl, trig, waited;
    BOOLEAN done = FALSE;

    if (Len > sizeof(pkt) - 4 || (!isLong && Len > 2)) {
        return FALSE;
    }
    RtlFillMemory(pkt, sizeof(pkt), 0xFF);
    if (isLong) {
        size = (4 + Len + 3) & ~3u;
        pkt[0] = (UCHAR)Len;                        /* word count lo */
        pkt[1] = (UCHAR)(Len >> 8);                 /* word count hi */
        pkt[2] = Dt;                                /* VC 0 */
        pkt[3] = 0x80 | 0x40;                       /* last packet, long packet */
        RtlCopyMemory(pkt + 4, Payload, Len);
    } else {
        size = 4;
        pkt[0] = Len > 0 ? Payload[0] : 0;          /* header data 0 */
        pkt[1] = Len > 1 ? Payload[1] : 0;          /* header data 1 */
        pkt[2] = Dt;
        pkt[3] = 0x80;                              /* last packet */
    }

    ctrl = Rd(C, DSI_CTRL);
    trig = Rd(C, DSI_TRIG_CTRL);
    if ((trig & 7) != TRIG_DMA_SW) {
        Wr(C, DSI_TRIG_CTRL, (trig & ~7u) | TRIG_DMA_SW);
    }
    Wr(C, DSI_CTRL, ctrl | CTRL_CMD_MODE_EN | CTRL_ENABLE);
    Wr(C, DSI_INT_CTRL, Rd(C, DSI_INT_CTRL) | INT_CMD_DMA_DONE);      /* ack a stale DONE */

    if (C->UseDma && C->DmaVa != NULL) {
        RtlCopyMemory(C->DmaVa, pkt, size);
        KeMemoryBarrier();
        Wr(C, DSI_TPG_CTRL, 0);
        Wr(C, DSI_DMA_CMD_OFFSET, C->DmaPa.LowPart);
        Wr(C, DSI_DMA_CMD_LENGTH, size);
    } else {
        Wr(C, DSI_TPG_CTRL, (1u << 1) | (1u << 2) | (3u << 16));      /* CMD_DMA_TPG_EN, FIFO mode, custom */
        for (i = 0; i < size; i += 4) {
            Wr(C, DSI_TPG_DMA_INIT, (ULONG)pkt[i] | ((ULONG)pkt[i + 1] << 8) | ((ULONG)pkt[i + 2] << 16) |
                                    ((ULONG)pkt[i + 3] << 24));
        }
        if ((size / 4) & 1) {
            Wr(C, DSI_TPG_DMA_INIT, 0);             /* the FIFO wants an even dword count */
        }
        Wr(C, DSI_TPG_FIFO_RESET, 1);
        KeStallExecutionProcessor(1);
        Wr(C, DSI_TPG_FIFO_RESET, 0);
        Wr(C, DSI_DMA_CMD_LENGTH, size);
    }
    dmaCtrl = Rd(C, DSI_CMD_DMA_CTRL);
    Wr(C, DSI_CMD_DMA_CTRL, dmaCtrl & ~DMA_CTRL_LOW_POWER);           /* HS, as the DT doze commands */
    Wr(C, DSI_DMA_SW_TRIGGER, 1);

    for (waited = 0; waited < 50; waited++) {       /* up to ~50 ms: a frame is ~8-17 ms */
        if (Rd(C, DSI_INT_CTRL) & INT_CMD_DMA_DONE) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    Wr(C, DSI_INT_CTRL, Rd(C, DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(C, DSI_TPG_CTRL, 0);
    Wr(C, DSI_CMD_DMA_CTRL, dmaCtrl);
    Wr(C, DSI_CTRL, ctrl);
    if (trig != Rd(C, DSI_TRIG_CTRL)) {
        Wr(C, DSI_TRIG_CTRL, trig);
    }
    LogPrint("DSI %s dt %02x cmd %02x len %u: %s after %u ms (status %08x fifo %08x smmu gfsr %08x)\n",
             C->UseDma ? "DMA" : "FIFO", Dt, Len ? Payload[0] : 0, Len, done ? "done" : "NO DONE", waited,
             Rd(C, DSI_STATUS), Rd(C, DSI_TPG_FIFO_STATUS),
             C->Smmu ? READ_REGISTER_ULONG((volatile ULONG *)(C->Smmu + SMMU_GFSR)) : 0);
    return done;
}

static VOID SetPercent(PDEVICE_CONTEXT C, LONG Pct)
{
    UCHAR cmd[3];
    ULONG level = (ULONG)Pct * BL_MAX / 100;

    if (level < 1) {
        level = 1;                                  /* 0 would turn the panel black */
    }
    cmd[0] = 0x51;                                  /* SET_DISPLAY_BRIGHTNESS */
    cmd[1] = (UCHAR)(level >> 8);                   /* bl-inverted-dbv: MSB first */
    cmd[2] = (UCHAR)level;
    LogPrint("brightness %d%% -> level %u (0x%03x)\n", Pct, level, level);
    DsiSend(C, 0x39, cmd, sizeof(cmd));
}

/* C:\topaz\dsicmd: "<DT> <bytes...>" in hex; sent once, then the file is deleted. */
static VOID RawCommand(PDEVICE_CONTEXT C)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\dsicmd");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[96];
    UCHAR bytes[16];
    ULONG n = 0, i, v = 0;
    BOOLEAN inNum = FALSE;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0))) {
        return;
    }
    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
        for (i = 0; i <= iosb.Information && n < sizeof(bytes); i++) {
            CHAR c = buf[i];
            ULONG d = (c >= '0' && c <= '9') ? (ULONG)(c - '0') : (c >= 'a' && c <= 'f') ? (ULONG)(c - 'a' + 10) :
                      (c >= 'A' && c <= 'F') ? (ULONG)(c - 'A' + 10) : 16;
            if (d < 16) {
                v = (v << 4) | d;
                inNum = TRUE;
            } else if (inNum) {
                bytes[n++] = (UCHAR)v;
                v = 0;
                inNum = FALSE;
            }
        }
    }
    ZwClose(h);                                     /* deletes the file */
    if (n >= 1) {
        LogPrint("dsicmd: %u bytes\n", n);
        DsiSend(C, bytes[0], bytes + 1, n - 1);
    }
}

static KSTART_ROUTINE PollThread;
static VOID PollThread(PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;
    LONG pct;

    period.QuadPart = -10000LL * POLL_MS;
    do {
        RawCommand(ctx);
        pct = ReadPercent();
        if (pct >= 0 && pct != ctx->LastPct) {
            SetPercent(ctx, pct);
            ctx->LastPct = pct;
        }
    } while (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- Hardware ------------------------------------------------------------------- */

static NTSTATUS HwInit(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;

    pa.QuadPart = (LONGLONG)DSI0_PA;
    Ctx->Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, DSI0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->Dsi == NULL) {
        LogPrint("map DSI0 failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    LogPrint("DSI0: hw %08x ctrl %08x status %08x video %08x dma_ctrl %08x trig %08x int %08x tpg %08x\n",
             Rd(Ctx, DSI_HW_VERSION), Rd(Ctx, DSI_CTRL), Rd(Ctx, DSI_STATUS), Rd(Ctx, DSI_VIDEO_MODE_CTRL),
             Rd(Ctx, DSI_CMD_DMA_CTRL), Rd(Ctx, DSI_TRIG_CTRL), Rd(Ctx, DSI_INT_CTRL), Rd(Ctx, DSI_TPG_CTRL));
    pa.QuadPart = (LONGLONG)APPS_SMMU_PA;
    Ctx->Smmu = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    {
        PHYSICAL_ADDRESS lo, hi, skip;
        lo.QuadPart = 0;
        hi.QuadPart = 0xFFFFFFFF;
        skip.QuadPart = 0;
        UNREFERENCED_PARAMETER(skip);
        Ctx->DmaVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
        if (Ctx->DmaVa != NULL) {
            Ctx->DmaPa = MmGetPhysicalAddress(Ctx->DmaVa);
        }
    }
    /* v0.4: the DMA path is the default (it is the only one that reaches the panel); C:\topaz\dsi.fifo forces the
     * old TPG FIFO path for experiments. */
    Ctx->UseDma = (Ctx->DmaVa != NULL);
    {
        UNICODE_STRING fn = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\dsi.fifo");
        OBJECT_ATTRIBUTES oa;
        IO_STATUS_BLOCK iosb;
        HANDLE h;
        InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL, 0, FILE_SHARE_READ,
                                    FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
            ZwClose(h);
            Ctx->UseDma = FALSE;
        }
    }
    LogPrint("DMA buffer %p pa %llx, smmu gfsr %08x, path %s\n", Ctx->DmaVa, Ctx->DmaPa.QuadPart,
             Ctx->Smmu ? READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Smmu + SMMU_GFSR)) : 0,
             Ctx->UseDma ? "DMA (default)" : "TPG FIFO (C:\\topaz\\dsi.fifo)");
    if (!(Rd(Ctx, DSI_CTRL) & CTRL_ENABLE)) {
        LogPrint("DSI0 not enabled: leaving the panel alone\n");
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

static VOID HwDeinit(PDEVICE_CONTEXT Ctx)
{
    Ctx->HwReady = FALSE;
    if (Ctx->Dsi != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Dsi, DSI0_SIZE);
        Ctx->Dsi = NULL;
    }
    if (Ctx->Smmu != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Smmu, 0x1000);
        Ctx->Smmu = NULL;
    }
    if (Ctx->DmaVa != NULL) {
        MmFreeContiguousMemorySpecifyCache(Ctx->DmaVa, PAGE_SIZE, MmNonCached);
        Ctx->DmaVa = NULL;
    }
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazBacklight " TOPAZ_BL_VERSION " ====\n");
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
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(DeviceGetContext(device), sizeof(DEVICE_CONTEXT));
        DeviceGetContext(device)->LastPct = -1;
    }
    return status;
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
    UNREFERENCED_PARAMETER(Translated);
    HwDeinit(DeviceGetContext(Device));
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
    ctx->LastPct = -1;                              /* re-apply after a power transition */
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
