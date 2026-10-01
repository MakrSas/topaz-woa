/*
 * LionSemi ln8000 2:1 charge pump (33 W path), topaz DTB ln8000_charger@51.
 * Register map from Xiaomi's lionsemi/ln8000_charger.{c,h}.
 *
 * v0.9: READ ONLY. Dumps and decodes the registers so the pump's state after Android/UEFI is
 * known before anything is written. Nothing here changes the chip.
 */
#include "driver.h"

#define LN_REG_DEVICE_ID       0x00      /* 0x42 */
#define LN_REG_SYS_STS         0x03      /* bit7 IIN loop, bit6 VFLOAT loop, bit3 bypass, bit2 switching, bit1 standby, bit0 shutdown */
#define LN_REG_SAFETY_STS      0x04
#define LN_REG_FAULT1_STS      0x05      /* bit7 WDT, bit6 VBAT OV, bit4 VAC unplug, bit3 VAC OV, bit1 VIN OV */
#define LN_REG_FAULT2_STS      0x06      /* bit7 IIN OC */
#define LN_REG_IIN_CTRL        0x1B      /* [6:0] IIN limit, 50 mA LSB */
#define LN_REG_REGULATION_CTRL 0x1C
#define LN_REG_SYS_CTRL        0x1E      /* bit3 STANDBY_EN, bit2 REV_IIN_DET, bit0 EN_1TO1 (bypass) */
#define LN_REG_GLITCH_CTRL     0x20      /* [3:2] VAC OVP: 0 6.5 V, 1 11 V, 2 12 V, 3 13 V */
#define LN_REG_FAULT_CTRL      0x21      /* bit6 dis IIN OCP, bit5 dis VBAT OV, bit4 dis VAC OV, bit3 dis VAC UV, bit2 dis VIN OV */
#define LN_REG_ADC_CTRL        0x23      /* [7:5] ADC mode */
#define LN_REG_TIMER_CTRL      0x26      /* bit7 WDT enable */
#define LN_REG_V_FLOAT_CTRL    0x28      /* VFLOAT = 3725 + 5 mV * n */
#define LN_REG_CHARGE_CTRL     0x29      /* bit7: Xiaomi driver's "initialised" mark */
#define LN_REG_PRODUCT_ID      0x31
#define LN_REG_BC_OP_1         0x41
#define LN_REG_BC_STS_B        0x4A
#define LN_NREGS               0x2A

VOID LnDump(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    UCHAR r[LN_NREGS], pid = 0, op1 = 0, stsb = 0;
    CHAR prefix[64];
    ULONG i, errors = 0;
    static const PCSTR vacOvp[4] = { "6.5V", "11V", "12V", "13V" };

    RtlZeroMemory(r, sizeof(r));
    for (i = 0; i < LN_NREGS; i++) {
        if (!NT_SUCCESS(I2cReadByte(&Ctx->Bus, LN8000_ADDR, (UCHAR)i, &r[i]))) {
            errors++;
        }
    }
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_PRODUCT_ID, &pid);
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_BC_OP_1, &op1);
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_BC_STS_B, &stsb);

    RtlStringCbPrintfA(prefix, sizeof(prefix), "ln8000 (%s) 00..29:", Why);
    LogHex(prefix, r, sizeof(r));
    LogPrint("ln8000: id=%02x read_errors=%u sys_sts=%02x(%s%s%s%s%s%s) safety=%02x fault1=%02x fault2=%02x "
             "pid=%02x bc_op1=%02x bc_stsb=%02x\n",
             r[LN_REG_DEVICE_ID], errors, r[LN_REG_SYS_STS],
             (r[LN_REG_SYS_STS] & 0x01) ? "shutdown " : "", (r[LN_REG_SYS_STS] & 0x02) ? "standby " : "",
             (r[LN_REG_SYS_STS] & 0x04) ? "SWITCHING " : "", (r[LN_REG_SYS_STS] & 0x08) ? "BYPASS " : "",
             (r[LN_REG_SYS_STS] & 0x40) ? "vfloat-loop " : "", (r[LN_REG_SYS_STS] & 0x80) ? "iin-loop" : "",
             r[LN_REG_SAFETY_STS], r[LN_REG_FAULT1_STS], r[LN_REG_FAULT2_STS], pid, op1, stsb);
    LogPrint("ln8000 cfg: iin_limit=%umA vfloat=%umV vac_ovp=%s sys_ctrl=%02x(standby_en=%u 1to1=%u rcp=%u) "
             "fault_ctrl=%02x(off: iin_ocp=%u vbat_ov=%u vac_ov=%u vin_ov=%u) regulation=%02x adc_mode=%u "
             "wdt=%u xiaomi_init=%u\n",
             50 * (r[LN_REG_IIN_CTRL] & 0x7F), 3725 + 5 * r[LN_REG_V_FLOAT_CTRL],
             vacOvp[(r[LN_REG_GLITCH_CTRL] >> 2) & 3],
             r[LN_REG_SYS_CTRL], (r[LN_REG_SYS_CTRL] >> 3) & 1, r[LN_REG_SYS_CTRL] & 1, (r[LN_REG_SYS_CTRL] >> 2) & 1,
             r[LN_REG_FAULT_CTRL], (r[LN_REG_FAULT_CTRL] >> 6) & 1, (r[LN_REG_FAULT_CTRL] >> 5) & 1,
             (r[LN_REG_FAULT_CTRL] >> 4) & 1, (r[LN_REG_FAULT_CTRL] >> 2) & 1,
             r[LN_REG_REGULATION_CTRL], r[LN_REG_ADC_CTRL] >> 5, r[LN_REG_TIMER_CTRL] >> 7,
             r[LN_REG_CHARGE_CTRL] >> 7);
}

/*
 * ADC results (read only; the ADC already runs in the mode Android left). One burst over
 * 0x09..0x11 so the channels belong together. Formulas: ln8000_convert_adc_code().
 */
BOOLEAN LnReadAdc(PDEVICE_CONTEXT Ctx, PULONG VinMv, PULONG IinMa, PULONG VbatMv, PULONG TdieRaw)
{
    UCHAR a[9];     /* 0x09 .. 0x11 */

    *VinMv = *IinMa = *VbatMv = *TdieRaw = 0;
    if (!NT_SUCCESS(GeniI2cReadReg(&Ctx->Bus, LN8000_ADDR, 0x09, a, sizeof(a)))) {
        return FALSE;
    }
    *IinMa  = ((((ULONG)a[1] & 0x03) << 8) | a[0]) * 4890 / 1000;                  /* 0x09/0x0A */
    *VinMv  = ((((ULONG)a[3] & 0x3F) << 4) | ((a[2] & 0xF0) >> 4)) * 16;           /* 0x0B/0x0C */
    *VbatMv = ((((ULONG)a[6] & 0x03) << 8) | a[5]) * 5;                            /* 0x0E/0x0F */
    *TdieRaw = (((ULONG)a[8] & 0x0F) << 6) | ((a[7] & 0xFC) >> 2);                 /* 0x10/0x11 */
    return TRUE;
}

/* ---- charge pump control (33 W step 3: first switching test at IBUS 1 A) --------------------
 * Sequence after Xiaomi's ln8000 driver (init_device / set_charging_enable): limits first,
 * reverse-current protection off for the start and on once current flows, STANDBY_EN=0 +
 * EN_1TO1=0 = 2:1 switching. The bq2589x keeps powering the system with its charging off.
 * The PPS voltage is moved in 20 mV steps once a second to hold IBUS near the target.
 */
#define CP_TARGET_MA       1000      /* first test */
#define CP_ABORT_MA        1500      /* software trip on ln8000 IIN */
#define CP_PPS_MA          1500      /* adapter current limit (PPS operating current) */
#define CP_IIN_CTRL_MA     1500      /* ln8000 input limit / OCP reference */
#define CP_VFLOAT_MV       4400      /* nopmi fv-max */
#define CP_VBAT_MAX_MV     4350      /* stop above (ln8000 ADC or fuel gauge) */
#define CP_VBAT_START_MV   4300
#define CP_TEMP_MAX_TENTHS 420
#define CP_SOC_MIN_TENTHS  300
#define CP_SOC_MAX_TENTHS  800
#define CP_GAUGE_MAX_MA    3000      /* battery current at IBUS 1 A is ~2 A */
#define CP_PPS_MAX_MV      9600
#define CP_TEST_STEPS      120       /* seconds */
#define LN_SYS_STANDBY_EN  0x08
#define LN_SYS_REV_IIN_DET 0x04
#define LN_SYS_EN_1TO1     0x01
#define LN_STS_SWITCHING   0x04
#define LN_FAULT1_MASK     0xCA      /* WDT, VBAT OV, VAC OV, VIN OV */
#define LN_FAULT2_IIN_OC   0x80

static NTSTATUS LnUpdate(PDEVICE_CONTEXT Ctx, UCHAR Reg, UCHAR Mask, UCHAR Val)
{
    UCHAR v = 0;
    NTSTATUS s = I2cReadByte(&Ctx->Bus, LN8000_ADDR, Reg, &v);

    if (NT_SUCCESS(s)) {
        s = I2cWriteByte(&Ctx->Bus, LN8000_ADDR, Reg, (UCHAR)((v & ~Mask) | (Val & Mask)));
    }
    return s;
}

static VOID BqCharge(PDEVICE_CONTEXT Ctx, BOOLEAN On)
{
    UCHAR r03 = 0;

    if (NT_SUCCESS(I2cReadByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, &r03))) {
        UCHAR w = On ? (UCHAR)(r03 | CHG_CHG_CONFIG) : (UCHAR)(r03 & ~CHG_CHG_CONFIG);
        if (w != r03) {
            I2cWriteByte(&Ctx->Bus, CHG_ADDR, CHG_REG03, w);
        }
    }
}

/* Pump to standby, bq2589x charging back, fixed PDO. Safe to call at any time. */
VOID CpStop(PDEVICE_CONTEXT Ctx, PCSTR Why)
{
    NTSTATUS s;

    if (!Ctx->CpActive) {
        return;
    }
    s = LnUpdate(Ctx, LN_REG_SYS_CTRL, LN_SYS_STANDBY_EN, LN_SYS_STANDBY_EN);
    Ctx->CpActive = FALSE;
    Ctx->CpRcp = FALSE;
    BqCharge(Ctx, TRUE);    /* JEITA policy re-checks it on the next poll */
    LogPrint("cp: STOP (%s) after %u s: standby (%08x), bq2589x charging on\n", Why, Ctx->CpSteps, s);
    if (Ctx->Pd.State == PD_ST_READY && Ctx->Pd.PpsMv != 0) {
        PdSetFixed(Ctx, "charge pump stopped");
    }
    LnDump(Ctx, "after stop");
}

static BOOLEAN CpSnapshot(PDEVICE_CONTEXT Ctx, PLONG Temp, PULONG Soc, PULONG Vgauge, PLONG Igauge)
{
    KIRQL irql;
    BOOLEAN valid;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    valid = Ctx->Snap.Valid;
    *Temp = Ctx->Snap.TempTenthsC;
    *Soc = Ctx->Snap.SocTenths;
    *Vgauge = Ctx->Snap.VoltageMv;
    *Igauge = Ctx->Snap.CurrentMa;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);
    return valid;
}

static VOID CpStart(PDEVICE_CONTEXT Ctx)
{
    ULONG vin, iin, vbat, tdie, soc, vg, mv;
    LONG temp, ig;
    UCHAR sts = 0, f1 = 0, f2 = 0;
    NTSTATUS s = STATUS_SUCCESS;

    Ctx->CpTried = TRUE;
    if (!CpSnapshot(Ctx, &temp, &soc, &vg, &ig) || temp >= CP_TEMP_MAX_TENTHS || soc < CP_SOC_MIN_TENTHS ||
        soc > CP_SOC_MAX_TENTHS || vg >= CP_VBAT_START_MV) {
        LogPrint("cp: not starting: temp %d soc %u vbat %u (need < %u, %u..%u, < %u)\n", temp, soc, vg,
                 CP_TEMP_MAX_TENTHS, CP_SOC_MIN_TENTHS, CP_SOC_MAX_TENTHS, CP_VBAT_START_MV);
        return;
    }
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_SYS_STS, &sts);
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_FAULT1_STS, &f1);
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_FAULT2_STS, &f2);
    if (!LnReadAdc(Ctx, &vin, &iin, &vbat, &tdie) || (f1 & LN_FAULT1_MASK) || (f2 & LN_FAULT2_IIN_OC) ||
        (sts & LN_STS_SWITCHING) || vbat < 3000 || vbat >= CP_VBAT_START_MV) {
        LogPrint("cp: not starting: ln8000 sts=%02x f1=%02x f2=%02x vbat=%u\n", sts, f1, f2, vbat);
        return;
    }

    /* 1. VBUS = 2 x VBAT + 300 mV (Xiaomi's start point), adapter limited to 1.5 A */
    mv = ((2 * vbat + 300) / 20) * 20;
    mv = max(mv, Ctx->Pd.PpsMinMv);
    mv = min(mv, min(Ctx->Pd.PpsMaxMv, (ULONG)CP_PPS_MAX_MV));
    LogPrint("cp: START test: vbat %umV (gauge %umV, %d.%dC, soc %u.%u%%) -> PPS %umV %umA, target IBUS %umA\n",
             vbat, vg, temp / 10, temp % 10, soc / 10, soc % 10, mv, CP_PPS_MA, CP_TARGET_MA);
    if (!PdSetPps(Ctx, mv, CP_PPS_MA)) {
        LogPrint("cp: PPS request failed, not starting\n");
        PdSetFixed(Ctx, "cp start failed");
        return;
    }
    PdWait(Ctx, 300);
    LnReadAdc(Ctx, &vin, &iin, &vbat, &tdie);
    if (vin + 400 < mv || vin > mv + 400) {
        LogPrint("cp: VBUS %umV does not match PPS %umV, not starting\n", vin, mv);
        PdSetFixed(Ctx, "cp start failed");
        return;
    }

    /* 2. ln8000 limits (still in standby) */
    s |= I2cWriteByte(&Ctx->Bus, LN8000_ADDR, LN_REG_V_FLOAT_CTRL, (UCHAR)((CP_VFLOAT_MV - 3725) / 5));
    s |= LnUpdate(Ctx, LN_REG_IIN_CTRL, 0x7F, (UCHAR)(CP_IIN_CTRL_MA / 50));
    s |= LnUpdate(Ctx, LN_REG_GLITCH_CTRL, 0x0C, 1 << 2);                   /* VAC OVP 11 V */
    s |= LnUpdate(Ctx, LN_REG_FAULT_CTRL, 0x7C, 0x00);                      /* all protections on */
    s |= LnUpdate(Ctx, LN_REG_SYS_CTRL, LN_SYS_REV_IIN_DET, 0);             /* rcp off for the start */
    if (!NT_SUCCESS(s)) {
        LogPrint("cp: ln8000 setup failed (%08x), not starting\n", s);
        PdSetFixed(Ctx, "cp start failed");
        return;
    }
    LnDump(Ctx, "before switching");

    /* 3. hand the battery over: bq2589x charging off, pump switching */
    Ctx->CpActive = TRUE;
    Ctx->CpRcp = FALSE;
    Ctx->CpSteps = 0;
    Ctx->CpPpsMv = mv;
    BqCharge(Ctx, FALSE);
    s = LnUpdate(Ctx, LN_REG_SYS_CTRL, LN_SYS_STANDBY_EN | LN_SYS_EN_1TO1, 0);
    PdWait(Ctx, 100);
    I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_SYS_STS, &sts);
    LnReadAdc(Ctx, &vin, &iin, &vbat, &tdie);
    LogPrint("cp: switching on (%08x): sys_sts=%02x vin=%umV iin=%umA vbat=%umV\n", s, sts, vin, iin, vbat);
    if (!NT_SUCCESS(s) || !(sts & LN_STS_SWITCHING)) {
        CpStop(Ctx, "did not enter switching");
    }
}

/* Called once a second from the Type-C state machine while we are an attached sink. */
VOID CpStep(PDEVICE_CONTEXT Ctx)
{
    ULONG vin, iin, vbat, tdie, soc, vg, mv;
    LONG temp, ig;
    UCHAR sts = 0, f1 = 0, f2 = 0;
    PCSTR why = NULL;

    if (!Ctx->CpActive) {
        if (!Ctx->CpTried && Ctx->Pd.State == PD_ST_READY && Ctx->Pd.PpsPos != 0 && Ctx->Pd.PpsTested) {
            CpStart(Ctx);
        }
        return;
    }
    Ctx->CpSteps++;
    CpSnapshot(Ctx, &temp, &soc, &vg, &ig);
    if (!NT_SUCCESS(I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_SYS_STS, &sts)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_FAULT1_STS, &f1)) ||
        !NT_SUCCESS(I2cReadByte(&Ctx->Bus, LN8000_ADDR, LN_REG_FAULT2_STS, &f2)) ||
        !LnReadAdc(Ctx, &vin, &iin, &vbat, &tdie)) {
        CpStop(Ctx, "ln8000 i2c error");
        return;
    }
    LogPrint("cp %3u: vin=%umV iin=%umA vbat=%umV pps=%umV | gauge %umV %dmA %d.%dC | sts=%02x f1=%02x f2=%02x rcp=%u\n",
             Ctx->CpSteps, vin, iin, vbat, Ctx->CpPpsMv, vg, ig, temp / 10, temp % 10, sts, f1, f2, Ctx->CpRcp);

    if ((f1 & LN_FAULT1_MASK) || (f2 & LN_FAULT2_IIN_OC))      why = "ln8000 fault";
    else if (!(sts & LN_STS_SWITCHING))                         why = "ln8000 left switching";
    else if (iin > CP_ABORT_MA)                                 why = "IBUS above 1.5 A";
    else if (vbat >= CP_VBAT_MAX_MV || vg >= CP_VBAT_MAX_MV)    why = "VBAT limit";
    else if (temp >= CP_TEMP_MAX_TENTHS)                        why = "battery temperature";
    else if (ig > CP_GAUGE_MAX_MA)                              why = "battery current above 3 A";
    else if (vin < 2 * vbat + 50)                               why = "VBUS too low (reverse current risk)";
    else if (Ctx->Pd.State != PD_ST_READY || Ctx->Pd.PpsMv == 0) why = "PD contract lost";
    else if (Ctx->CpSteps >= CP_TEST_STEPS)                     why = "test done (120 s)";
    if (why != NULL) {
        CpStop(Ctx, why);
        return;
    }

    /* reverse current protection once current flows (Xiaomi: IIN > 200 mA, headroom > 300 mV) */
    if (!Ctx->CpRcp && iin > 200 && vin > 2 * vbat + 300) {
        LnUpdate(Ctx, LN_REG_SYS_CTRL, LN_SYS_REV_IIN_DET, LN_SYS_REV_IIN_DET);
        Ctx->CpRcp = TRUE;
        LogPrint("cp: reverse current protection on\n");
    }

    /* hold IBUS near the target with 20 mV PPS steps */
    mv = Ctx->CpPpsMv;
    if (iin + 100 < CP_TARGET_MA && vin < 2 * vbat + 800) {
        mv += 20;
    } else if (iin > CP_TARGET_MA + 50) {
        mv -= 20;
    }
    mv = min(mv, min(Ctx->Pd.PpsMaxMv, (ULONG)CP_PPS_MAX_MV));
    mv = max(mv, 2 * vbat + 100);
    mv = (mv / 20) * 20;
    if (mv != Ctx->CpPpsMv) {
        if (!PdSetPps(Ctx, mv, CP_PPS_MA)) {
            CpStop(Ctx, "PPS step failed");
            return;
        }
        Ctx->CpPpsMv = mv;
    }
}
