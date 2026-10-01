/*
 * Redmi Note 12 4G (topaz, SM6225) battery hardware: QUP0 SE1 I2C with the
 * sm5602 fuel gauge (0x71) and the bq2589x-compatible charger (0x6A).
 * Register formulas from Xiaomi's "nopmi" stack (spes kernel, drivers/power/supply/nopmi).
 */
#pragma once

/* ---- TLMM ---------------------------------------------------------------- */
#define TLMM_BASE            0x00400000ULL
#define TLMM_TILE_WEST       0x00100000ULL
#define TLMM_PIN_STRIDE      0x1000
#define TLMM_PIN_PA(tile, pin) (TLMM_BASE + (tile) + (ULONGLONG)(pin) * TLMM_PIN_STRIDE)
#define TLMM_CTL             0x0      /* pull[1:0] func[5:2] drv[8:6] oe[9] */
#define TLMM_IO              0x4      /* in[0] out[1] */
#define TLMM_PULL_NONE       0
#define TLMM_DRV_MA(ma)      (((ma) / 2) - 1)

/* ---- GCC ----------------------------------------------------------------- */
#define GCC_BASE             0x01400000ULL
#define GCC_APCS_VOTE_REG    0x7900C
#define GCC_QUP0_VOTE_BITS   ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11)) /* m/s ahb, core, core2x, s1 */
#define GCC_QUP0_S1_CBCR     0x1F274
#define GCC_QUP0_S1_RCG      0x1F278
#define GCC_RCG_PERF_DFSR(l) (0x1C + 4 * (l))   /* QUP RCGs run in DFS mode */
#define GCC_CBCR_CLK_OFF     (1u << 31)

/* ---- QUPv3 GENI SE1 (I2C) ------------------------------------------------ */
#define QUP0_SE1_BASE        0x04A84000ULL
#define GENI_SE_SIZE         0x4000
#define QUP1_PIN_SDA         4
#define QUP1_PIN_SCL         5
#define QUP1_FUNC            1

#define GENI_FORCE_DEFAULT_REG   0x20
#define GENI_OUTPUT_CTRL         0x24
#define GENI_CGC_CTRL            0x28
#define SE_GENI_STATUS           0x40
#define GENI_SER_M_CLK_CFG       0x48
#define GENI_FW_REVISION_RO      0x68
#define SE_GENI_CLK_SEL          0x7C
#define SE_GENI_BYTE_GRAN        0x254
#define SE_GENI_DMA_MODE_EN      0x258
#define SE_GENI_TX_PACKING_CFG0  0x260
#define SE_GENI_TX_PACKING_CFG1  0x264
#define SE_I2C_TX_TRANS_LEN      0x26C
#define SE_I2C_RX_TRANS_LEN      0x270
#define SE_I2C_SCL_COUNTERS      0x278
#define SE_GENI_RX_PACKING_CFG0  0x284
#define SE_GENI_RX_PACKING_CFG1  0x288
#define SE_GENI_M_CMD0           0x600
#define SE_GENI_M_CMD_CTRL_REG   0x604
#define SE_GENI_M_IRQ_STATUS     0x610
#define SE_GENI_M_IRQ_EN         0x614
#define SE_GENI_M_IRQ_CLEAR      0x618
#define SE_GENI_S_IRQ_CLEAR      0x648
#define SE_GENI_TX_FIFOn         0x700
#define SE_GENI_RX_FIFOn         0x780
#define SE_GENI_TX_FIFO_STATUS   0x800
#define SE_GENI_RX_FIFO_STATUS   0x804
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR        0xC44
#define SE_DMA_RX_IRQ_CLR        0xD44
#define SE_GSI_EVENT_EN          0xE18
#define SE_IRQ_EN                0xE1C
#define SE_HW_PARAM_0            0xE24

#define GENI_PROTO_I2C           3
#define M_GENI_CMD_ACTIVE        (1u << 0)
#define M_GENI_CMD_ABORT         (1u << 1)
#define M_GENI_CMD_CANCEL        (1u << 2)
#define M_CMD_DONE_EN            (1u << 0)
#define M_CMD_CANCEL_EN          (1u << 4)
#define M_GP_IRQ_1_EN            (1u << 10)  /* NACK */
#define M_I2C_ERR_MASK           ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13))
#define M_RX_FIFO_WATERMARK_EN   (1u << 26)
#define M_RX_FIFO_LAST_EN        (1u << 27)
#define TX_FIFO_WC_MASK          0x0FFFFFFFu
#define RX_FIFO_WC_MASK          0x01FFFFFFu
#define I2C_OP_WRITE             0x1
#define I2C_OP_READ              0x2
#define M_OPCODE_SHIFT           27
#define I2C_STOP_STRETCH         (1u << 2)
#define I2C_SLV_ADDR_SHIFT       9
/* 400 kHz from 19.2 MHz (geni_i2c_clk_map) */
#define I2C_400K_CLK_DIV         2
#define I2C_400K_T_HIGH          5
#define I2C_400K_T_LOW           12
#define I2C_400K_T_CYCLE         24

/* ---- sm5602 fuel gauge (16-bit SMBus words, little endian) --------------- */
#define FG_ADDR                  0x71
#define FG_REG_DEVICE_ID         0x00
#define FG_REG_STATUS            0x04      /* bit9 battery present */
#define FG_REG_SOC               0x05      /* [14:8] %, [7:0] 1/256 %, bit15 sign */
#define FG_REG_OCV               0x06
#define FG_REG_VOLTAGE           0x07      /* mV = 1800*(d&0x7FFF)/19622 + 2700 */
#define FG_REG_CURRENT           0x08      /* mA = (d&0x7FFF)*1000/4088 * (10/rsns), bit15 sign */
#define FG_REG_TEMP_IN           0x09      /* die temperature, [7:0] degC */
#define FG_REG_TEMP_EX           0x0A      /* NTC, lookup in FgThermalTable */
#define FG_REG_SOC_CYCLE         0x0B      /* [8:0] cycles */
#define FG_REG_FG_OP_STATUS      0x11
#define FG_REG_BAT_CAP           0x62      /* mAh = (d&0x7FFF)*1000/2048 */
#define FG_STATUS_BATT_PRESENT   (1u << 9)
#define FG_RSNS_FACTOR           2         /* DT sm,rsns = 0 -> rsns 5 -> 10/5 */

/* ---- charger (bq25890-compatible) ---------------------------------------- */
/* Register map: TI bq25890 datasheet (SLUSBX6); the part answers REG14 PN=001 (clone, same map). */
#define CHG_ADDR                 0x6A
#define CHG_REG00                0x00      /* bit7 EN_HIZ, bit6 EN_ILIM, [5:0] IINLIM = 100 + 50*n mA */
#define CHG_REG02                0x02      /* bit7 CONV_START, bit6 CONV_RATE, bit4 ICO_EN, bit3 HVDCP_EN, bit2 MAXC_EN, bit1 FORCE_DPDM, bit0 AUTO_DPDM_EN */
#define CHG_REG03                0x03      /* bit5 OTG_CONFIG, bit4 CHG_CONFIG */
#define CHG_REG04                0x04      /* bit7 EN_PUMPX, [6:0] ICHG = 64*n mA */
#define CHG_REG06                0x06      /* [7:2] VREG = 3840 + 16*n mV */
#define CHG_REG07                0x07      /* bit7 EN_TERM, [5:4] WATCHDOG, bit3 EN_TIMER */
#define CHG_REG0B                0x0B      /* [7:5] VBUS_STAT, [4:3] CHRG_STAT, bit2 PG_STAT */
#define CHG_REG0C                0x0C      /* faults (read to clear) */
#define CHG_REG0D                0x0D      /* bit7 FORCE_VINDPM, [6:0] VINDPM = 2600 + 100*n mV */
#define CHG_REG0E                0x0E      /* bit7 THERM_STAT, [6:0] BATV = 2304 + 20*n mV (ADC) */
#define CHG_REG0F                0x0F      /* [6:0] SYSV = 2304 + 20*n mV (ADC) */
#define CHG_REG11                0x11      /* bit7 VBUS_GD, [6:0] VBUSV = 2600 + 100*n mV (ADC) */
#define CHG_REG12                0x12      /* [6:0] ICHGR = 50*n mA (ADC) */
#define CHG_REG13                0x13      /* bit7 VDPM_STAT, bit6 IDPM_STAT, [5:0] IDPM_LIM = 100 + 50*n mA (effective input limit) */
#define CHG_REG14                0x14      /* bit7 REG_RST, bit6 ICO_OPTIMIZED, [5:3] PN, [1:0] DEV_REV */
#define CHG_NREGS                0x15
#define CHG_CONV_START           0x80
#define CHG_CONV_RATE            0x40
#define CHG_FORCE_DPDM           0x02
#define CHG_OTG_CONFIG           0x20
#define CHG_CHG_CONFIG           0x10
#define CHG_VBUS_STAT(r)         (((r) >> 5) & 7)  /* 0 none, 1 SDP, 2 CDP, 3 DCP, 4 HVDCP, 5 unknown, 6 non-std, 7 OTG */
#define CHG_CHRG_STAT(r)         (((r) >> 3) & 3)  /* 0 not charging, 1 pre, 2 fast, 3 done */
#define CHG_PG_STAT(r)           (((r) >> 2) & 1)

/* ---- battery pack (topaz: 5000 mAh nominal, 4.45 V max) ------------------ */
#define BATT_DESIGN_MAH          5000
#define BATT_NOMINAL_MV          3870
