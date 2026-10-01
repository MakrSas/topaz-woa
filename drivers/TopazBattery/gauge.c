/*
 * sm5602 fuel gauge + charger readout and the charger policy:
 *  - external power present -> make sure charging is enabled (CHG_CONFIG=1, OTG off)
 *  - no external power      -> keep the OTG boost on so a bus-powered hub can be
 *                              hot-plugged (UEFI TopazOtgDxe turns it on at boot)
 * The fuel gauge is only read: its algorithm/parameters were set up by Android.
 */
#include "driver.h"

/* NTC table from the topaz DTB (sm5602 battery0,thermal_table): -20..80 degC */
static const USHORT g_ThermalTable[101] = {   /* signed 16-bit values */
    0x506e, 0x4f35, 0x4e07, 0x4cc0, 0x4b76, 0x4a25, 0x48bd, 0x475d, 0x45f5, 0x4477,
    0x4304, 0x4175, 0x3fde, 0x3e4a, 0x3ca2, 0x3af6, 0x3945, 0x378a, 0x35cb, 0x33fc,
    0x322b, 0x3059, 0x2e82, 0x2ca0, 0x2ac6, 0x28c6, 0x26e0, 0x24ea, 0x22e8, 0x20ef,
    0x1ee7, 0x1cee, 0x1ae8, 0x18d0, 0x16d6, 0x14ce, 0x12f3, 0x10e4, 0x0ed6, 0x0cc4,
    0x0abc, 0x08bc, 0x06a8, 0x04ad, 0x02a1, 0x009e, 0xf669, 0xf471, 0xf278, 0xf081,
    0xee7b, 0xec8c, 0xeaa1, 0xe8be, 0xe6e1, 0xe505, 0xe332, 0xe162, 0xdf9b, 0xddd9,
    0xdc1f, 0xda67, 0xd8b4, 0xd70b, 0xd565, 0xd3c7, 0xd232, 0xd0a1, 0xcf18, 0xcd94,
    0xcc18, 0xcaa5, 0xc936, 0xc7d3, 0xc670, 0xc519, 0xc3c6, 0xc27a, 0xc136, 0xbff7,
    0xbec1, 0xbd92, 0xbc6e, 0xbb4d, 0xba36, 0xb922, 0xb816, 0xb712, 0xb611, 0xb517,
    0xb422, 0xb334, 0xb249, 0xb164, 0xb085, 0xafad, 0xaed8, 0xae0a, 0xad3e, 0xac78,
    0xabb7,
};

/* Same lookup as sm5602_fg.c _calculate_battery_temp_ex(), but interpolated to 0.1 degC */
static LONG NtcToTenthsC(USHORT Raw)
{
    SHORT v = (SHORT)Raw;
    ULONG i;

    if (Raw >= 0x8001 && Raw <= 0x823B) {
        v = 0;
    }
    if (v >= (SHORT)g_ThermalTable[0]) {
        return -200;
    }
    for (i = 1; i < 101; i++) {
        if (v >= (SHORT)g_ThermalTable[i]) {
            LONG hi = (SHORT)g_ThermalTable[i - 1], lo = (SHORT)g_ThermalTable[i];
            return (LONG)(i - 1) * 10 - 200 + (LONG)((hi - v) * 10 / (hi - lo));
        }
    }
    return 800;
}

NTSTATUS BattHwInit(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG clkSel = 0;
    USHORT id = 0, st = 0, op = 0;
    UCHAR r03 = 0, r0b = 0;

    status = TlmmPinMap(&Ctx->PinSda, TLMM_TILE_WEST, QUP1_PIN_SDA);
    if (NT_SUCCESS(status)) status = TlmmPinMap(&Ctx->PinScl, TLMM_TILE_WEST, QUP1_PIN_SCL);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    LogPrint("TLMM before: sda ctl=%08x scl ctl=%08x\n",
             MmioRead32(&Ctx->PinSda.Regs, TLMM_CTL), MmioRead32(&Ctx->PinScl.Regs, TLMM_CTL));
    /* qupv3_se1_i2c_active: gpio4/5 func qup1, 2 mA, no bias */
    TlmmConfig(&Ctx->PinSda, QUP1_FUNC, TLMM_PULL_NONE, 2, FALSE);
    TlmmConfig(&Ctx->PinScl, QUP1_FUNC, TLMM_PULL_NONE, 2, FALSE);

    status = GccEnableQup0Se1(&clkSel);
    if (!NT_SUCCESS(status)) {
        LogPrint("GCC enable failed: %08x (continuing)\n", status);
    }
    status = GeniI2cInit(&Ctx->Bus, QUP0_SE1_BASE, clkSel);
    LogPrint("GeniI2cInit: %08x\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_DEVICE_ID, &id);
    I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_STATUS, &st);
    I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_FG_OP_STATUS, &op);
    LogPrint("sm5602: id=%04x (%08x) status=%04x op_status=%04x\n", id, status, st, op);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &r0b);
    LogPrint("charger: reg03=%02x reg0b=%02x\n", r03, r0b);

    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

VOID BattHwDeinit(PDEVICE_CONTEXT Ctx)
{
    Ctx->HwReady = FALSE;
    GeniI2cDeinit(&Ctx->Bus);
    TlmmPinUnmap(&Ctx->PinSda);
    TlmmPinUnmap(&Ctx->PinScl);
}

static VOID ChargerPolicy(PDEVICE_CONTEXT Ctx, UCHAR R03, UCHAR R0B)
{
    ULONG vbus = CHG_VBUS_STAT(R0B);
    UCHAR want = R03;

    if (vbus >= 1 && vbus <= 6) {
        want = (UCHAR)((R03 | CHG_CHG_CONFIG) & ~CHG_OTG_CONFIG);   /* adapter: charge */
    } else if (vbus == 0) {
        want = (UCHAR)(R03 | CHG_OTG_CONFIG);                      /* nothing: boost for the hub */
    }
    if (want != R03) {
        NTSTATUS s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, want);
        LogPrint("charger policy: vbus_stat=%u reg03 %02x -> %02x (%08x)\n", vbus, R03, want, s);
    }
}

VOID BattPoll(PDEVICE_CONTEXT Ctx)
{
    BATT_SNAPSHOT s;
    USHORT st = 0, soc = 0, volt = 0, curr = 0, tex = 0, cap = 0, cyc = 0;
    NTSTATUS e = STATUS_SUCCESS;  /* OR of all read statuses: stays NT_SUCCESS only if every read succeeded */
    KIRQL irql;
    ULONG state, notifySoc;
    BOOLEAN notify;

    if (!Ctx->HwReady) {
        return;
    }
    RtlZeroMemory(&s, sizeof(s));

    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_STATUS, &st);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_SOC, &soc);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_VOLTAGE, &volt);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_CURRENT, &curr);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_TEMP_EX, &tex);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_BAT_CAP, &cap);
    e |= I2cReadWord(&Ctx->Bus, FG_ADDR, FG_REG_SOC_CYCLE, &cyc);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &s.ChgReg03);
    I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &s.ChgReg0B);

    s.Valid = NT_SUCCESS(e);
    s.Present = TRUE;   /* non-removable pack; FG_STATUS_BATT_PRESENT is only logged */
    s.SocTenths = (soc & 0x8000) ? 0 : (((soc >> 8) & 0x7F) * 10 + ((soc & 0xFF) * 10) / 256);
    if (s.SocTenths > 1000) {
        s.SocTenths = 1000;
    }
    s.VoltageMv = 1800 * (volt & 0x7FFF) / 19622 + 2700;
    s.CurrentMa = (LONG)((curr & 0x7FFF) * 1000 / 4088) * FG_RSNS_FACTOR;
    if (curr & 0x8000) {
        s.CurrentMa = -s.CurrentMa;
    }
    s.TempTenthsC = NtcToTenthsC(tex);
    s.FullMah = (cap & 0x7FFF) * 1000 / 2048;
    if (s.FullMah < 1000 || s.FullMah > 7000) {
        s.FullMah = BATT_DESIGN_MAH;
    }
    s.Cycles = cyc & 0x1FF;

    s.OnLine = CHG_VBUS_STAT(s.ChgReg0B) >= 1 && CHG_VBUS_STAT(s.ChgReg0B) <= 6 && CHG_PG_STAT(s.ChgReg0B);
    s.Charging = s.OnLine && (CHG_CHRG_STAT(s.ChgReg0B) == 1 || CHG_CHRG_STAT(s.ChgReg0B) == 2);
    s.ChargeDone = s.OnLine && CHG_CHRG_STAT(s.ChgReg0B) == 3;

    ChargerPolicy(Ctx, s.ChgReg03, s.ChgReg0B);

    state = (s.OnLine ? 1 : 0) | (s.Charging ? 2 : 0) | (s.ChargeDone ? 4 : 0);
    notifySoc = s.SocTenths / 10;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    Ctx->Snap = s;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);

    notify = state != Ctx->LastNotifiedState || notifySoc != Ctx->LastNotifiedSoc;
    if ((Ctx->Polls++ % 12) == 0 || notify) {    /* every minute, or on change */
        LogPrint("poll: err=%08x st=%04x soc=%04x(%u.%u%%) v=%04x(%umV) i=%04x(%dmA) tex=%04x(%d.%dC) cap=%04x(%umAh) cyc=%u "
                 "chg r03=%02x r0b=%02x online=%u charging=%u done=%u\n",
                 e, st, soc, s.SocTenths / 10, s.SocTenths % 10, volt, s.VoltageMv, curr, s.CurrentMa,
                 tex, s.TempTenthsC / 10, (s.TempTenthsC < 0 ? -s.TempTenthsC : s.TempTenthsC) % 10,
                 cap, s.FullMah, s.Cycles, s.ChgReg03, s.ChgReg0B, s.OnLine, s.Charging, s.ChargeDone);
    }
    if (notify) {
        Ctx->LastNotifiedState = state;
        Ctx->LastNotifiedSoc = notifySoc;
        if (Ctx->ClassHandle != NULL) {
            BatteryClassStatusNotify(Ctx->ClassHandle);
        }
    }
}

/*
 * Called when the device leaves D0 (system shutdown/restart included). The boost keeps 5 V on
 * VBUS after the SoC powers off and the PMIC then sees a cable and turns the phone back on.
 */
VOID BattOtgOff(PDEVICE_CONTEXT Ctx)
{
    UCHAR r03 = 0;
    NTSTATUS s;

    if (!Ctx->HwReady) {
        return;
    }
    s = I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);
    if (NT_SUCCESS(s) && (r03 & CHG_OTG_CONFIG)) {
        s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, (UCHAR)(r03 & ~CHG_OTG_CONFIG));
    }
    LogPrint("OTG off on D0 exit: reg03=%02x (%08x)\n", r03, s);
}

/* ---- Poll thread ---------------------------------------------------------- */

static KSTART_ROUTINE BattThread;

static VOID BattThread(PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;

    period.QuadPart = -10000LL * 5000; /* 5 s */
    LogPrint("thread started\n");
    while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        BattPoll(Ctx);
    }
    LogPrint("thread exit (polls=%u)\n", Ctx->Polls);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS BattThreadStart(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    KeInitializeEvent(&Ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&Ctx->ThreadHandle, THREAD_ALL_ACCESS, NULL, NULL, NULL, BattThread, Ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(Ctx->ThreadHandle, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       (PVOID *)&Ctx->Thread, NULL);
    ZwClose(Ctx->ThreadHandle);
    Ctx->ThreadHandle = NULL;
    return status;
}

VOID BattThreadStop(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Thread != NULL) {
        KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
}
