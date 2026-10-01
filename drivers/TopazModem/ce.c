/*
 * P4 copy engines, polled (ath10k ce.c for WCN3990: target_64bit, shadow_reg_support, rri_on_ddr).
 * Each CE has a source ring (sender) and a dest ring (receiver). The host owns the source ring of
 * the host->target CEs (0, 3, 4, 7) and the dest ring of the target->host CEs (1, 2, 5, 7, 8..11);
 * the firmware already programmed the opposite rings (in MSA) after WLAN_MODE: never touch those.
 * All rings and buffers come from one PhysAlloc() block: the WLAN SMMU context bank is identity.
 */
#include "Modem.h"

#define Out ModemOut

#define WLAN_PA        0x0C800000u
#define CE_PA          (WLAN_PA + 0x240000u)
#define CE_COUNT       12
#define SHADOW_PA      (WLAN_PA + 0x32000u)      /* src write index shadow: +4*ce */

/* wcn3990_ce_regs */
#define SR_BASE_LO     0x00
#define SR_BASE_HI     0x04
#define SR_SIZE        0x08
#define DR_BASE_LO     0x0C
#define DR_BASE_HI     0x10
#define DR_SIZE        0x14
#define CTRL1          0x18
#define MISC_STATUS    0x38
#define SR_WR_IDX      0x3C
#define DR_WR_IDX      0x40
#define CUR_SRRI       0x44
#define CUR_DRRI       0x48
#define SRC_WM         0x4C                       /* low [31:16], high [15:0] */
#define DST_WM         0x50
#define WRAP_RRI_LO    0xC004                     /* from CE_PA (0x24C004 from WLAN_PA) */
#define WRAP_RRI_HI    0xC008

#define CTRL1_DMAX     0x0000FFFFu
#define CTRL1_SRC_SWAP (1u << 17)
#define CTRL1_DST_SWAP (1u << 18)
#define CTRL1_RRI_UPD  (1u << 19)
#define ADDR_HI_MASK   0x1Fu

#define ATTR_DIS_INTR  0x08
#define RX_BUF_SIZE    2048
#define RX_POST        32                         /* buffers kept posted per dest ring (<= ring) */

typedef struct { UINT64 Addr; UINT16 Nbytes, Flags; UINT32 Toeplitz; } CE_DESC;   /* ce_desc_64, 16 B */

typedef struct { UINT16 SrcN, SrcMax, DstN, Flags; } CE_ATTR;

/* host_ce_config_wlan (ath10k snoc.c) */
STATIC CONST CE_ATTR mAttr[CE_COUNT] = {
  { 16, 2048, 0, 0 },                   /* 0  host->target HTC control */
  { 0, 2048, 512, 0 },                  /* 1  target->host HTT + HTC control */
  { 0, 2048, 64, 0 },                   /* 2  target->host WMI (+ HTC ctrl) */
  { 32, 2048, 0, 0 },                   /* 3  host->target WMI */
  { 2048, 256, 0, ATTR_DIS_INTR },      /* 4  host->target HTT */
  { 0, 512, 512, 0 },                   /* 5  target->host HTT */
  { 0, 0, 0, 0 },                       /* 6  target autonomous */
  { 2, 2048, 2, 0 },                    /* 7  diag window (unused) */
  { 0, 2048, 128, 0 },                  /* 8  target->uMC */
  { 0, 2048, 512, 0 },                  /* 9  target->host HTT */
  { 0, 2048, 512, 0 },                  /* 10 target->host HTT */
  { 0, 2048, 512, 0 },                  /* 11 target->host pktlog */
};

typedef struct {
  volatile CE_DESC *Src, *Dst;
  UINT64  SrcPa, DstPa;
  UINT32  SrcN, DstN;                   /* ring sizes, power of 2 */
  UINT32  SrcWr;                        /* next source slot we fill */
  UINT32  DstWr, DstSw;                 /* next dest slot to post / to complete */
  UINT8  *RxBuf, *TxBuf;
  UINT64  RxPa, TxPa;
  UINT32  Rx, Tx, TxFull;
} CE_STATE;

STATIC CE_STATE         mCe[CE_COUNT];
STATIC UINT8           *mRegs, *mShadow;
STATIC volatile UINT32 *mRri;
STATIC UINT64           mRriPa;
STATIC BOOLEAN          mUp;
STATIC UINTN            mStartMs, mNextDump;

#define CE_REG(c, o)        ((UINTN)mRegs + (UINTN)(c) * SIZE_4KB + (o))
#define CeRd(c, o)          MmioRead32 (CE_REG (c, o))
#define CeWr(c, o, v)       MmioWrite32 (CE_REG (c, o), (v))
#define DmaBarrier()        __dsb (15)    /* DSB SY: descriptors/buffers visible before the doorbell */

STATIC BOOLEAN HostRxPipe(UINT32 C)
{
  return mAttr[C].DstN != 0 && C != 7;          /* CE7 = diag, nothing to receive there */
}

STATIC BOOLEAN HostTxPipe(UINT32 C)
{
  return C == 0 || C == 3 || C == 4;            /* HTC control, WMI, HTT */
}

/* TX buffers: at most TX_SLOTS_MAX in flight per pipe, each src_sz_max bytes (CE4: 256) */
#define TX_SLOTS_MAX   64
#define TxSlots(c)     MIN (mCe[c].SrcN, TX_SLOTS_MAX)
#define TxSz(c)        ((UINT32)mAttr[c].SrcMax)

STATIC UINT32 Pow2(UINT32 N)
{
  UINT32 p = 1;

  while (p < N) {
    p <<= 1;
  }
  return p;
}

STATIC VOID CeDump(CONST CHAR8 *Tag)
{
  UINT32 c;

  Out ("  ce %a: rri lo/hi %08x/%08x\r\n", Tag, MmioRead32 ((UINTN)mRegs + WRAP_RRI_LO),
       MmioRead32 ((UINTN)mRegs + WRAP_RRI_HI));
  for (c = 0; c < CE_COUNT; c++) {
    if (mAttr[c].SrcN == 0 && mAttr[c].DstN == 0) {
      continue;
    }
    Out ("   ce%u: sr %08x/%x dr %08x/%x ctrl1 %08x wm %08x/%08x misc %08x idx s %x/%x d %x/%x rri %08x rx %u tx %u\r\n",
         c, CeRd (c, SR_BASE_LO), CeRd (c, SR_SIZE), CeRd (c, DR_BASE_LO), CeRd (c, DR_SIZE), CeRd (c, CTRL1),
         CeRd (c, SRC_WM), CeRd (c, DST_WM), CeRd (c, MISC_STATUS), CeRd (c, SR_WR_IDX), CeRd (c, CUR_SRRI),
         CeRd (c, DR_WR_IDX), CeRd (c, CUR_DRRI), mRri[c], mCe[c].Rx, mCe[c].Tx);
  }
}

/* ath10k_ce_init_src_ring */
STATIC VOID InitSrc(UINT32 C)
{
  CE_STATE *s = &mCe[C];
  UINT32 v;

  s->SrcWr = CeRd (C, SR_WR_IDX) & (s->SrcN - 1);
  CeWr (C, SR_BASE_LO, (UINT32)s->SrcPa);
  CeWr (C, SR_BASE_HI, (UINT32)(s->SrcPa >> 32) & ADDR_HI_MASK);
  CeWr (C, SR_SIZE, s->SrcN);
  v = CeRd (C, CTRL1);
  v = (v & ~(CTRL1_DMAX | CTRL1_SRC_SWAP)) | mAttr[C].SrcMax;
  CeWr (C, CTRL1, v);
  CeWr (C, SRC_WM, s->SrcN);                                       /* low 0, high n */
}

/* ath10k_ce_init_dest_ring */
STATIC VOID InitDst(UINT32 C)
{
  CE_STATE *s = &mCe[C];
  UINT32 hi;

  s->DstSw = CeRd (C, CUR_DRRI) & (s->DstN - 1);
  s->DstWr = CeRd (C, DR_WR_IDX) & (s->DstN - 1);
  if (s->DstWr != s->DstSw) {
    Out ("  ce%u: dest ring not empty at init (wr %x rri %x)\r\n", C, s->DstWr, s->DstSw);
  }
  CeWr (C, DR_BASE_LO, (UINT32)s->DstPa);
  hi = CeRd (C, DR_BASE_HI);
  CeWr (C, DR_BASE_HI, (hi & ~ADDR_HI_MASK) | ((UINT32)(s->DstPa >> 32) & ADDR_HI_MASK));
  CeWr (C, DR_SIZE, s->DstN);
  CeWr (C, CTRL1, CeRd (C, CTRL1) & ~CTRL1_DST_SWAP);
  CeWr (C, DST_WM, s->DstN);                                       /* low 0, high n */
}

STATIC VOID PostOne(UINT32 C)
{
  CE_STATE *s = &mCe[C];
  UINT32 slot = s->DstWr;

  s->Dst[slot].Addr   = s->RxPa + (UINT64)(slot % RX_POST) * RX_BUF_SIZE;
  s->Dst[slot].Nbytes = 0;
  s->DstWr = (slot + 1) & (s->DstN - 1);
}

/* One block: RRI, rings (4 KiB aligned), RX buffers, TX slots. Never freed: the hardware keeps it. */
STATIC BOOLEAN CeAlloc(VOID)
{
  UINTN off = SIZE_4KB, size;                   /* RRI in the first page */
  UINT64 pa;
  UINT8 *va;
  UINT32 c;

  for (c = 0; c < CE_COUNT; c++) {
    mCe[c].SrcN = mAttr[c].SrcN ? Pow2 (mAttr[c].SrcN) : 0;
    mCe[c].DstN = mAttr[c].DstN ? Pow2 (mAttr[c].DstN) : 0;
    off += ALIGN_VALUE (mCe[c].SrcN * sizeof (CE_DESC), SIZE_4KB);
    off += ALIGN_VALUE (mCe[c].DstN * sizeof (CE_DESC), SIZE_4KB);
    off += HostRxPipe (c) ? RX_POST * RX_BUF_SIZE : 0;
    off += HostTxPipe (c) ? TxSlots (c) * TxSz (c) : 0;
  }
  size = off;
  va = PhysAlloc (size, &pa);
  if (va == NULL) {
    Out ("  ce: PhysAlloc %x failed\r\n", (UINT32)size);
    return FALSE;
  }
  Out ("  ce: host memory %lx + %x\r\n", pa, (UINT32)size);
  mRri = (volatile UINT32 *)va;
  mRriPa = pa;
  for (off = SIZE_4KB, c = 0; c < CE_COUNT; c++) {
    CE_STATE *s = &mCe[c];
    if (s->SrcN != 0) {
      s->Src = (volatile CE_DESC *)(va + off);
      s->SrcPa = pa + off;
      off += ALIGN_VALUE (s->SrcN * sizeof (CE_DESC), SIZE_4KB);
    }
    if (s->DstN != 0) {
      s->Dst = (volatile CE_DESC *)(va + off);
      s->DstPa = pa + off;
      off += ALIGN_VALUE (s->DstN * sizeof (CE_DESC), SIZE_4KB);
    }
    if (HostRxPipe (c)) {
      s->RxBuf = va + off;
      s->RxPa = pa + off;
      off += RX_POST * RX_BUF_SIZE;
    }
    if (HostTxPipe (c)) {
      s->TxBuf = va + off;
      s->TxPa = pa + off;
      off += TxSlots (c) * TxSz (c);
    }
  }
  return TRUE;
}

/* ath10k_snoc_hif_power_up (after wlan_enable) + hif_start: RRI, rings, RX buffers. */
BOOLEAN CeStart(VOID)
{
  UINT32 c, i;

  if (mUp) {
    return TRUE;
  }
  LogSetLazy (FALSE);                           /* register pokes: keep the log if the bus hangs */
  mRegs = MapPhys (CE_PA, (CE_COUNT + 1) * SIZE_4KB, FALSE);
  mShadow = MapPhys (SHADOW_PA, SIZE_4KB, FALSE);
  if (mRegs == NULL || mShadow == NULL || !CeAlloc ()) {
    Out ("  ce: map/alloc failed (regs %p shadow %p)\r\n", mRegs, mShadow);
    LogSetLazy (TRUE);
    return FALSE;
  }

  /* ath10k_ce_alloc_rri: the CE writes its read indices to mRri[ce] (src [15:0], dst [31:16]) */
  MmioWrite32 ((UINTN)mRegs + WRAP_RRI_LO, (UINT32)mRriPa);
  MmioWrite32 ((UINTN)mRegs + WRAP_RRI_HI, (UINT32)(mRriPa >> 32) & ADDR_HI_MASK);
  for (c = 0; c < CE_COUNT; c++) {
    CeWr (c, CTRL1, CeRd (c, CTRL1) | CTRL1_RRI_UPD);
  }

  for (c = 0; c < CE_COUNT; c++) {
    if (mCe[c].SrcN != 0) {
      InitSrc (c);
    }
    if (mCe[c].DstN != 0) {
      InitDst (c);
    }
  }
  Out ("  ce: %u rings programmed\r\n", CE_COUNT);

  /* ath10k_snoc_rx_post */
  for (c = 0; c < CE_COUNT; c++) {
    if (!HostRxPipe (c)) {
      continue;
    }
    for (i = 0; i < RX_POST; i++) {
      PostOne (c);
    }
    DmaBarrier ();
    CeWr (c, DR_WR_IDX, mCe[c].DstWr);
  }
  mUp = TRUE;
  mStartMs = ModemMs ();
  mNextDump = mStartMs + 2000;
  CeDump ("after init");
  LogSetLazy (TRUE);
  return TRUE;
}

/* Polled from the GLINK loop. Completed dest descriptors have nbytes != 0 (ath10k recv_next_64). */
BOOLEAN CePoll(VOID)
{
  BOOLEAN any = FALSE;
  UINT32 c, n, k;

  if (!mUp) {
    return FALSE;
  }
  for (c = 0; c < CE_COUNT; c++) {
    CE_STATE *s = &mCe[c];
    if (!HostRxPipe (c)) {
      continue;
    }
    for (k = 0; k < 16; k++) {
      UINT32 slot = s->DstSw;
      n = s->Dst[slot].Nbytes;
      if (n == 0) {
        break;
      }
      DmaBarrier ();                            /* payload after the descriptor */
      s->Dst[slot].Nbytes = 0;
      s->DstSw = (slot + 1) & (s->DstN - 1);
      s->Rx++;
      HtcRx (c, s->RxBuf + (slot % RX_POST) * RX_BUF_SIZE, n);
      PostOne (c);                              /* same buffer, RX_POST slots ahead */
      DmaBarrier ();
      CeWr (c, DR_WR_IDX, s->DstWr);
      any = TRUE;
    }
  }
  if (mNextDump != 0 && ModemMs () >= mNextDump) {
    CeDump (any || mCe[1].Rx + mCe[2].Rx != 0 ? "status" : "no rx yet");
    mNextDump = (ModemMs () - mStartMs < 20000) ? mNextDump + 8000 : 0;   /* t+2, +10, +18 s */
  }
  return any;
}

/* _ath10k_ce_send_nolock_64 + shadow write index. TransferId = HTC endpoint (descriptor metadata). */
BOOLEAN CeSend(UINT32 Ce, CONST VOID *Data, UINT32 Len, UINT32 TransferId)
{
  CE_STATE *s;
  UINT32 mask, sw, slot;
  volatile CE_DESC *d;

  if (!mUp || Ce >= CE_COUNT || !HostTxPipe (Ce) || Len > TxSz (Ce)) {
    return FALSE;
  }
  s = &mCe[Ce];
  mask = s->SrcN - 1;
  sw = mRri[Ce] & 0xFFFF;                       /* src read index from DDR */
  if (((sw - 1 - s->SrcWr) & mask) == 0 || ((s->SrcWr - sw) & mask) >= TxSlots (Ce) - 1) {
    s->TxFull++;                                /* ring full, or all TX buffers still in flight */
    return FALSE;
  }
  slot = s->SrcWr;
  CopyMem (s->TxBuf + (UINTN)(slot % TxSlots (Ce)) * TxSz (Ce), Data, Len);
  d = &s->Src[slot];
  d->Addr     = s->TxPa + (UINT64)(slot % TxSlots (Ce)) * TxSz (Ce);   /* addr[1]: hi bits only, no gather */
  d->Nbytes   = (UINT16)Len;
  d->Flags    = (UINT16)((TransferId << 4) & 0xFFF0);
  d->Toeplitz = 0;
  s->SrcWr = (slot + 1) & mask;
  s->Tx++;
  DmaBarrier ();
  MmioWrite32 ((UINTN)mShadow + 4 * Ce, s->SrcWr);
  return TRUE;
}

VOID CeSummary(VOID)
{
  if (mUp) {
    CeDump ("summary");
  }
}
