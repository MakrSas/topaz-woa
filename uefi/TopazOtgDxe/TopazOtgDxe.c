/*
 * TopazOtgDxe - turn on USB OTG VBUS on Redmi Note 12 4G (topaz/tapas, SM6225).
 *
 * VBUS for USB host mode comes from the TI bq2589x charger boost (I2C 0x6A on
 * QUP0 SE1, GPIO4/5). Qualcomm's UsbPwrCtrlDxe only knows PMIC chargers, so
 * nothing enables the boost. This driver talks GENI I2C directly (polled FIFO,
 * same code as drivers/TopazTouch/hwio.c), disables the bq watchdog (otherwise
 * registers reset to defaults after 40 s) and sets OTG_CONFIG.
 * rt1711h (0x4E) registers are only logged.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Guid/EventGroup.h>
#include <Guid/Acpi.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/EFIPmicPon.h>
#include "AbSlot.h"

#define TAG "TopazOtg: "

/*
 * Everything logged also goes into a buffer that is printed on the screen at
 * ReadyToBoot (and held for a few seconds), so results are visible in RELEASE
 * builds where DEBUG output goes nowhere.
 */
STATIC CHAR16 mLog[4096];
STATIC UINTN  mLogLen;

STATIC VOID LogAdd(CONST CHAR8 *Fmt, ...)
{
  CHAR8   a[256];
  VA_LIST ap;
  UINTN   n;

  VA_START (ap, Fmt);
  n = AsciiVSPrint (a, sizeof (a), Fmt, ap);
  VA_END (ap);
  DEBUG ((DEBUG_ERROR, TAG "%a", a));
  if (mLogLen + n + 2 < ARRAY_SIZE (mLog)) {
    mLogLen += UnicodeSPrintAsciiFormat (mLog + mLogLen, (ARRAY_SIZE (mLog) - mLogLen) * sizeof (CHAR16), "%a", a);
  }
}

#define LOG(...) LogAdd (__VA_ARGS__)

/* TLMM */
#define TLMM_PIN(tile, pin)   (0x00400000UL + (tile) + (pin) * 0x1000UL)
#define TLMM_WEST             0x00100000UL
#define QUP1_I2C_SDA          4
#define QUP1_I2C_SCL          5
#define QUP1_FUNC             1
#define USB_SWITCH_GPIO       66

/* GCC */
#define GCC_BASE              0x01400000UL
#define GCC_QUP_VOTE          (GCC_BASE + 0x7900C)
#define GCC_QUP_VOTE_BITS     ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11))
#define GCC_QUP0_S1_CBCR      (GCC_BASE + 0x1F274)
#define GCC_QUP0_S1_RCG       (GCC_BASE + 0x1F278)
#define RCG_CMD_DFSR          0x14
#define RCG_PERF_DFSR(l)      (0x1C + 4 * (l))

/* GENI SE */
#define SE_BASE               0x04A84000UL
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

#define M_CMD_DONE_EN            (1u << 0)
#define M_CMD_CANCEL_EN          (1u << 4)
#define M_I2C_ERR_MASK           ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | \
                                  (1u << 10) | (1u << 12) | (1u << 13))
#define M_I2C_NACK               (1u << 10)

#define BQ_ADDR                  0x6A
#define TCPC_ADDR                0x4E

STATIC UINT32 mTxDepth = 16;
STATIC UINT32 mLastIrq;

#define SE_R(o)      MmioRead32(SE_BASE + (o))
#define SE_W(o, v)   MmioWrite32(SE_BASE + (o), (v))

STATIC VOID GeniCancel(VOID)
{
  UINTN i;

  SE_W(SE_GENI_M_CMD_CTRL_REG, 1u << 2);         /* cancel */
  for (i = 0; i < 1000 && (SE_R(SE_GENI_M_IRQ_STATUS) & M_CMD_CANCEL_EN) == 0; i++) {
    MicroSecondDelay(1);
  }
  if (SE_R(SE_GENI_STATUS) & 1u) {
    SE_W(SE_GENI_M_CMD_CTRL_REG, 1u << 1);       /* abort */
    MicroSecondDelay(100);
  }
  SE_W(SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
}

STATIC EFI_STATUS GeniWaitDone(VOID)
{
  UINTN i;
  UINT32 st = 0;

  for (i = 0; i < 20000; i++) {
    st = SE_R(SE_GENI_M_IRQ_STATUS);
    if (st & (M_CMD_DONE_EN | M_I2C_ERR_MASK)) {
      break;
    }
    MicroSecondDelay(1);
  }
  mLastIrq = st;
  SE_W(SE_GENI_M_IRQ_CLEAR, st);
  if (st & M_I2C_ERR_MASK) {
    GeniCancel();
    return (st & M_I2C_NACK) ? EFI_NOT_FOUND : EFI_DEVICE_ERROR;
  }
  if ((st & M_CMD_DONE_EN) == 0) {
    GeniCancel();
    return EFI_TIMEOUT;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS I2cWrite(UINT8 Addr, CONST UINT8 *Buf, UINT32 Len, BOOLEAN Stop)
{
  UINT32 done = 0, word, n;
  UINTN i;

  SE_W(SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  SE_W(SE_I2C_TX_TRANS_LEN, Len);
  SE_W(SE_GENI_M_CMD0, (1u << 27) | ((UINT32)Addr << 9) | (Stop ? 0 : (1u << 2)));
  while (done < Len) {
    for (i = 0; i < 20000 && (SE_R(SE_GENI_TX_FIFO_STATUS) & 0x0FFFFFFF) >= mTxDepth; i++) {
      MicroSecondDelay(1);
    }
    word = 0;
    n = MIN (4, Len - done);
    CopyMem (&word, Buf + done, n);
    SE_W(SE_GENI_TX_FIFOn, word);
    done += n;
  }
  return GeniWaitDone ();
}

STATIC EFI_STATUS I2cRead(UINT8 Addr, UINT8 *Buf, UINT32 Len)
{
  UINT32 got = 0, words, word, n;
  UINTN spins = 0;

  SE_W(SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  SE_W(SE_I2C_RX_TRANS_LEN, Len);
  SE_W(SE_GENI_M_CMD0, (2u << 27) | ((UINT32)Addr << 9));
  while (got < Len && spins < 20000) {
    if (SE_R(SE_GENI_M_IRQ_STATUS) & M_I2C_ERR_MASK) {
      break;
    }
    words = SE_R(SE_GENI_RX_FIFO_STATUS) & 0x01FFFFFF;
    if (words == 0) {
      MicroSecondDelay(1);
      spins++;
      continue;
    }
    while (words-- && got < Len) {
      word = SE_R(SE_GENI_RX_FIFOn);
      n = MIN (4, Len - got);
      CopyMem (Buf + got, &word, n);
      got += n;
    }
  }
  if (got != Len) {
    GeniWaitDone ();
    return (mLastIrq & M_I2C_NACK) ? EFI_NOT_FOUND : EFI_TIMEOUT;
  }
  return GeniWaitDone ();
}

STATIC EFI_STATUS RegRead(UINT8 Addr, UINT8 Reg, UINT8 *Val)
{
  EFI_STATUS s = I2cWrite (Addr, &Reg, 1, FALSE);
  if (EFI_ERROR (s)) {
    return s;
  }
  return I2cRead (Addr, Val, 1);
}

STATIC EFI_STATUS RegWrite(UINT8 Addr, UINT8 Reg, UINT8 Val)
{
  UINT8 b[2] = { Reg, Val };
  return I2cWrite (Addr, b, 2, TRUE);
}

STATIC VOID ClocksOn(VOID)
{
  UINT32 cbcr = 0, dfs, perf, lvl, sel = 0;
  UINTN i;

  MmioOr32 (GCC_QUP_VOTE, GCC_QUP_VOTE_BITS);
  for (i = 0; i < 1000; i++) {
    cbcr = MmioRead32 (GCC_QUP0_S1_CBCR);
    if ((cbcr & (1u << 31)) == 0) {
      break;
    }
    MicroSecondDelay(1);
  }
  /* QUP RCGs run in DFS mode: pick the perf level that is XO (19.2 MHz) undivided. */
  dfs = MmioRead32 (GCC_QUP0_S1_RCG + RCG_CMD_DFSR);
  for (lvl = 0; lvl < 8; lvl++) {
    perf = MmioRead32 (GCC_QUP0_S1_RCG + RCG_PERF_DFSR (lvl));
    LOG ("dfs[%u]=%08x\n", lvl, perf);
    if (((perf >> 8) & 7) == 0 && ((perf >> 12) & 3) == 0 && (perf & 0x1F) <= 1) {
      sel = lvl;
      break;
    }
  }
  if (lvl == 8) {
    sel = 0;
  }
  SE_W(SE_GENI_CLK_SEL, sel);
  LOG ("vote=%08x s1_cbcr=%08x cmd_dfsr=%08x clk_sel=%u\n",
          MmioRead32 (GCC_QUP_VOTE), cbcr, dfs, sel);
}

STATIC EFI_STATUS GeniInit(VOID)
{
  UINT32 fw = SE_R(GENI_FW_REVISION_RO), cfg0, cfg1, cfg[4] = { 0 }, i, idx = 7;

  mTxDepth = (SE_R(SE_HW_PARAM_0) >> 16) & 0x3F;
  LOG ("se1 fw_rev=%08x proto=%u tx_depth=%u status=%08x\n",
          fw, (fw >> 8) & 0xFF, mTxDepth, SE_R(SE_GENI_STATUS));
  if (((fw >> 8) & 0xFF) != 3) {
    return EFI_UNSUPPORTED;
  }
  if (mTxDepth == 0) {
    mTxDepth = 16;
  }

  SE_W(SE_GSI_EVENT_EN, 0);
  SE_W(SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFF);
  SE_W(SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFF);
  SE_W(SE_DMA_TX_IRQ_CLR, 0xFFFFFFFF);
  SE_W(SE_DMA_RX_IRQ_CLR, 0xFFFFFFFF);
  SE_W(GENI_CGC_CTRL, SE_R(GENI_CGC_CTRL) | 0x7F);
  SE_W(GENI_OUTPUT_CTRL, 0x7F);
  SE_W(GENI_FORCE_DEFAULT_REG, 1);
  SE_W(SE_IRQ_EN, SE_R(SE_IRQ_EN) | (1u << 2) | (1u << 3));
  SE_W(SE_GENI_DMA_MODE_EN, SE_R(SE_GENI_DMA_MODE_EN) & ~1u);
  SE_W(SE_GENI_RX_WATERMARK_REG, mTxDepth - 1);
  SE_W(SE_GENI_RX_RFR_WATERMARK_REG, mTxDepth);
  SE_W(SE_GENI_M_IRQ_EN, SE_R(SE_GENI_M_IRQ_EN) | M_CMD_DONE_EN | M_I2C_ERR_MASK |
       (1u << 26) | (1u << 27));

  /* geni_se_config_packing(8 bpw, 4 words, msb_to_lsb) */
  for (i = 0; i < 4; i++) {
    cfg[i] = (idx << 5) | (1u << 4) | (7u << 1);
    idx = (i + 1) * 8 + 7;
  }
  cfg[3] |= 1u;
  cfg0 = cfg[0] | (cfg[1] << 10);
  cfg1 = cfg[2] | (cfg[3] << 10);
  SE_W(SE_GENI_TX_PACKING_CFG0, cfg0);
  SE_W(SE_GENI_TX_PACKING_CFG1, cfg1);
  SE_W(SE_GENI_RX_PACKING_CFG0, cfg0);
  SE_W(SE_GENI_RX_PACKING_CFG1, cfg1);
  SE_W(SE_GENI_BYTE_GRAN, 0);

  /* 100 kHz from 19.2 MHz: {div 7, t_high 10, t_low 11, t_cycle 26}; slow and safe */
  SE_W(GENI_SER_M_CLK_CFG, (7u << 4) | 1u);
  SE_W(SE_I2C_SCL_COUNTERS, (10u << 20) | (11u << 10) | 26u);
  return EFI_SUCCESS;
}

STATIC VOID DumpRegs(UINT8 Addr, CONST CHAR8 *Name, CONST UINT8 *Regs, UINTN Count)
{
  UINTN i;
  UINT8 v;
  EFI_STATUS s;

  for (i = 0; i < Count; i++) {
    v = 0;
    s = RegRead (Addr, Regs[i], &v);
    LOG ("%a[%02x]=%02x (%r)\n", Name, Regs[i], v, s);
  }
}

STATIC VOID ConPrint(CONST CHAR8 *Fmt, ...)
{
  CHAR8   a[160];
  CHAR16  w[160];
  VA_LIST ap;

  if (gST->ConOut == NULL) {
    return;
  }
  VA_START (ap, Fmt);
  AsciiVSPrint (a, sizeof (a), Fmt, ap);
  VA_END (ap);
  UnicodeSPrintAsciiFormat (w, sizeof (w), "%a", a);
  gST->ConOut->OutputString (gST->ConOut, w);
}

/* TRUE if a key is waiting; drains it into *Key */
STATIC BOOLEAN KeyPressed(EFI_INPUT_KEY *Key)
{
  return gST->ConIn != NULL && !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, Key));
}

/* Try to put CHG_CONFIG back to 1 while boosting, so the phone still charges later. */
STATIC VOID RestoreCharging(VOID)
{
  UINT8 r03 = 0, r0b = 0;

  RegRead (BQ_ADDR, 0x03, &r03);
  if (r03 & 0x10) {
    return;
  }
  RegWrite (BQ_ADDR, 0x03, r03 | 0x10);
  gBS->Stall (100 * 1000);
  RegRead (BQ_ADDR, 0x0B, &r0b);
  if ((r0b >> 5) != 7) {
    RegWrite (BQ_ADDR, 0x03, r03);           /* boost dropped: this chip needs CHG_CONFIG=0 */
    LOG ("CHG_CONFIG=1 kills boost on this chip, left at 0 (no charging until Android)\n");
  } else {
    LOG ("CHG_CONFIG restored to 1 with boost on\n");
  }
}

/*
 * The boost only starts when VBUS has no external supply, and the chip drops
 * OTG_CONFIG while a PC/charger cable is plugged in. Wait until VBUS is free
 * (or a key is pressed), then enable OTG and check VBUS_STAT == 7.
 */
STATIC BOOLEAN WaitAndEnableOtg(UINTN Seconds)
{
  UINTN  t;
  UINT8  r03 = 0, r0b = 0, r0c = 0, try = 0;
  EFI_INPUT_KEY key;

  for (t = 0; t < Seconds * 4; t++) {
    RegRead (BQ_ADDR, 0x0B, &r0b);
    RegRead (BQ_ADDR, 0x03, &r03);
    if ((r0b >> 5) == 7) {
      LOG ("OTG ON: reg03=%02x reg0b=%02x after %u ms (try %u)\n", r03, r0b, (UINT32)(t * 250), try);
      RestoreCharging ();
      return TRUE;
    }
    if ((r0b >> 5) == 0) {
      /* no input source: request boost; every 2nd attempt also clears CHG_CONFIG */
      try++;
      RegWrite (BQ_ADDR, 0x03, (try & 1) ? (r03 | 0x20) : ((r03 | 0x20) & ~0x10));
    }
    if ((t % 4) == 0) {
      RegRead (BQ_ADDR, 0x0C, &r0c);
      ConPrint ("\r  USB 5V: vbus_stat=%u reg03=%02x reg0c=%02x (%us, any key = skip)   ",
                r0b >> 5, r03, r0c, (UINT32)(Seconds - t / 4));
    }
    if (KeyPressed (&key)) {
      LOG ("OTG skipped by key: reg03=%02x reg0b=%02x\n", r03, r0b);
      return FALSE;
    }
    gBS->Stall (250 * 1000);
  }
  RegRead (BQ_ADDR, 0x0C, &r0c);
  RegRead (BQ_ADDR, 0x0C, &r0c);
  LOG ("OTG FAILED: reg03=%02x reg0b=%02x reg0c=%02x tries=%u\n", r03, r0b, r0c, try);
  return FALSE;
}

STATIC VOID DumpLog(VOID)
{
  UINTN i;

  for (i = 0; i < mLogLen; i++) {
    CHAR16 c[3] = { mLog[i], 0, 0 };
    if (c[0] == L'\n') {
      c[0] = L'\r';
      c[1] = L'\n';
    }
    if (gST->ConOut != NULL) {
      gST->ConOut->OutputString (gST->ConOut, c);
    }
  }
}

/* ---- Boot menu: Vol+/Vol- move, Power (any other key) selects ---------------- */

/*
 * Android/TWRP items are disabled: switching A/B from UEFI needs the full ABL
 * SetActiveSlot (type GUID swap of every _a/_b pair), see docs/AGENT_BRIEF_drivers.md 2c.
 * Android is started from the PC with `fastboot set_active a`.
 */
enum { MENU_WINDOWS, MENU_WINDOWS_NOGPU, MENU_FASTBOOT, MENU_POWEROFF, MENU_COUNT, MENU_ANDROID = 100, MENU_TWRP };
STATIC CONST CHAR8 *mMenu[MENU_COUNT] = { "Windows", "Windows (no GPU, safe display)", "Fastboot", "Power off" };

/*
 * Layout: slot b boot_b = this UEFI (active by default), slot a = Android + TWRP in
 * recovery_a. Android/TWRP switch the A/B attributes to slot a (Android switches back to
 * b after boot with `bootctl set-active-boot-slot 1`).
 */
STATIC AB_SLOT_INFO mAb;
STATIC EFI_STATUS   mAbStatus = EFI_NOT_READY;

/*
 * Reboot into a specific ABL mode. Mu's ResetSystem is plain PSCI and drops the
 * reset data, so do what Android's qpnp-power-on does for "reboot recovery":
 * store the reason in PMIC PON SOFT_RB_SPARE (gen2 PON: reason << 1, bits 7:1)
 * and do a warm reset (the spare register only survives a warm reset).
 * ABL reads it back on the next boot. 0x01 = recovery (Android lives in
 * recovery_a when this UEFI is flashed to boot_a), 0x02 = fastboot.
 */
#define ABL_REASON_RECOVERY  0x01
#define ABL_REASON_FASTBOOT  0x02

STATIC VOID RebootWithReason(UINT8 Reason)
{
  EFI_QCOM_PMIC_PON_PROTOCOL *pon = NULL;
  EFI_STATUS s;
  UINT8 before = 0, after = 0;

  s = gBS->LocateProtocol (&gQcomPmicPonProtocolGuid, NULL, (VOID **)&pon);
  if (!EFI_ERROR (s)) {
    pon->GetSpareReg (0, EFI_PM_PON_SOFT_SPARE, &before);
    s = pon->SetSpareReg (0, EFI_PM_PON_SOFT_SPARE, (UINT8)(Reason << 1), 0xFE);
    pon->GetSpareReg (0, EFI_PM_PON_SOFT_SPARE, &after);
  }
  ConPrint ("  PON soft spare: %02x -> %02x (%r)\r\n", before, after, s);
  gBS->Stall (2 * 1000 * 1000);
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
}

/*
 * "Windows (no GPU)": GPU0 in our DSDT has Name (GPUE, One) and _STA returns 0 when it is zero.
 * Patch the installed DSDT in place: GPUE OneOp -> ZeroOp, and _HID "TPZG0610" -> "TPZX0610"
 * because TopazDisplay yields the panel whenever HKLM\HARDWARE\ACPI\DSDT contains "TPZG0610".
 * Both edits lower the byte sum, the checksum byte takes the difference.
 */
STATIC EFI_STATUS DisableGpu0(VOID)
{
  STATIC CONST UINT8 Gpue[] = { 0x08, 'G', 'P', 'U', 'E', 0x01 };
  STATIC CONST UINT8 Hid[]  = { 'T', 'P', 'Z', 'G', '0', '6', '1', '0' };
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *rsdp = NULL;
  EFI_ACPI_DESCRIPTION_HEADER *xsdt, *dsdt = NULL;
  EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *fadt;
  UINT8 *p;
  UINTN i, n, gpue = 0, hid = 0;

  if (EFI_ERROR (EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, (VOID **)&rsdp)) || rsdp == NULL) {
    return EFI_NOT_FOUND;
  }
  xsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)rsdp->XsdtAddress;
  n = (xsdt->Length - sizeof (*xsdt)) / sizeof (UINT64);
  for (i = 0; i < n && dsdt == NULL; i++) {
    UINT64 a = ReadUnaligned64 ((UINT64 *)((UINT8 *)xsdt + sizeof (*xsdt) + i * sizeof (UINT64)));
    fadt = (EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE *)(UINTN)a;
    if (fadt->Header.Signature == EFI_ACPI_2_0_FIXED_ACPI_DESCRIPTION_TABLE_SIGNATURE) {
      dsdt = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)(fadt->XDsdt != 0 ? fadt->XDsdt : fadt->Dsdt);
    }
  }
  if (dsdt == NULL) {
    return EFI_NOT_FOUND;
  }
  p = (UINT8 *)dsdt;
  for (i = sizeof (*dsdt); i + sizeof (Hid) <= dsdt->Length; i++) {
    if (CompareMem (p + i, Gpue, sizeof (Gpue)) == 0) {
      p[i + 5] = 0x00;
      p[9] += 1;                                       /* OneOp -> ZeroOp: sum - 1 */
      gpue++;
    } else if (CompareMem (p + i, Hid, sizeof (Hid)) == 0) {
      p[i + 3] = 'X';
      p[9] += (UINT8)('G' - 'X');                      /* sum + ('X' - 'G') */
      hid++;
    }
  }
  LOG ("DSDT %p len %u: GPUE %u, HID %u, sum %02x\n", dsdt, dsdt->Length, (UINT32)gpue, (UINT32)hid,
       CalculateCheckSum8 (p, dsdt->Length));
  return gpue == 1 ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC VOID MenuDraw(UINTN Sel, UINTN Left)
{
  UINTN i;

  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 2);
  for (i = 0; i < MENU_COUNT; i++) {
    ConPrint ("  %a %a          \r\n", (i == Sel) ? ">>" : "  ", mMenu[i]);
  }
  ConPrint ("\r\n  Vol+/Vol- select, Power confirm. Default in %2us  \r\n", (UINT32)Left);
}

STATIC UINTN BootMenu(UINTN TimeoutSec)
{
  EFI_INPUT_KEY key;
  UINTN sel = MENU_WINDOWS, ticks = 0, quiet = 0, left = TimeoutSec;
  BOOLEAN touched = FALSE;

  if (gST->ConIn == NULL || gST->ConOut == NULL) {
    return MENU_WINDOWS;
  }
  /* Vol+ is held at power-on to get here: drop keys until the buttons are quiet for 0.5 s */
  while (quiet < 5 && ticks < 50) {
    quiet = KeyPressed (&key) ? 0 : quiet + 1;
    gBS->Stall (100 * 1000);
    ticks++;
  }
  /* no ClearScreen: the boot logos stay, the text cells only cover the top rows */
  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 0);
  ConPrint ("  ==== topaz: choose OS ====\r\n");
  MenuDraw (sel, left);
  LOG ("slots: %r a=%016lx b=%016lx\n", mAbStatus, mAb.AttrA, mAb.AttrB);
  for (ticks = 0; touched || ticks < TimeoutSec * 10; ticks++) {
    if (KeyPressed (&key)) {
      touched = TRUE;
      if (key.ScanCode == SCAN_UP) {
        sel = (sel + MENU_COUNT - 1) % MENU_COUNT;
      } else if (key.ScanCode == SCAN_DOWN) {
        sel = (sel + 1) % MENU_COUNT;
      } else {
        LOG ("menu: key scan=%x char=%x -> %a\n", key.ScanCode, key.UnicodeChar, mMenu[sel]);
        return sel;
      }
      MenuDraw (sel, left);
    }
    if (!touched && (ticks % 10) == 0) {
      left = TimeoutSec - ticks / 10;
      MenuDraw (sel, left);
    }
    gBS->Stall (100 * 1000);
  }
  return sel;
}

STATIC VOID EFIAPI OnReadyToBoot(IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_INPUT_KEY key;
  UINTN choice;

  gBS->CloseEvent (Event);
  /*
   * Notify functions run at TPL_CALLBACK, which blocks the keypad polling timer, so
   * ConIn never sees a key. Drop to TPL_APPLICATION for the interactive part and raise
   * back before returning to the event dispatcher.
   */
  gBS->RestoreTPL (TPL_APPLICATION);

  mAbStatus = AbSlotRead (&mAb);
#ifdef TOPAZ_AB_FIX
  /* one-off repair: attributes say a, GUIDs are still on b -> put the attributes back on b */
  if (!EFI_ERROR (mAbStatus) && AB_ACTIVE (mAb.AttrA)) {
    EFI_INPUT_KEY k;
    EFI_STATUS st = AbSlotSetActive (&mAb, TRUE);
    gST->ConOut->ClearScreen (gST->ConOut);
    ConPrint ("  AB FIX: attributes back to slot b: %r\r\n  a=%016lx b=%016lx\r\n  Any key\r\n", st, mAb.AttrA, mAb.AttrB);
    while (!KeyPressed (&k)) {
      gBS->Stall (100 * 1000);
    }
  }
#endif
  if (!EFI_ERROR (mAbStatus) && AB_ACTIVE (mAb.AttrB) && !AB_SUCCESS (mAb.AttrB)) {
    mAbStatus = AbSlotMarkBSuccessful (&mAb);        /* keep ABL from falling back to slot a */
  }

  for (;;) {
    choice = BootMenu (3);
    if (choice == MENU_ANDROID || choice == MENU_TWRP) {
      EFI_STATUS st = EFI_ERROR (mAbStatus) ? mAbStatus :
                      !AB_ACTIVE (mAb.AttrB) ? EFI_ALREADY_STARTED : AbSlotRequestA (&mAb);
      ConPrint ("\r\n  request slot a (b unbootable): %r  a=%016lx b=%016lx\r\n", st, mAb.AttrA, mAb.AttrB);
      if (EFI_ERROR (st)) {
        ConPrint ("  NOT switched. Any key: back to menu\r\n");
        while (!KeyPressed (&key)) {
          gBS->Stall (100 * 1000);
        }
        continue;
      }
      gBS->Stall (1500 * 1000);
      if (choice == MENU_TWRP) {
        RebootWithReason (ABL_REASON_RECOVERY);
      }
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    } else if (choice == MENU_FASTBOOT) {
      ConPrint ("\r\n  Rebooting to fastboot...\r\n");
      RebootWithReason (ABL_REASON_FASTBOOT);
    } else if (choice == MENU_WINDOWS_NOGPU) {
      EFI_STATUS st = DisableGpu0 ();
      ConPrint ("\r\n  GPU0 off in DSDT: %r\r\n", st);
      if (EFI_ERROR (st)) {
        ConPrint ("  NOT patched. Any key: back to menu\r\n");
        while (!KeyPressed (&key)) {
          gBS->Stall (100 * 1000);
        }
        continue;
      }
    } else if (choice == MENU_POWEROFF) {
      {
        /*
         * PSCI SYSTEM_OFF works, but the charger boost keeps 5 V on VBUS after the SoC is off;
         * the PMIC sees a cable and powers straight back on. Drop OTG_CONFIG first.
         */
        UINT8 r03 = 0, r0b = 0;
        RegRead (BQ_ADDR, 0x03, &r03);
        RegWrite (BQ_ADDR, 0x03, r03 & ~0x20);
        gBS->Stall (300 * 1000);
        RegRead (BQ_ADDR, 0x0B, &r0b);
        ConPrint ("\r\n  Powering off (OTG off, vbus_stat=%u; a cable on USB turns it back on)\r\n", r0b >> 5);
        gBS->Stall (1 * 1000 * 1000);
      }
      gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }
    break;                                              /* Windows */
  }

  ConPrint ("\r\n  Windows. Unplug PC/charger cable, plug the hub.\r\n");
  if (WaitAndEnableOtg (60)) {
    ConPrint ("\r\n  USB 5V ON\r\n");
    gBS->Stall (1 * 1000 * 1000);
  } else {
    ConPrint ("\r\n  USB 5V not enabled. Log:\r\n");
    DumpLog ();
    ConPrint ("\r\n  Any key: continue\r\n");
    while (!KeyPressed (&key)) {
      gBS->Stall (100 * 1000);
    }
  }
  gBS->RaiseTPL (TPL_CALLBACK);
}

EFI_STATUS
EFIAPI
TopazOtgEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC CONST UINT8 BqRegs[] = { 0x00, 0x03, 0x07, 0x0A, 0x0B, 0x0C, 0x14 };
  STATIC CONST UINT8 TcRegs[] = { 0x00, 0x01, 0x02, 0x03, 0x1A, 0x1C, 0x1D, 0x1E };
  EFI_STATUS s;
  UINT8 r03 = 0, r07 = 0, r0b = 0, r0c = 0;
  UINT32 ctl;
  EFI_EVENT ev;

  EfiCreateEventReadyToBootEx (TPL_CALLBACK, OnReadyToBoot, NULL, &ev);

  LOG ("start, gpio4 ctl=%08x gpio5 ctl=%08x usb_sw(gpio66) ctl=%08x io=%08x\n",
          MmioRead32 (TLMM_PIN (TLMM_WEST, QUP1_I2C_SDA)), MmioRead32 (TLMM_PIN (TLMM_WEST, QUP1_I2C_SCL)),
          MmioRead32 (TLMM_PIN (TLMM_WEST, USB_SWITCH_GPIO)), MmioRead32 (TLMM_PIN (TLMM_WEST, USB_SWITCH_GPIO) + 4));

  /* qupv3_se1_i2c_active: func qup1, 2 mA, no bias */
  ctl = (QUP1_FUNC << 2) | (0u << 6);
  MmioWrite32 (TLMM_PIN (TLMM_WEST, QUP1_I2C_SDA), ctl);
  MmioWrite32 (TLMM_PIN (TLMM_WEST, QUP1_I2C_SCL), ctl);

  ClocksOn ();
  s = GeniInit ();
  if (EFI_ERROR (s)) {
    LOG ("GENI init failed: %r\n", s);
    return EFI_SUCCESS;
  }

  DumpRegs (BQ_ADDR, "bq", BqRegs, ARRAY_SIZE (BqRegs));
  DumpRegs (TCPC_ADDR, "tcpc", TcRegs, ARRAY_SIZE (TcRegs));

  s = RegRead (BQ_ADDR, 0x07, &r07);
  if (!EFI_ERROR (s)) {
    s = RegWrite (BQ_ADDR, 0x07, r07 & ~0x30);          /* WATCHDOG[5:4] = 00: disabled */
  }
  if (!EFI_ERROR (s)) {
    s = RegRead (BQ_ADDR, 0x03, &r03);
  }
  if (!EFI_ERROR (s)) {
    s = RegWrite (BQ_ADDR, 0x03, r03 | 0x20);           /* OTG_CONFIG = 1 */
  }
  LOG ("bq otg enable: %r (reg07 %02x reg03 %02x)\n", s, r07, r03);

  MicroSecondDelay (100 * 1000);
  RegRead (BQ_ADDR, 0x0B, &r0b);
  RegRead (BQ_ADDR, 0x0C, &r0c);                       /* fault reg is read-to-clear: read twice */
  RegRead (BQ_ADDR, 0x0C, &r0c);
  RegRead (BQ_ADDR, 0x03, &r03);
  LOG ("after: reg03=%02x reg0b=%02x (vbus_stat=%u, 7=otg) reg0c=%02x\n",
          r03, r0b, r0b >> 5, r0c);
  return EFI_SUCCESS;
}
