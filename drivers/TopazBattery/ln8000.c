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
