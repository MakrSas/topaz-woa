/*
 * Speaker amp detection (P9 step A4), read-only. The topaz dtbo (board-id 0x30022) lists three
 * amps on QUP0 SE1 I2C, chosen at probe time by the stock kernel: fs16xx @0x34, aw87xxx @0x59,
 * sia81xx @0x2b (sound card default "sia81x9", one-wire mode); shared reset GPIO 106.
 *
 * TopazBattery owns that GENI SE (configured it, polls it from its thread about once a second)
 * and there is no lock between drivers. So: no SE init here, only register reads/writes of
 * single transfers, and only right after a TopazBattery burst ended (SE seen active, then idle
 * for 30 ms), which leaves us most of a second. Runs once when C:\topaz\amp.probe exists.
 */
#include "Audio.h"

#define Out ModemOut

#define QUP0_SE1_PA          0x04A84000u
#define GENI_FW_REVISION_RO  0x68
#define SE_GENI_STATUS       0x40
#define GENI_SER_M_CLK_CFG   0x48
#define SE_GENI_CLK_SEL      0x7C
#define SE_I2C_TX_TRANS_LEN  0x26C
#define SE_I2C_RX_TRANS_LEN  0x270
#define SE_GENI_M_CMD0       0x600
#define SE_GENI_M_CMD_CTRL   0x604
#define SE_GENI_M_IRQ_STATUS 0x610
#define SE_GENI_M_IRQ_CLEAR  0x618
#define SE_GENI_TX_FIFOn     0x700
#define SE_GENI_RX_FIFOn     0x780
#define SE_GENI_RX_FIFO_STATUS 0x804
#define M_GENI_CMD_ACTIVE    (1u << 0)
#define M_CMD_DONE           (1u << 0)
#define M_CMD_CANCEL         (1u << 4)
#define M_NACK               (1u << 10)
#define M_I2C_ERR_MASK       ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 10) | (1u << 12) | (1u << 13))
#define I2C_OP_WRITE         1u
#define I2C_OP_READ          2u
#define I2C_STOP_STRETCH     (1u << 2)

STATIC UINTN mSe;

#define SE_RD(o)     MmioRead32 (mSe + (o))
#define SE_WR(o, v)  MmioWrite32 (mSe + (o), (v))

/* 0 ok, 1 NACK, 2 other error, 3 timeout (same handling as TopazBattery hwio.c) */
STATIC UINT32 WaitDone(VOID)
{
  UINT32 i, st = 0;

  for (i = 0; i < 20000; i++) {
    st = SE_RD (SE_GENI_M_IRQ_STATUS);
    if (st & (M_CMD_DONE | M_I2C_ERR_MASK)) {
      break;
    }
    KeStallExecutionProcessor (1);
  }
  SE_WR (SE_GENI_M_IRQ_CLEAR, st);
  if ((st & M_I2C_ERR_MASK) || !(st & M_CMD_DONE)) {
    SE_WR (SE_GENI_M_CMD_CTRL, 1u << 2);         /* cancel */
    for (i = 0; i < 1000 && !(SE_RD (SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
      KeStallExecutionProcessor (1);
    }
    if (SE_RD (SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
      SE_WR (SE_GENI_M_CMD_CTRL, 1u << 1);       /* abort */
      KeStallExecutionProcessor (100);
    }
    SE_WR (SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    return (st & M_NACK) ? 1 : (st & M_I2C_ERR_MASK) ? 2 : 3;
  }
  return 0;
}

/* write WLen (<= 16) bytes (no stop if RLen != 0), then read RLen (<= 16) bytes */
STATIC UINT32 Xfer(UINT8 Addr, CONST UINT8 *W, UINT32 WLen, UINT8 *R, UINT32 RLen)
{
  UINT32 r, got = 0, spins = 0, w, i, n;

  if (WLen != 0) {
    SE_WR (SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
    SE_WR (SE_I2C_TX_TRANS_LEN, WLen);
    SE_WR (SE_GENI_M_CMD0, (I2C_OP_WRITE << 27) | ((UINT32)Addr << 9) | (RLen != 0 ? I2C_STOP_STRETCH : 0));
    for (i = 0; i < WLen; i += 4) {             /* 16 bytes = 4 words, fits any TX FIFO */
      w = 0;
      n = MIN (4u, WLen - i);
      CopyMem (&w, W + i, n);
      SE_WR (SE_GENI_TX_FIFOn, w);
    }
    r = WaitDone ();
    if (r != 0 || RLen == 0) {
      return r;
    }
  }
  SE_WR (SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  SE_WR (SE_I2C_RX_TRANS_LEN, RLen);
  SE_WR (SE_GENI_M_CMD0, (I2C_OP_READ << 27) | ((UINT32)Addr << 9));
  while (got < RLen && spins < 20000) {
    if (SE_RD (SE_GENI_M_IRQ_STATUS) & M_I2C_ERR_MASK) {
      break;
    }
    if ((SE_RD (SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFFu) == 0) {
      KeStallExecutionProcessor (1);
      spins++;
      continue;
    }
    w = SE_RD (SE_GENI_RX_FIFOn);
    n = MIN (4u, RLen - got);
    CopyMem (R + got, &w, n);
    got += n;
  }
  r = WaitDone ();
  return (r == 0 && got < RLen) ? 3 : r;
}

STATIC UINT32 ReadReg(UINT8 Addr, UINT8 Reg, UINT8 *Buf, UINT32 Len)
{
  return Xfer (Addr, &Reg, 1, Buf, Len);
}

/* Wait for the end of a TopazBattery burst (SE active seen, then 30 ms idle) or 3 s without any
   activity. FALSE: no quiet window within 5 s, don't touch the bus. */
STATIC BOOLEAN WaitQuiet(BOOLEAN *Seen)
{
  UINT64 t0 = KeQueryInterruptTime (), idleSince = 0, now;
  BOOLEAN seen = FALSE;

  *Seen = FALSE;
  for (;;) {
    now = KeQueryInterruptTime ();
    if (SE_RD (SE_GENI_STATUS) & M_GENI_CMD_ACTIVE) {
      seen = TRUE;
      idleSince = 0;
    } else if (idleSince == 0) {
      idleSince = now;
    }
    if (idleSince != 0 && now - idleSince >= 300000 && (seen || now - t0 >= 30000000)) {
      *Seen = seen;
      return TRUE;
    }
    if (now - t0 >= 50000000) {
      return FALSE;
    }
    KeStallExecutionProcessor (20);
  }
}

/* Map SE1 once and check that TopazBattery configured it as an I2C master. */
STATIC BOOLEAN BusOpen(BOOLEAN Log)
{
  UINT32 fw, mclk;

  if (mSe == 0) {
    mSe = (UINTN)MapPhys (QUP0_SE1_PA, SIZE_16KB, FALSE);
  }
  if (mSe == 0) {
    return FALSE;
  }
  fw = SE_RD (GENI_FW_REVISION_RO);
  mclk = SE_RD (GENI_SER_M_CLK_CFG);
  if (Log) {
    Out ("  SE1 fw_rev %08x (proto %u) clk_sel %x m_clk_cfg %x status %08x\r\n", fw, (fw >> 8) & 0xFF,
         SE_RD (SE_GENI_CLK_SEL), mclk, SE_RD (SE_GENI_STATUS));
  }
  return ((fw >> 8) & 0xFF) == 3 && (mclk & 1) != 0;
}

/* Lab: one transaction in a quiet window. 0 ok, 1 NACK, 2 error, 3 timeout, 4 no bus/window. */
UINT32 AmpI2cXfer(UINT8 Addr, CONST UINT8 *W, UINT32 WLen, UINT8 *R, UINT32 RLen)
{
  BOOLEAN seen;
  KIRQL irql;
  UINT32 r;

  if (WLen > 16 || RLen > 16 || (WLen == 0 && RLen == 0) || !BusOpen (FALSE) || !WaitQuiet (&seen)) {
    return 4;
  }
  KeRaiseIrql (DISPATCH_LEVEL, &irql);
  r = Xfer (Addr, W, WLen, R, RLen);
  KeLowerIrql (irql);
  return r;
}

STATIC CONST CHAR8 *Res(UINT32 R)
{
  static CONST CHAR8 *n[] = { "ok", "NACK", "error", "timeout" };
  return n[R & 3];
}

VOID AmpProbe(VOID)
{
  STATIC CONST struct { UINT8 Addr; CONST CHAR8 *Name; UINT8 Reg; UINT8 Len; } probe[] = {
    { 0x34, "fs16xx (FourSemi), id reg 0x03", 0x03, 2 },
    { 0x59, "aw87xxx (Awinic), chip id reg 0x00", 0x00, 1 },
    { 0x2B, "sia81xx (SI-in), reg 0x00", 0x00, 1 },
  };
  UINT32 i, r[ARRAY_SIZE (probe)];
  UINT8 v[ARRAY_SIZE (probe)][4];
  BOOLEAN seen = FALSE;
  KIRQL irql;

  Out ("\r\n  ==== speaker amp probe (QUP0 SE1, read-only) ====\r\n");
  /* TopazBattery must have configured it (I2C proto, 400 kHz divider) or we stay off the bus */
  if (!BusOpen (TRUE)) {
    Out ("  SE1 is not a configured I2C master: skipped\r\n");
    return;
  }
  if (!WaitQuiet (&seen)) {
    Out ("  no quiet window on the bus within 5 s: skipped\r\n");
    return;
  }
  /* all transfers back to back inside the quiet window (< 2 ms) */
  KeRaiseIrql (DISPATCH_LEVEL, &irql);
  for (i = 0; i < ARRAY_SIZE (probe); i++) {
    *(UINT32 *)v[i] = 0;
    r[i] = ReadReg (probe[i].Addr, probe[i].Reg, v[i], probe[i].Len);
  }
  KeLowerIrql (irql);
  Out ("  window after %a\r\n", seen ? "a TopazBattery burst" : "3 s of bus silence");
  for (i = 0; i < ARRAY_SIZE (probe); i++) {
    Out ("  0x%02x %-36a: %a", probe[i].Addr, probe[i].Name, Res (r[i]));
    if (r[i] == 0) {
      Out (" value %02x %02x (%u byte%a)", v[i][0], v[i][1], probe[i].Len, probe[i].Len > 1 ? "s" : "");
    }
    Out ("\r\n");
  }
}
