/*
 * Redmi Note 12 4G (topaz/tapas, SM6225 "khaje") hardware addresses.
 * Sources: vendor DTB (/sys/firmware/fdt), MiCode topaz-t-oss
 * drivers/clk/qcom/gcc-khaje.c, drivers/pinctrl/qcom/pinctrl-khaje.c,
 * include/linux/qcom-geni-se.h, drivers/i2c/busses/i2c-qcom-geni.c.
 */
#pragma once

/* ---- TLMM (pinctrl) ---------------------------------------------------- */
#define TLMM_BASE            0x00400000ULL
#define TLMM_TILE_WEST       0x00100000ULL
#define TLMM_TILE_SOUTH      0x00500000ULL
#define TLMM_TILE_EAST       0x00900000ULL
#define TLMM_PIN_STRIDE      0x1000
#define TLMM_PIN_PA(tile, pin) (TLMM_BASE + (tile) + (ULONGLONG)(pin) * TLMM_PIN_STRIDE)

#define TLMM_CTL             0x0      /* pull[1:0] func[5:2] drv[8:6] oe[9] */
#define TLMM_IO              0x4      /* in[0] out[1] */
#define TLMM_PULL_NONE       0
#define TLMM_PULL_DOWN       1
#define TLMM_PULL_UP         3
#define TLMM_DRV_MA(ma)      (((ma) / 2) - 1)

/* ---- GCC ---------------------------------------------------------------- */
#define GCC_BASE             0x01400000ULL
#define GCC_APCS_VOTE_REG    0x7900C  /* gcc_qupv3_* enable_reg */
#define GCC_QUP0_M_AHB_BIT   6
#define GCC_QUP0_S_AHB_BIT   7
#define GCC_QUP0_CORE_BIT    8
#define GCC_QUP0_CORE2X_BIT  9
#define GCC_QUP0_S1_BIT      11       /* gcc_qupv3_wrap0_s1_clk (charger/typec bus) */
#define GCC_QUP0_S2_BIT      12       /* gcc_qupv3_wrap0_s2_clk (touch bus) */
#define GCC_QUP0_S2_CBCR     0x1F3A4  /* halt_reg of gcc_qupv3_wrap0_s2_clk */
#define GCC_QUP0_S2_RCG_CMD  0x1F3A8
#define GCC_QUP0_S1_RCG_CMD  0x1F278
#define GCC_RCG_CMD_UPDATE   (1u << 0)
#define GCC_RCG_CFG_OFF      0x4      /* CFG_RCGR: src_sel[10:8] div[4:0] */
#define GCC_CBCR_CLK_OFF     (1u << 31)

/* ---- QUPv3 GENI serial engines ----------------------------------------- */
#define QUP0_SE1_BASE        0x04A84000ULL /* i2c: bq2589x@6a, rt1711h@4e, sc8551, ln8000 */
#define QUP0_SE2_BASE        0x04A88000ULL /* i2c: focaltech@38 (touch) */
#define GENI_SE_SIZE         0x4000

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
#define SE_GENI_TX_WATERMARK_REG 0x80C
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR        0xC44
#define SE_DMA_RX_IRQ_CLR        0xD44
#define SE_GSI_EVENT_EN          0xE18
#define SE_IRQ_EN                0xE1C
#define SE_HW_PARAM_0            0xE24
#define SE_DMA_GENERAL_CFG       0xE30

#define GENI_PROTO_I2C           3
#define M_GENI_CMD_ACTIVE        (1u << 0)
#define M_GENI_CMD_CANCEL        (1u << 2)
#define M_GENI_CMD_ABORT         (1u << 1)
#define M_CMD_DONE_EN            (1u << 0)
#define M_CMD_OVERRUN_EN         (1u << 1)
#define M_ILLEGAL_CMD_EN         (1u << 2)
#define M_CMD_FAILURE_EN         (1u << 3)
#define M_CMD_CANCEL_EN          (1u << 4)
#define M_CMD_ABORT_EN           (1u << 5)
#define M_GP_IRQ_1_EN            (1u << 10)  /* I2C NACK */
#define M_GP_IRQ_3_EN            (1u << 12)  /* bus protocol error */
#define M_GP_IRQ_4_EN            (1u << 13)  /* arbitration lost */
#define M_RX_FIFO_WATERMARK_EN   (1u << 26)
#define M_RX_FIFO_LAST_EN        (1u << 27)
#define M_TX_FIFO_WATERMARK_EN   (1u << 30)
#define M_I2C_ERR_MASK (M_CMD_OVERRUN_EN | M_ILLEGAL_CMD_EN | M_CMD_FAILURE_EN | \
                        M_CMD_ABORT_EN | M_GP_IRQ_1_EN | M_GP_IRQ_3_EN | M_GP_IRQ_4_EN)
#define TX_FIFO_WC_MASK          0x0FFFFFFFu
#define RX_FIFO_WC_MASK          0x01FFFFFFu
#define RX_LAST                  (1u << 31)
#define RX_LAST_BYTE_VALID_SHIFT 28

#define I2C_OP_WRITE             0x1
#define I2C_OP_READ              0x2
#define M_OPCODE_SHIFT           27
#define I2C_STOP_STRETCH         (1u << 2)
#define I2C_SLV_ADDR_SHIFT       9

/* 400 kHz from 19.2 MHz XO: {clk_div, t_high, t_low, t_cycle} (geni_i2c_clk_map) */
#define I2C_400K_CLK_DIV         2
#define I2C_400K_T_HIGH          5
#define I2C_400K_T_LOW           12
#define I2C_400K_T_CYCLE         24

/* ---- Touch (FocalTech "focaltech,fts" @0x38 on SE2) --------------------- */
#define TS_I2C_ADDR              0x38
#define TS_PIN_I2C_SDA           6    /* WEST, func 1 = qup2 */
#define TS_PIN_I2C_SCL           7
#define TS_PIN_I2C_FUNC          1
#define TS_PIN_IRQ               80   /* WEST, active low, pull-up */
#define TS_PIN_RESET             86   /* WEST, active low */
#define TS_PIN_AVDD              36   /* EAST, output high */
#define TS_MAX_CONTACTS          10
#define TS_DISPLAY_X             1080
#define TS_DISPLAY_Y             2400

#define FTS_REG_CHIP_ID          0xA3
#define FTS_REG_CHIP_ID2         0x9F
#define FTS_REG_FW_VER           0xA6
#define FTS_REG_VENDOR_ID        0xA8
#define FTS_TOUCH_DATA_LEN       (3 + 6 * TS_MAX_CONTACTS)
