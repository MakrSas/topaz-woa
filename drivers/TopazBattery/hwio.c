/*
 * Low level SoC access: MMIO mapping, TLMM pins, GCC clocks, GENI I2C (copy of TopazTouch/hwio.c, SE1).
 * The GENI I2C part is a polled FIFO-mode port of Linux i2c-qcom-geni.c.
 */
#include "driver.h"

/* ---- MMIO ---------------------------------------------------------------- */

NTSTATUS MmioMap(PMMIO_RANGE Range, ULONGLONG Pa, SIZE_T Size)
{
    Range->Pa.QuadPart = (LONGLONG)Pa;
    Range->Size = Size;
    Range->Va = (volatile UCHAR *)MmMapIoSpaceEx(Range->Pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
    return (Range->Va != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID MmioUnmap(PMMIO_RANGE Range)
{
    if (Range->Va != NULL) {
        MmUnmapIoSpace((PVOID)Range->Va, Range->Size);
        Range->Va = NULL;
    }
}

/* ---- TLMM ---------------------------------------------------------------- */

NTSTATUS TlmmPinMap(PTLMM_PIN P, ULONGLONG Tile, ULONG Pin)
{
    P->Pin = Pin;
    return MmioMap(&P->Regs, TLMM_PIN_PA(Tile, Pin), TLMM_PIN_STRIDE);
}

VOID TlmmPinUnmap(PTLMM_PIN P)
{
    MmioUnmap(&P->Regs);
}

VOID TlmmConfig(PTLMM_PIN P, ULONG Func, ULONG Pull, ULONG DriveMa, BOOLEAN Output)
{
    ULONG ctl = (Pull & 0x3) | ((Func & 0xF) << 2) | ((TLMM_DRV_MA(DriveMa) & 0x7) << 6);
    if (Output) {
        ctl |= (1u << 9);
    }
    MmioWrite32(&P->Regs, TLMM_CTL, ctl);
}

VOID TlmmSetOutput(PTLMM_PIN P, BOOLEAN High)
{
    MmioWrite32(&P->Regs, TLMM_IO, High ? 0x2 : 0x0);
}

BOOLEAN TlmmGetInput(PTLMM_PIN P)
{
    return (MmioRead32(&P->Regs, TLMM_IO) & 0x1) != 0;
}

/* ---- GCC ----------------------------------------------------------------- */

/*
 * Vote the QUP0 wrapper + SE1 clocks on and return the DFS perf level that runs the
 * SE clock from XO (19.2 MHz) undivided; the I2C timing table assumes 19.2 MHz.
 * QUP RCGs are in DFS mode, so the RCG itself is never reprogrammed.
 */
NTSTATUS GccEnableQup0Se1(PULONG ClkSel)
{
    MMIO_RANGE vote = {0}, qup = {0};
    NTSTATUS status;
    ULONG cbcr = 0, i, lvl, perf;

    *ClkSel = 0;
    status = MmioMap(&vote, GCC_BASE + (GCC_APCS_VOTE_REG & ~0xFFFu), 0x1000);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = MmioMap(&qup, GCC_BASE + (GCC_QUP0_S1_CBCR & ~0xFFFu), 0x1000);
    if (!NT_SUCCESS(status)) {
        MmioUnmap(&vote);
        return status;
    }

    MmioWrite32(&vote, GCC_APCS_VOTE_REG & 0xFFF, MmioRead32(&vote, GCC_APCS_VOTE_REG & 0xFFF) | GCC_QUP0_VOTE_BITS);
    for (i = 0; i < 1000; i++) {
        cbcr = MmioRead32(&qup, GCC_QUP0_S1_CBCR & 0xFFF);
        if ((cbcr & GCC_CBCR_CLK_OFF) == 0) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    for (lvl = 0; lvl < 8; lvl++) {
        perf = MmioRead32(&qup, (GCC_QUP0_S1_RCG & 0xFFF) + GCC_RCG_PERF_DFSR(lvl));
        if (((perf >> 8) & 7) == 0 && ((perf >> 12) & 3) == 0 && (perf & 0x1F) <= 1) {
            *ClkSel = lvl;
            break;
        }
    }
    LogPrint("GCC: vote=%08x s1 cbcr=%08x (%s) dfs level=%u\n", MmioRead32(&vote, GCC_APCS_VOTE_REG & 0xFFF),
             cbcr, (cbcr & GCC_CBCR_CLK_OFF) ? "OFF" : "on", *ClkSel);

    MmioUnmap(&qup);
    MmioUnmap(&vote);
    return (cbcr & GCC_CBCR_CLK_OFF) ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
}

/* ---- GENI I2C ------------------------------------------------------------ */

static VOID GeniConfigPacking(PGENI_I2C Bus)
{
    /* geni_se_config_packing(se, 8, 4, msb_to_lsb=true, tx, rx) */
    ULONG cfg[4] = {0};
    const int bpw = 8, packWords = 4;
    int idxStart = bpw - 1, idx = idxStart, i;
    int iter = (8 * packWords) / 8;
    ULONG cfg0, cfg1;

    for (i = 0; i < iter; i++) {
        cfg[i] = ((ULONG)idx << 5) | (1u << 4) | ((ULONG)(bpw - 1) << 1);
        idx = ((i + 1) * 8) + idxStart;
    }
    cfg[iter - 1] |= 1u;
    cfg0 = cfg[0] | (cfg[1] << 10);
    cfg1 = cfg[2] | (cfg[3] << 10);

    MmioWrite32(&Bus->Se, SE_GENI_TX_PACKING_CFG0, cfg0);
    MmioWrite32(&Bus->Se, SE_GENI_TX_PACKING_CFG1, cfg1);
    MmioWrite32(&Bus->Se, SE_GENI_RX_PACKING_CFG0, cfg0);
    MmioWrite32(&Bus->Se, SE_GENI_RX_PACKING_CFG1, cfg1);
    MmioWrite32(&Bus->Se, SE_GENI_BYTE_GRAN, bpw / 16);
}

static VOID GeniIrqClear(PGENI_I2C Bus)
{
    MmioWrite32(&Bus->Se, SE_GSI_EVENT_EN, 0);
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFF);
}

NTSTATUS GeniI2cInit(PGENI_I2C Bus, ULONGLONG SeBase, ULONG ClkSel)
{
    NTSTATUS status;
    ULONG v;

    status = MmioMap(&Bus->Se, SeBase, GENI_SE_SIZE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Bus->Proto = (MmioRead32(&Bus->Se, GENI_FW_REVISION_RO) >> 8) & 0xFF;
    Bus->TxDepth = (MmioRead32(&Bus->Se, SE_HW_PARAM_0) >> 16) & 0x3F;
    LogPrint("GENI %llx: fw_rev=%08x proto=%u tx_depth=%u status=%08x\n",
             SeBase, MmioRead32(&Bus->Se, GENI_FW_REVISION_RO), Bus->Proto, Bus->TxDepth,
             MmioRead32(&Bus->Se, SE_GENI_STATUS));
    if (Bus->Proto != GENI_PROTO_I2C) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Bus->TxDepth == 0) {
        Bus->TxDepth = 16;
    }

    /* geni_se_init + io_init + io_set_mode (FIFO) */
    GeniIrqClear(Bus);
    MmioWrite32(&Bus->Se, GENI_CGC_CTRL, MmioRead32(&Bus->Se, GENI_CGC_CTRL) | 0x7F);
    MmioWrite32(&Bus->Se, GENI_OUTPUT_CTRL, 0x7F);
    MmioWrite32(&Bus->Se, GENI_FORCE_DEFAULT_REG, 1);
    MmioWrite32(&Bus->Se, SE_IRQ_EN, MmioRead32(&Bus->Se, SE_IRQ_EN) | (1u << 2) | (1u << 3));
    MmioWrite32(&Bus->Se, SE_GENI_DMA_MODE_EN, MmioRead32(&Bus->Se, SE_GENI_DMA_MODE_EN) & ~1u);
    MmioWrite32(&Bus->Se, SE_GSI_EVENT_EN, 0);
    MmioWrite32(&Bus->Se, SE_GENI_RX_WATERMARK_REG, Bus->TxDepth - 1);
    MmioWrite32(&Bus->Se, SE_GENI_RX_RFR_WATERMARK_REG, Bus->TxDepth);
    v = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_EN);
    v |= M_CMD_DONE_EN | M_I2C_ERR_MASK | M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN;
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_EN, v);
    GeniConfigPacking(Bus);

    /* qcom_geni_i2c_conf: 400 kHz */
    MmioWrite32(&Bus->Se, SE_GENI_CLK_SEL, ClkSel);
    MmioWrite32(&Bus->Se, GENI_SER_M_CLK_CFG, (I2C_400K_CLK_DIV << 4) | 1u);
    MmioWrite32(&Bus->Se, SE_I2C_SCL_COUNTERS,
                (I2C_400K_T_HIGH << 20) | (I2C_400K_T_LOW << 10) | I2C_400K_T_CYCLE);
    return STATUS_SUCCESS;
}

VOID GeniI2cDeinit(PGENI_I2C Bus)
{
    MmioUnmap(&Bus->Se);
}

static VOID GeniCancel(PGENI_I2C Bus)
{
    ULONG i;

    MmioWrite32(&Bus->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_CANCEL);
    for (i = 0; i < 1000; i++) {
        if (MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL_EN) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    if ((MmioRead32(&Bus->Se, SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) != 0) {
        MmioWrite32(&Bus->Se, SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
        KeStallExecutionProcessor(100);
    }
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
}

static NTSTATUS GeniWaitDone(PGENI_I2C Bus)
{
    ULONG i, st = 0;

    for (i = 0; i < 20000; i++) { /* ~20 ms */
        st = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
    Bus->LastIrqStatus = st;
    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, st);

    if (st & M_GP_IRQ_1_EN) {
        GeniCancel(Bus);
        return STATUS_NO_SUCH_DEVICE;          /* NACK */
    }
    if (st & M_I2C_ERR_MASK) {
        GeniCancel(Bus);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    if ((st & M_CMD_DONE_EN) == 0) {
        GeniCancel(Bus);
        return STATUS_IO_TIMEOUT;
    }
    return STATUS_SUCCESS;
}

NTSTATUS GeniI2cWrite(PGENI_I2C Bus, UCHAR Addr, const UCHAR *Buf, ULONG Len, BOOLEAN Stop)
{
    ULONG param = ((ULONG)Addr << I2C_SLV_ADDR_SHIFT) | (Stop ? 0 : I2C_STOP_STRETCH);
    ULONG done = 0, i;

    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_I2C_TX_TRANS_LEN, Len);
    MmioWrite32(&Bus->Se, SE_GENI_M_CMD0, ((ULONG)I2C_OP_WRITE << M_OPCODE_SHIFT) | param);

    while (done < Len) {
        ULONG word = 0, n = min(4u, Len - done);
        for (i = 0; i < 20000; i++) {
            if ((MmioRead32(&Bus->Se, SE_GENI_TX_FIFO_STATUS) & TX_FIFO_WC_MASK) < Bus->TxDepth) {
                break;
            }
            KeStallExecutionProcessor(1);
        }
        RtlCopyMemory(&word, Buf + done, n);
        MmioWrite32(&Bus->Se, SE_GENI_TX_FIFOn, word);
        done += n;
    }
    return GeniWaitDone(Bus);
}

NTSTATUS GeniI2cRead(PGENI_I2C Bus, UCHAR Addr, UCHAR *Buf, ULONG Len)
{
    ULONG param = (ULONG)Addr << I2C_SLV_ADDR_SHIFT;
    ULONG got = 0, spins = 0;

    MmioWrite32(&Bus->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    MmioWrite32(&Bus->Se, SE_I2C_RX_TRANS_LEN, Len);
    MmioWrite32(&Bus->Se, SE_GENI_M_CMD0, ((ULONG)I2C_OP_READ << M_OPCODE_SHIFT) | param);

    while (got < Len && spins < 20000) {
        ULONG words = MmioRead32(&Bus->Se, SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MASK;
        ULONG st = MmioRead32(&Bus->Se, SE_GENI_M_IRQ_STATUS);
        if (st & M_I2C_ERR_MASK) {
            break;
        }
        if (words == 0) {
            KeStallExecutionProcessor(1);
            spins++;
            continue;
        }
        while (words-- && got < Len) {
            ULONG word = MmioRead32(&Bus->Se, SE_GENI_RX_FIFOn);
            ULONG n = min(4u, Len - got);
            RtlCopyMemory(Buf + got, &word, n);
            got += n;
        }
    }
    return (got == Len) ? GeniWaitDone(Bus) : (GeniWaitDone(Bus), STATUS_IO_TIMEOUT);
}

NTSTATUS GeniI2cReadReg(PGENI_I2C Bus, UCHAR Addr, UCHAR Reg, UCHAR *Buf, ULONG Len)
{
    NTSTATUS status = GeniI2cWrite(Bus, Addr, &Reg, 1, FALSE);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return GeniI2cRead(Bus, Addr, Buf, Len);
}

NTSTATUS I2cReadWord(PGENI_I2C Bus, UCHAR Addr, UCHAR Reg, PUSHORT Val)
{
    UCHAR b[2] = {0};
    NTSTATUS status = GeniI2cReadReg(Bus, Addr, Reg, b, 2);
    *Val = (USHORT)(b[0] | (b[1] << 8));
    return status;
}

NTSTATUS I2cReadByte(PGENI_I2C Bus, UCHAR Addr, UCHAR Reg, PUCHAR Val)
{
    return GeniI2cReadReg(Bus, Addr, Reg, Val, 1);
}

NTSTATUS I2cWriteByte(PGENI_I2C Bus, UCHAR Addr, UCHAR Reg, UCHAR Val)
{
    UCHAR b[2] = { Reg, Val };
    return GeniI2cWrite(Bus, Addr, b, 2, TRUE);
}
