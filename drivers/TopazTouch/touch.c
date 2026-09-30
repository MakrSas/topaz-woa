/*
 * FocalTech touch controller -> Windows multi-touch digitizer via VHF.
 * v0.2: polled (GPIO80 level + periodic poll), no ACPI, root-enumerated.
 */
#include "driver.h"

#define REPORTID_TOUCH  0x01
#define REPORTID_MAXCNT 0x02

#define LE16(v) (UCHAR)((v) & 0xFF), (UCHAR)(((v) >> 8) & 0xFF)

/* Physical size of the 6.67" 1080x2400 panel in 0.01 cm units */
#define PHYS_X 690
#define PHYS_Y 1535

#define FINGER_COLLECTION(maxx, maxy)                                   \
    0x05, 0x0D,        /*   Usage Page (Digitizer)            */        \
    0x09, 0x22,        /*   Usage (Finger)                    */        \
    0xA1, 0x02,        /*   Collection (Logical)              */        \
    0x09, 0x42,        /*     Usage (Tip Switch)              */        \
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,         \
    0x95, 0x07, 0x81, 0x03, /* 7 bit padding                  */        \
    0x09, 0x51,        /*     Usage (Contact Identifier)      */        \
    0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,                     \
    0x05, 0x01,        /*     Usage Page (Generic Desktop)    */        \
    0x75, 0x10, 0x55, 0x0E, 0x65, 0x11, 0x35, 0x00,                     \
    0x26, LE16(maxx), 0x46, LE16(PHYS_X), 0x09, 0x30, 0x81, 0x02,       \
    0x26, LE16(maxy), 0x46, LE16(PHYS_Y), 0x09, 0x31, 0x81, 0x02,       \
    0x55, 0x00, 0x65, 0x00, 0x45, 0x00,                                 \
    0xC0

#define MAXX TS_RAW_MAX_X
#define MAXY TS_RAW_MAX_Y

static const UCHAR g_ReportDescriptor[] = {
    0x05, 0x0D,             /* Usage Page (Digitizer) */
    0x09, 0x04,             /* Usage (Touch Screen)   */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, REPORTID_TOUCH,
    FINGER_COLLECTION(MAXX, MAXY), FINGER_COLLECTION(MAXX, MAXY),
    FINGER_COLLECTION(MAXX, MAXY), FINGER_COLLECTION(MAXX, MAXY),
    FINGER_COLLECTION(MAXX, MAXY), FINGER_COLLECTION(MAXX, MAXY),
    FINGER_COLLECTION(MAXX, MAXY), FINGER_COLLECTION(MAXX, MAXY),
    FINGER_COLLECTION(MAXX, MAXY), FINGER_COLLECTION(MAXX, MAXY),
    0x05, 0x0D,
    0x09, 0x54,             /* Usage (Contact Count) */
    0x15, 0x00, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x85, REPORTID_MAXCNT,
    0x09, 0x55,             /* Usage (Contact Count Maximum) */
    0x25, TS_MAX_CONTACTS, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,
    0xC0
};

#pragma pack(push, 1)
typedef struct _TOUCH_FINGER {
    UCHAR  Tip;
    UCHAR  Id;
    USHORT X;
    USHORT Y;
} TOUCH_FINGER;

typedef struct _TOUCH_REPORT {
    UCHAR        ReportId;
    TOUCH_FINGER Finger[TS_MAX_CONTACTS];
    UCHAR        Count;
} TOUCH_REPORT;
#pragma pack(pop)

/* ---- VHF ------------------------------------------------------------------ */

static VOID EvtVhfGetFeature(PVOID ClientContext, VHFOPERATIONHANDLE Op, PVOID OpContext, PHID_XFER_PACKET Packet)
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(ClientContext);
    UNREFERENCED_PARAMETER(OpContext);

    if (Packet->reportId == REPORTID_MAXCNT && Packet->reportBufferLen >= 2) {
        Packet->reportBuffer[0] = REPORTID_MAXCNT;
        Packet->reportBuffer[1] = TS_MAX_CONTACTS;
        status = STATUS_SUCCESS;
    }
    VhfAsyncOperationComplete(Op, status);
}

NTSTATUS TouchVhfCreate(PDEVICE_CONTEXT Ctx)
{
    VHF_CONFIG cfg;
    NTSTATUS status;

    VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(Ctx->Device),
                    (USHORT)sizeof(g_ReportDescriptor), (PUCHAR)g_ReportDescriptor);
    cfg.VhfClientContext = Ctx;
    cfg.VendorID = 0x2808;   /* FocalTech */
    cfg.ProductID = 0x6225;  /* SM6225 */
    cfg.VersionNumber = 0x0001;
    cfg.EvtVhfAsyncOperationGetFeature = EvtVhfGetFeature;

    status = VhfCreate(&cfg, &Ctx->Vhf);
    LogPrint("VhfCreate: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = VhfStart(Ctx->Vhf);
    LogPrint("VhfStart: %08x\n", status);
    Ctx->VhfStarted = NT_SUCCESS(status);
    return status;
}

VOID TouchVhfDelete(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Vhf != NULL) {
        VhfDelete(Ctx->Vhf, TRUE);
        Ctx->Vhf = NULL;
        Ctx->VhfStarted = FALSE;
    }
}

/* ---- Hardware ------------------------------------------------------------- */

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

NTSTATUS TouchHwInit(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    UCHAR id1 = 0, id2 = 0, fw = 0, vendor = 0;

    status = TlmmPinMap(&Ctx->PinSda, TLMM_TILE_WEST, TS_PIN_I2C_SDA);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinScl, TLMM_TILE_WEST, TS_PIN_I2C_SCL);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinIrq, TLMM_TILE_WEST, TS_PIN_IRQ);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinReset, TLMM_TILE_WEST, TS_PIN_RESET);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinAvdd, TLMM_TILE_EAST, TS_PIN_AVDD);
    LogPrint("TLMM map: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    LogPrint("TLMM before: sda ctl=%08x scl ctl=%08x irq ctl=%08x io=%08x rst ctl=%08x io=%08x avdd ctl=%08x io=%08x\n",
             MmioRead32(&Ctx->PinSda.Regs, TLMM_CTL), MmioRead32(&Ctx->PinScl.Regs, TLMM_CTL),
             MmioRead32(&Ctx->PinIrq.Regs, TLMM_CTL), MmioRead32(&Ctx->PinIrq.Regs, TLMM_IO),
             MmioRead32(&Ctx->PinReset.Regs, TLMM_CTL), MmioRead32(&Ctx->PinReset.Regs, TLMM_IO),
             MmioRead32(&Ctx->PinAvdd.Regs, TLMM_CTL), MmioRead32(&Ctx->PinAvdd.Regs, TLMM_IO));

    /* qupv3_se2_i2c_active: gpio6/7 func qup2, 2 mA, no bias */
    TlmmConfig(&Ctx->PinSda, TS_PIN_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);
    TlmmConfig(&Ctx->PinScl, TS_PIN_I2C_FUNC, TLMM_PULL_NONE, 2, FALSE);
    /* ts_int_active: gpio80 input, 8 mA, pull-up */
    TlmmConfig(&Ctx->PinIrq, 0, TLMM_PULL_UP, 8, FALSE);
    /* ts_avdd: gpio36 output high */
    TlmmSetOutput(&Ctx->PinAvdd, TRUE);
    TlmmConfig(&Ctx->PinAvdd, 0, TLMM_PULL_UP, 8, TRUE);

    status = GccEnableQup0Se2();
    if (!NT_SUCCESS(status)) {
        LogPrint("GCC enable failed: %08x (continuing)\n", status);
    }
    status = GeniI2cInit(&Ctx->Bus, QUP0_SE2_BASE);
    LogPrint("GeniI2cInit: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* First try without reset: the IC may still run the firmware Android loaded. */
    status = GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, FTS_REG_CHIP_ID, &id1, 1);
    LogPrint("probe (no reset): chip_id=%02x status=%08x irq=%08x\n", id1, status, Ctx->Bus.LastIrqStatus);

    /* Reset pulse (ts_reset_active: gpio86 output, pull-up) */
    TlmmSetOutput(&Ctx->PinReset, FALSE);
    TlmmConfig(&Ctx->PinReset, 0, TLMM_PULL_UP, 8, TRUE);
    SleepMs(20);
    TlmmSetOutput(&Ctx->PinReset, TRUE);
    SleepMs(250);

    status = GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, FTS_REG_CHIP_ID, &id1, 1);
    LogPrint("chip_id  (0xA3) = %02x status=%08x irq=%08x\n", id1, status, Ctx->Bus.LastIrqStatus);
    GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, FTS_REG_CHIP_ID2, &id2, 1);
    GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, FTS_REG_FW_VER, &fw, 1);
    GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, FTS_REG_VENDOR_ID, &vendor, 1);
    LogPrint("chip_id2 (0x9F) = %02x fw_ver (0xA6) = %02x vendor (0xA8) = %02x irq_line=%u\n",
             id2, fw, vendor, TlmmGetInput(&Ctx->PinIrq));

    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

VOID TouchHwDeinit(PDEVICE_CONTEXT Ctx)
{
    Ctx->HwReady = FALSE;
    GeniI2cDeinit(&Ctx->Bus);
    TlmmPinUnmap(&Ctx->PinSda);
    TlmmPinUnmap(&Ctx->PinScl);
    TlmmPinUnmap(&Ctx->PinIrq);
    TlmmPinUnmap(&Ctx->PinReset);
    TlmmPinUnmap(&Ctx->PinAvdd);
}

/* ---- Polling thread -------------------------------------------------------- */

static VOID TouchProcess(PDEVICE_CONTEXT Ctx, const UCHAR *Buf)
{
    TOUCH_REPORT rpt;
    HID_XFER_PACKET pkt;
    ULONG i, n = 0;
    USHORT mask = 0;

    RtlZeroMemory(&rpt, sizeof(rpt));
    rpt.ReportId = REPORTID_TOUCH;

    for (i = 0; i < TS_MAX_CONTACTS; i++) {
        const UCHAR *p = Buf + 3 + 6 * i;
        UCHAR event = p[0] >> 6;
        UCHAR id = p[2] >> 4;
        ULONG x = ((ULONG)(p[0] & 0x0F) << 8) | p[1];
        ULONG y = ((ULONG)(p[2] & 0x0F) << 8) | p[3];
        BOOLEAN down = (event != 1);                /* 0 down, 1 up, 2 contact, 3 none */

        if (id >= TS_MAX_CONTACTS || event == 3) {
            continue;
        }
        /* the fw keeps an "up" slot for many frames: report the lift only once */
        if (!down && !(Ctx->ActiveMask & (1u << id))) {
            continue;
        }
        if (x > Ctx->MaxRawX || y > Ctx->MaxRawY) {
            Ctx->MaxRawX = max(Ctx->MaxRawX, x);
            Ctx->MaxRawY = max(Ctx->MaxRawY, y);
            if (Ctx->RawLogged < 2000) {
                Ctx->RawLogged++;
                LogPrint("max raw x=%u y=%u\n", Ctx->MaxRawX, Ctx->MaxRawY);
            }
        }
        /* log every touch-down / lift with raw bytes of that slot */
        if ((!down || !(Ctx->ActiveMask & (1u << id))) && Ctx->RawLogged < 2000) {
            Ctx->RawLogged++;
            LogPrint("%s id=%u x=%u y=%u raw=%02x %02x %02x %02x %02x %02x td=%02x\n",
                     event == 1 ? "up  " : "down", id, x, y, p[0], p[1], p[2], p[3], p[4], p[5], Buf[2]);
        }
        rpt.Finger[n].Tip = down ? 1 : 0;
        rpt.Finger[n].Id = id;
        rpt.Finger[n].X = (USHORT)min(x, (ULONG)MAXX);
        rpt.Finger[n].Y = (USHORT)min(y, (ULONG)MAXY);
        Ctx->LastX[id] = rpt.Finger[n].X;
        Ctx->LastY[id] = rpt.Finger[n].Y;
        if (down) {
            mask |= (USHORT)(1u << id);
        }
        n++;
    }
    /* contacts that vanished without an "up" event: report them lifted */
    for (i = 0; i < TS_MAX_CONTACTS && n < TS_MAX_CONTACTS; i++) {
        BOOLEAN inReport = FALSE;
        ULONG j;

        if (!(Ctx->ActiveMask & (1u << i)) || (mask & (1u << i))) {
            continue;
        }
        for (j = 0; j < n; j++) {
            inReport |= (rpt.Finger[j].Id == i);
        }
        if (!inReport) {
            rpt.Finger[n].Tip = 0;
            rpt.Finger[n].Id = (UCHAR)i;
            rpt.Finger[n].X = Ctx->LastX[i];
            rpt.Finger[n].Y = Ctx->LastY[i];
            n++;
        }
    }
    Ctx->ActiveMask = mask;
    rpt.Count = (UCHAR)n;

    if (n == 0 || !Ctx->VhfStarted) {
        return;
    }
    pkt.reportBuffer = (PUCHAR)&rpt;
    pkt.reportBufferLen = sizeof(rpt);
    pkt.reportId = REPORTID_TOUCH;
    VhfReadReportSubmit(Ctx->Vhf, &pkt);
}

static KSTART_ROUTINE TouchThread;

static VOID TouchThread(PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    UCHAR buf[FTS_TOUCH_DATA_LEN];
    LARGE_INTEGER period;
    ULONG errors = 0, polls = 0;
    BOOLEAN wasTouching = FALSE;

    period.QuadPart = -10000LL * 5; /* 5 ms */
    LogPrint("thread started\n");

    while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        NTSTATUS status;
        BOOLEAN irqLow = !TlmmGetInput(&Ctx->PinIrq);

        polls++;
        /* Read on IRQ assertion, while fingers are down, and every ~100 ms as a fallback. */
        if (!irqLow && !wasTouching && (polls % 20) != 0) {
            continue;
        }
        status = GeniI2cReadReg(&Ctx->Bus, TS_I2C_ADDR, 0x00, buf, sizeof(buf));
        if (!NT_SUCCESS(status)) {
            if (errors++ < 20) {
                LogPrint("read error %08x irq=%08x\n", status, Ctx->Bus.LastIrqStatus);
            }
            continue;
        }
        wasTouching = ((buf[2] & 0x0F) != 0 && (buf[2] & 0x0F) <= TS_MAX_CONTACTS) || Ctx->ActiveMask != 0;
        TouchProcess(Ctx, buf);
    }
    LogPrint("thread exit (polls=%u errors=%u)\n", polls, errors);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS TouchThreadStart(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    KeInitializeEvent(&Ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&Ctx->ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, TouchThread, Ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(Ctx->ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       (PVOID *)&Ctx->Thread, NULL);
    ZwClose(Ctx->ThreadHandle);
    Ctx->ThreadHandle = NULL;
    return status;
}

VOID TouchThreadStop(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Thread != NULL) {
        KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
}
