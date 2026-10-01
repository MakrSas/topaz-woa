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

static VOID TcpcDump(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    USHORT alert = 0, amask = 0;
    UCHAR tc = 0, role = 0, pc = 0, cc = 0, pwr = 0, flt = 0, c8 = 0, c11 = 0, c14 = 0;

    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT, &alert);
    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT_MASK, &amask);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TCPC_CTRL, &tc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, &role);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_CTRL, &pc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, &cc);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_STATUS, &pwr);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_FAULT_STATUS, &flt);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL8, &c8);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL11, &c11);
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL14, &c14);
    LogPrint("rt1711h %s: alert=%04x mask=%04x tcpc_ctrl=%02x role=%02x power_ctrl=%02x cc_status=%02x "
             "power_status=%02x fault=%02x rtctrl8=%02x rtctrl11=%02x rtctrl14=%02x\n",
             Why, alert, amask, tc, role, pc, cc, pwr, flt, c8, c11, c14);
}

/*
 * rt1711h Type-C controller (no PD). The vendor setup is what Linux rt1711h_init() writes;
 * without "shipping off" the CC comparators may be idle and CC_STATUS never shows a charger
 * (v0.5: cc_status stayed 00 with an adapter plugged). Roles: TcStep() below.
 */
static VOID TcpcInit(PDEVICE_CONTEXT Ctx)
{
    USHORT vid = 0, pid = 0;
    UCHAR role = 0;
    NTSTATUS t, w = STATUS_SUCCESS;

    Ctx->TcpcOk = FALSE;
    t = I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_VID, &vid);
    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_PID, &pid);
    LogPrint("rt1711h: vid=%04x pid=%04x (%08x)\n", vid, pid, t);
    if (!NT_SUCCESS(t) || vid != 0x29CF) {
        LogPrint("rt1711h: not found -> cc detect OFF\n");
        return;
    }
    TcpcDump(Ctx, "before");
    w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL8, RT1711H_RTCTRL8_INIT);
    w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL11, RT1711H_RTCTRL11_INIT);
    w |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, RT1711H_RTCTRL14, RT1711H_RTCTRL14_INIT);
    {
        UCHAR clr[3] = { TCPC_REG_ALERT, 0xFF, 0xFF };   /* clear all alerts (W1C) */
        w |= GeniI2cWrite(&Ctx->Bus, TCPC_ADDR, clr, sizeof(clr), TRUE);
    }
    KeStallExecutionProcessor(2000);    /* CC debounce (tTCPCfilter ~0.4 ms) */
    TcpcDump(Ctx, "after");
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, &role);
    Ctx->TcpcOk = NT_SUCCESS(w);
    LogPrint("rt1711h: init writes %08x role=%02x -> %s\n", w, role, Ctx->TcpcOk ? "ok" : "FAILED");
}

/* ---- Type-C role state machine (1 s steps from the poll thread) ----------------------------
 * TOGGLING: rt1711h alternates Rp/Rd looking for a partner, OTG off.
 * SRC:      partner shows Rd (hub / OTG adapter): we are Rp, OTG boost on.
 * SNK:      partner shows Rp (charger): we are Rd, OTG off, charging per JEITA.
 * FALLBACK: CC unusable: the old VBUS_STAT-only policy (OTG on whenever nothing is plugged).
 */
#define TC_FALLBACK 0
#define TC_TOGGLING 1
#define TC_SRC      2
#define TC_SNK      3
static const PCSTR g_TcNames[] = { "fallback", "toggling", "source(hub)", "sink(charger)" };

static NTSTATUS ChgSetOtg(PDEVICE_CONTEXT Ctx, BOOLEAN On, PCSTR Why)
{
    UCHAR r03 = 0, want;
    NTSTATUS s = I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03);

    if (!NT_SUCCESS(s)) {
        return s;
    }
    want = On ? (UCHAR)(r03 | CHG_OTG_CONFIG) : (UCHAR)(r03 & ~CHG_OTG_CONFIG);
    if (want != r03) {
        s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, want);
        LogPrint("otg %s (%s): reg03 %02x -> %02x (%08x)\n", On ? "on" : "off", Why, r03, want, s);
    }
    return s;
}

static BOOLEAN TcReadCc(PDEVICE_CONTEXT Ctx, PUCHAR Cc)
{
    /* rt1711h auto-idles after 32 ms; the first transfer after idle may only wake it */
    return NT_SUCCESS(I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, Cc)) ||
           NT_SUCCESS(I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_CC_STATUS, Cc));
}

static VOID TcSetState(PDEVICE_CONTEXT Ctx, ULONG State, PCSTR Why)
{
    LogPrint("type-c: %s -> %s (%s)\n", g_TcNames[Ctx->TcState], g_TcNames[State], Why);
    Ctx->TcState = State;
    Ctx->TcCandidate = 0;
    Ctx->TcDetachSteps = 0;
    Ctx->TcVbusNoCcSteps = 0;
    Ctx->TcChanged = TRUE;
}

static VOID TcStartToggling(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    NTSTATUS a, b;

    ChgSetOtg(Ctx, FALSE, Why);
    a = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_DRP_TOGGLE);
    b = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_COMMAND, TCPC_CMD_LOOK4CONNECTION);
    if (!NT_SUCCESS(a) || !NT_SUCCESS(b)) {
        LogPrint("type-c: start toggling failed (%08x %08x)\n", a, b);
    }
    TcSetState(Ctx, TC_TOGGLING, Why);
}

static VOID TcFallback(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    /* plain sink terminations: a charger always sees Rd and keeps VBUS on */
    I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
    TcSetState(Ctx, TC_FALLBACK, Why);
}

static VOID TcStep(PDEVICE_CONTEXT Ctx)
{
    UCHAR cc = 0, r0b = 0, pwr = 0;
    ULONG c1, c2, vbus;

    if (Ctx->TcState == TC_FALLBACK) {
        return;
    }
    if (!TcReadCc(Ctx, &cc) || !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0B, &r0b))) {
        return;     /* bus trouble: change nothing */
    }
    c1 = TCPC_CC1(cc);
    c2 = TCPC_CC2(cc);
    vbus = CHG_VBUS_STAT(r0b);
    if (cc != Ctx->TcLastCc) {
        LogPrint("type-c: [%s] cc_status %02x -> %02x (looking=%u term=%s cc1=%u cc2=%u) vbus_stat=%u\n",
                 g_TcNames[Ctx->TcState], Ctx->TcLastCc, cc, (cc & TCPC_CC_LOOKING) ? 1 : 0,
                 (cc & TCPC_CC_TERM_RD) ? "Rd" : "Rp", c1, c2, vbus);
        Ctx->TcLastCc = cc;
    }

    switch (Ctx->TcState) {
    case TC_TOGGLING: {
        ULONG cand = 0;

        if (!(cc & TCPC_CC_LOOKING)) {
            if (cc & TCPC_CC_TERM_RD) {
                if (c1 != 0 || c2 != 0) {
                    cand = TC_SNK;                  /* we were Rd and saw Rp */
                }
            } else if (c1 == TCPC_CC_SRC_RD || c2 == TCPC_CC_SRC_RD) {
                cand = TC_SRC;                      /* we were Rp and saw Rd */
            }
            if (cand == 0) {
                /* toggling stopped on Ra / noise: look again (no OTG change, it is already off) */
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_DRP_TOGGLE);
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_COMMAND, TCPC_CMD_LOOK4CONNECTION);
            }
        }
        if (cand != 0 && cand == Ctx->TcCandidate) {
            if (cand == TC_SRC) {
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SRC_RP_RP);
                ChgSetOtg(Ctx, TRUE, "hub attached");
                TcSetState(Ctx, TC_SRC, "partner Rd");
            } else {
                I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
                ChgSetOtg(Ctx, FALSE, "charger attached");
                TcSetState(Ctx, TC_SNK, "partner Rp");
            }
            return;
        }
        Ctx->TcCandidate = cand;
        /* a charger gives VBUS only after it saw our Rd, so VBUS without any CC result = CC is broken */
        if (cand == 0 && vbus >= 1 && vbus <= 6) {
            if (++Ctx->TcVbusNoCcSteps >= 5) {
                TcFallback(Ctx, "charger VBUS for 5 s but CC never resolved: CC unusable");
            }
        } else {
            Ctx->TcVbusNoCcSteps = 0;
        }
        break;
    }
    case TC_SRC:
        /* hub gone: neither CC shows Rd any more */
        if (c1 != TCPC_CC_SRC_RD && c2 != TCPC_CC_SRC_RD) {
            if (++Ctx->TcDetachSteps >= 2) {
                TcStartToggling(Ctx, "hub removed");
            }
        } else {
            Ctx->TcDetachSteps = 0;
        }
        break;
    case TC_SNK:
        /* charger gone: CC open, or no VBUS seen by both the charger and the TCPC */
        I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_POWER_STATUS, &pwr);
        if ((c1 == 0 && c2 == 0) || (vbus == 0 && !(pwr & TCPC_VBUS_PRESENT))) {
            if (++Ctx->TcDetachSteps >= 2) {
                TcStartToggling(Ctx, "charger removed");
            }
        } else {
            Ctx->TcDetachSteps = 0;
        }
        break;
    }
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

    TcpcInit(Ctx);
    Ctx->TcLastCc = 0xFF;
    Ctx->TcState = TC_FALLBACK;
    if (Ctx->TcpcOk) {
        Ctx->TcState = TC_TOGGLING;     /* toggling itself starts on D0 entry (BattThreadStart) */
    } else {
        TcFallback(Ctx, "rt1711h not usable");
    }
    Ctx->LastVbusStat = (ULONG)~0;
    Ctx->JeitaZone = (ULONG)~0;
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

/* Charger registers 00..14 in one log line (read only). */
static VOID ChargerDump(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    UCHAR r[CHG_NREGS];
    CHAR prefix[64];
    ULONG i;

    RtlZeroMemory(r, sizeof(r));
    for (i = 0; i < CHG_NREGS; i++) {
        if (i == CHG_REG0C) {
            continue;   /* faults are read-to-clear: leave them to whoever needs them */
        }
        I2cReadByte(&Ctx->Bus, CHG_ADDR, (UCHAR)i, &r[i]);
    }
    RtlStringCbPrintfA(prefix, sizeof(prefix), "charger regs (%s) 00..14:", Why);
    LogHex(prefix, r, sizeof(r));
    LogPrint("charger cfg: iinlim=%umA en_ilim=%u hiz=%u ichg=%umA vreg=%umV vindpm=%umV(force=%u) "
             "reg02=%02x(ico=%u hvdcp=%u maxc=%u auto_dpdm=%u) wdog=%u timer=%u pn=%u rev=%u ico_done=%u\n",
             100 + 50 * (r[CHG_REG00] & 0x3F), (r[CHG_REG00] >> 6) & 1, r[CHG_REG00] >> 7,
             64 * (r[CHG_REG04] & 0x7F), 3840 + 16 * (r[CHG_REG06] >> 2),
             2600 + 100 * (r[CHG_REG0D] & 0x7F), r[CHG_REG0D] >> 7,
             r[CHG_REG02], (r[CHG_REG02] >> 4) & 1, (r[CHG_REG02] >> 3) & 1, (r[CHG_REG02] >> 2) & 1, r[CHG_REG02] & 1,
             (r[CHG_REG07] >> 4) & 3, (r[CHG_REG07] >> 3) & 1,
             (r[CHG_REG14] >> 3) & 7, r[CHG_REG14] & 3, (r[CHG_REG14] >> 6) & 1);
}

/*
 * Charger ADC: reads the result of the conversion started by the previous poll, then starts the
 * next one-shot conversion (REG02 CONV_START, self-clearing). Only the ADC trigger bit is written.
 */
static VOID ChargerAdc(PDEVICE_CONTEXT Ctx, BOOLEAN Log)
{
    UCHAR r00 = 0, r02 = 0, r04 = 0, r0e = 0, r0f = 0, r11 = 0, r12 = 0, r13 = 0;
    ULONG vbat, vsys, vbus, ichgr, idpm;
    NTSTATUS s;

    s = I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG02, &r02);
    if (!NT_SUCCESS(s)) {
        return;
    }
    if (Log && !(r02 & CHG_CONV_START)) {
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, &r00);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, &r04);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0E, &r0e);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG0F, &r0f);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG11, &r11);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG12, &r12);
        I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG13, &r13);
        vbat = 2304 + 20 * (r0e & 0x7F);
        vsys = 2304 + 20 * (r0f & 0x7F);
        vbus = (r11 & 0x7F) ? 2600 + 100 * (r11 & 0x7F) : 0;
        ichgr = 50 * (r12 & 0x7F);
        idpm = 100 + 50 * (r13 & 0x3F);
        LogPrint("chg adc: vbus=%umV(gd=%u) vbat=%umV vsys=%umV ichg=%umA pbat=%umW | lim: iinlim=%umA idpm_lim=%umA "
                 "vdpm=%u idpm=%u ichg_set=%umA therm=%u | r00=%02x r02=%02x r04=%02x r0e=%02x r11=%02x r12=%02x r13=%02x\n",
                 vbus, r11 >> 7, vbat, vsys, ichgr, vbat * ichgr / 1000, 100 + 50 * (r00 & 0x3F), idpm,
                 r13 >> 7, (r13 >> 6) & 1, 64 * (r04 & 0x7F), r0e >> 7,
                 r00, r02, r04, r0e, r11, r12, r13);
    } else if (Log) {
        LogPrint("chg adc: conversion still running (r02=%02x)\n", r02);
    }
    if (!(r02 & (CHG_CONV_START | CHG_CONV_RATE))) {
        /* never write FORCE_DPDM back: it would restart input detection */
        I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG02, (UCHAR)((r02 | CHG_CONV_START) & ~CHG_FORCE_DPDM));
    }
}

/*
 * Software JEITA, zones and currents from the topaz DTB (qcom,nopmi-chg), capped to what the
 * bq2589x alone may do. Ordered cold -> hot; MinTenths = lower bound of the zone.
 */
typedef struct _JEITA_ZONE {
    LONG  MinTenths;
    ULONG IchgMa;       /* 0 = do not charge */
    ULONG VregMv;
} JEITA_ZONE;

static const JEITA_ZONE g_Jeita[] = {
    { -1000,    0, 0 },                                   /* below -10 degC: no charging */
    {  -100,  490, CHG_VREG_MV },                         /* -10..0  temp_tn1_to_t0_fcc */
    {     0,  950, CHG_VREG_MV },                         /*   0..5  temp_t0_to_t1_fcc */
    {    50, CHG_ICHG_MAX_MA, CHG_VREG_MV },              /*   5..10 (DTB 2400) */
    {   100, CHG_ICHG_MAX_MA, CHG_VREG_MV },              /*  10..15 (DTB 3900) */
    {   150, CHG_ICHG_MAX_MA, CHG_VREG_MV },              /*  15..48 (DTB 5950, charge-pump path) */
    {   480, CHG_ICHG_HOT_MA, CHG_VREG_HOT_MV },          /*  48..60 (DTB 2350 / 4.10 V) */
    {   600,    0, 0 },                                   /* above 60 degC: no charging */
};
#define JEITA_ZONES (sizeof(g_Jeita) / sizeof(g_Jeita[0]))

static ULONG JeitaZoneOf(LONG Tenths)
{
    ULONG z = 0;
    while (z + 1 < JEITA_ZONES && Tenths >= g_Jeita[z + 1].MinTenths) {
        z++;
    }
    return z;
}

/* Moves to a zone with less current at once; to one with more only 1.5 degC inside it. */
static ULONG JeitaUpdate(PDEVICE_CONTEXT Ctx, LONG Tenths)
{
    ULONG z = JeitaZoneOf(Tenths), last = Ctx->JeitaZone;

    if (last < JEITA_ZONES && z != last && g_Jeita[z].IchgMa >= g_Jeita[last].IchgMa &&
        (JeitaZoneOf(Tenths - 15) != z || JeitaZoneOf(Tenths + 15) != z)) {
        z = last;
    }
    if (z != last) {
        LogPrint("jeita: %d.%dC zone %d -> %u (ichg %umA vreg %umV)\n", Tenths / 10,
                 (Tenths < 0 ? -Tenths : Tenths) % 10, last < JEITA_ZONES ? (LONG)last : -1, z,
                 g_Jeita[z].IchgMa, g_Jeita[z].VregMv);
        Ctx->JeitaZone = z;
    }
    return z;
}

/*
 * Input limit from what the source says it can give: Type-C Rp advert first, then the
 * charger's own BC1.2 result. 0 = leave the charger's own choice alone.
 */
static ULONG InputLimitMa(UCHAR R0B, UCHAR Cc)
{
    ULONG rp = 0;

    if (Cc != 0xFF) {
        rp = max(TCPC_CC1(Cc), TCPC_CC2(Cc));
    }
    if (rp == 3) {
        return CHG_IINLIM_MAX_MA;           /* Rp 3.0 A */
    }
    if (rp == 2) {
        return 1500;                        /* Rp 1.5 A */
    }
    switch (CHG_VBUS_STAT(R0B)) {
    case 2:  return 1500;                   /* CDP */
    case 3:                                 /* DCP */
    case 4:  return CHG_IINLIM_MAX_MA;      /* HVDCP (still 5 V: we don't ask for more) */
    default: return 0;                      /* SDP / unknown / non-standard: keep */
    }
}

/* ICHG / VREG / IINLIM, written only when they differ from what the charger holds. */
static VOID ChargerLimits(PDEVICE_CONTEXT Ctx, ULONG Zone, UCHAR R0B, UCHAR Cc)
{
    UCHAR r00 = 0, r04 = 0, r06 = 0, w00, w04, w06;
    ULONG iin = InputLimitMa(R0B, Cc);

    if (!NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, &r00)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, &r04)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG06, &r06))) {
        return;
    }
    w04 = r04;
    w06 = r06;
    if (g_Jeita[Zone].IchgMa != 0) {
        w04 = (UCHAR)((r04 & 0x80) | (g_Jeita[Zone].IchgMa / 64));                  /* round down */
        w06 = (UCHAR)((r06 & 0x03) | (((g_Jeita[Zone].VregMv - 3840) / 16) << 2));
    }
    w00 = r00;
    if (iin != 0) {
        w00 = (UCHAR)((r00 & 0xC0) | ((iin - 100) / 50));
    }
    if (w00 != r00 || w04 != r04 || w06 != r06) {
        NTSTATUS a = STATUS_SUCCESS, b = STATUS_SUCCESS, c = STATUS_SUCCESS;
        if (w06 != r06) a = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG06, w06);
        if (w04 != r04) b = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG04, w04);
        if (w00 != r00) c = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG00, w00);
        LogPrint("charger limits: zone %u vbus_stat=%u cc=%02x | r00 %02x->%02x (iinlim %umA) r04 %02x->%02x (ichg %umA) "
                 "r06 %02x->%02x (vreg %umV) (%08x %08x %08x)\n",
                 Zone, CHG_VBUS_STAT(R0B), Cc, r00, w00, 100 + 50 * (w00 & 0x3F), r04, w04, 64 * (w04 & 0x7F),
                 r06, w06, 3840 + 16 * (w06 >> 2), a, b, c);
    }
}

/*
 * CHG_CONFIG per JEITA whenever an adapter is in. OTG belongs to the Type-C state machine; only
 * in FALLBACK the old VBUS_STAT policy drives it (boost whenever nothing is plugged).
 */
static VOID ChargerPolicy(PDEVICE_CONTEXT Ctx, UCHAR R03, UCHAR R0B, BOOLEAN Allow)
{
    ULONG vbus = CHG_VBUS_STAT(R0B);
    UCHAR want = R03;
    UCHAR chg = Allow ? CHG_CHG_CONFIG : 0;

    if (vbus >= 1 && vbus <= 6) {
        want = (UCHAR)(((R03 & ~CHG_CHG_CONFIG) | chg) & ~CHG_OTG_CONFIG);  /* adapter: charge (if JEITA allows) */
    } else if (vbus == 0 && Ctx->TcState == TC_FALLBACK) {
        want = (UCHAR)(R03 | CHG_OTG_CONFIG);                               /* nothing: boost for the hub */
    }
    if (want != R03) {
        NTSTATUS s = I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, want);
        LogPrint("charger policy: [%s] vbus_stat=%u reg03 %02x -> %02x (%08x)\n",
                 g_TcNames[Ctx->TcState], vbus, R03, want, s);
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

    if (CHG_VBUS_STAT(s.ChgReg0B) != Ctx->LastVbusStat) {
        CHAR why[32];
        RtlStringCbPrintfA(why, sizeof(why), "vbus_stat %d->%u",
                           Ctx->LastVbusStat == (ULONG)~0 ? -1 : (LONG)Ctx->LastVbusStat, CHG_VBUS_STAT(s.ChgReg0B));
        ChargerDump(Ctx, why);
        Ctx->LastVbusStat = CHG_VBUS_STAT(s.ChgReg0B);
    }

    {
        /* No trusted temperature -> no charging changes beyond the old on/off policy */
        ULONG zone = s.Valid ? JeitaUpdate(Ctx, s.TempTenthsC) : Ctx->JeitaZone;
        BOOLEAN allow = zone >= JEITA_ZONES || g_Jeita[zone].IchgMa != 0;

        ChargerPolicy(Ctx, s.ChgReg03, s.ChgReg0B, allow);
        if (zone < JEITA_ZONES && s.OnLine) {
            /* the Rp advert is only meaningful while we are an attached sink */
            ChargerLimits(Ctx, zone, s.ChgReg0B, Ctx->TcState == TC_SNK ? Ctx->TcLastCc : 0xFF);
        }
    }

    state = (s.OnLine ? 1 : 0) | (s.Charging ? 2 : 0) | (s.ChargeDone ? 4 : 0);
    notifySoc = s.SocTenths / 10;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    Ctx->Snap = s;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);

    notify = state != Ctx->LastNotifiedState || notifySoc != Ctx->LastNotifiedSoc;
    ChargerAdc(Ctx, s.OnLine || (Ctx->Polls % 12) == 0);
    if ((Ctx->Polls++ % 12) == 0 || notify) {    /* every minute, or on change */
        LogPrint("poll: err=%08x st=%04x soc=%04x(%u.%u%%) v=%04x(%umV) i=%04x(%dmA) tex=%04x(%d.%dC) cap=%04x(%umAh) cyc=%u "
                 "chg r03=%02x r0b=%02x online=%u charging=%u done=%u cc=%02x tc=%s\n",
                 e, st, soc, s.SocTenths / 10, s.SocTenths % 10, volt, s.VoltageMv, curr, s.CurrentMa,
                 tex, s.TempTenthsC / 10, (s.TempTenthsC < 0 ? -s.TempTenthsC : s.TempTenthsC) % 10,
                 cap, s.FullMah, s.Cycles, s.ChgReg03, s.ChgReg0B, s.OnLine, s.Charging, s.ChargeDone, Ctx->TcLastCc, g_TcNames[Ctx->TcState]);
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

/*
 * Called on every D0 exit (after the thread stopped): back to plain Rd/Rd so that a charger
 * always sees a sink and gives VBUS while no driver runs (off-mode charging, next boot).
 */
VOID BattTcSinkOnly(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS s;

    if (!Ctx->HwReady || !Ctx->TcpcOk) {
        return;
    }
    s = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, TCPC_ROLE_SINK_RD_RD);
    LogPrint("type-c: [%s] -> Rd/Rd on D0 exit (%08x)\n", g_TcNames[Ctx->TcState], s);
}

/* ---- Poll thread ---------------------------------------------------------- */

static KSTART_ROUTINE BattThread;

static VOID BattThread(PVOID Context)
{
    PDEVICE_CONTEXT Ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;
    ULONG tick = 0;

    period.QuadPart = -10000LL * 1000; /* 1 s: Type-C steps; battery poll every 5 s */
    LogPrint("thread started\n");
    while (KeWaitForSingleObject(&Ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        if (Ctx->HwReady) {
            TcStep(Ctx);
        }
        if (++tick % 5 == 0 || Ctx->TcChanged) {
            Ctx->TcChanged = FALSE;
            BattPoll(Ctx);
        }
    }
    LogPrint("thread exit (polls=%u)\n", Ctx->Polls);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS BattThreadStart(PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    if (Ctx->HwReady && Ctx->TcState != TC_FALLBACK) {
        TcStartToggling(Ctx, "D0 entry");   /* D0 exit left plain Rd/Rd (BattTcSinkOnly) */
    }
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
