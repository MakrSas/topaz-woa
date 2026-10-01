/*
 * P4 HTT setup (ath10k htt.c ath10k_htt_setup, 64-bit variants for WCN3990), after WMI READY:
 * VERSION_REQ -> VERSION_CONF, FRAG_DESC_BANK_CFG (continuous_frag_desc), RX_RING_CFG (host rx
 * ring of buffer addresses + a host-written alloc index), AGGR_CFG v2. v0.12 showed the radio
 * receives (pdev rx_frame cycles grow) but no frame reaches the firmware: the RX DMA has nowhere
 * to put frames until the host ring exists. HTT messages go out on the HTT endpoint (CE4).
 */
#include "Modem.h"

#define Out ModemOut
#define Step WlfwSetStep
#define T   (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000)

#define H2T_VERSION_REQ         0
#define H2T_RX_RING_CFG         2
#define H2T_AGGR_CFG            5
#define H2T_FRAG_DESC_BANK_CFG  6

#define T2H_VERSION_CONF        0x00              /* htt_tlv_t2h_msg_type */
#define T2H_RX_IND              0x01
#define T2H_PEER_MAP            0x03
#define T2H_RX_IN_ORD_PADDR_IND 0x12

#define RING_SIZE               256               /* power of 2, >= HTT_RX_RING_SIZE_MIN (128) */
#define BUF_SIZE                2048              /* HTT_RX_BUF_SIZE */
#define MAX_PENDING_TX          (1024 + 32)       /* TARGET_TLV_NUM_MSDU_DESC */
#define EXT_DESC_SIZE           72                /* sizeof (htt_msdu_ext_desc_64) */

/* wcn3990 htt_rx_desc_v2 field offsets in 4-byte words (computed from ath10k rx_desc.h) */
#define OFF_ATTENTION           1
#define OFF_FRAG_INFO           2
#define OFF_MPDU_START          4
#define OFF_MSDU_START          7
#define OFF_MSDU_END            12
#define OFF_MPDU_END            26
#define OFF_PPDU_START          27
#define OFF_PPDU_END            37
#define OFF_HDR_STATUS          74                /* rx_hdr_status: first 64 B of the 802.11 header */
#define OFF_MSDU_PAYLOAD        90

STATIC UINT8   *mBlk;                             /* [ring u64 x N][alloc idx][frag bank][buffers] */
STATIC UINT64   mBlkPa, mRingPa, mIdxPa, mBankPa, mBufPa;
STATIC volatile UINT64 *mRing;
STATIC volatile UINT32 *mIdx;
STATIC UINT8   *mBuf;
STATIC UINT32   mAlloc;                           /* next ring slot the host fills */
STATIC UINT32   mState, mVerMajor, mVerMinor, mMsgs, mInOrd, mMsdus, mRecycled, mLogged;
STATIC UINTN    mReqMs;

STATIC UINT8 *Put16(UINT8 *P, UINT16 V)
{
  *(UINT16 *)P = V;
  return P + 2;
}

STATIC UINT8 *Put64(UINT8 *P, UINT64 V)
{
  *(UINT64 *)P = V;
  return P + 8;
}

STATIC BOOLEAN HttAlloc(VOID)
{
  UINTN ring = ALIGN_VALUE (RING_SIZE * 8 + 64, SIZE_4KB);
  UINTN bank = ALIGN_VALUE (MAX_PENDING_TX * EXT_DESC_SIZE, SIZE_4KB);
  UINTN size = ring + bank + (UINTN)RING_SIZE * BUF_SIZE;

  mBlk = PhysAlloc (size, &mBlkPa);
  if (mBlk == NULL) {
    Out ("  htt: PhysAlloc %x failed\r\n", (UINT32)size);
    return FALSE;
  }
  mRing   = (volatile UINT64 *)mBlk;
  mRingPa = mBlkPa;
  mIdx    = (volatile UINT32 *)(mBlk + RING_SIZE * 8);   /* fw_idx_shadow_reg: host-written alloc index */
  mIdxPa  = mBlkPa + RING_SIZE * 8;
  mBankPa = mBlkPa + ring;
  mBuf    = mBlk + ring + bank;
  mBufPa  = mBlkPa + ring + bank;
  Out ("  htt: host memory %lx + %x (ring %u x %u B, frag bank %u x %u B)\r\n", mBlkPa, (UINT32)size, RING_SIZE,
       BUF_SIZE, MAX_PENDING_TX, EXT_DESC_SIZE);
  return TRUE;
}

/* One rx buffer back on the ring (ath10k_htt_rx_ring_fill_n): attention flags cleared first. */
STATIC VOID PostBuf(UINT64 Pa)
{
  UINT8 *va = mBuf + (Pa - mBufPa);

  *(volatile UINT32 *)(va + 4 * OFF_ATTENTION) = 0;
  mRing[mAlloc] = Pa;
  mAlloc = (mAlloc + 1) & (RING_SIZE - 1);
}

STATIC VOID PublishIdx(VOID)
{
  __dsb (15);                                     /* ring entries before the index (ath10k mb()) */
  *mIdx = mAlloc;
}

/* ath10k_htt_send_frag_desc_bank_cfg_64 (no peer flow control: q_state empty) */
STATIC BOOLEAN SendFragBank(VOID)
{
  UINT8 m[64], *p = m;

  ZeroMem (m, sizeof (m));
  *p++ = H2T_FRAG_DESC_BANK_CFG;
  *p++ = 0;                                       /* info */
  *p++ = 1;                                       /* num_banks */
  *p++ = EXT_DESC_SIZE;                           /* desc_size */
  p = Put64 (p, mBankPa);                         /* bank_base_addrs[0], [1..3] = 0 */
  p += 3 * 8;
  p = Put16 (p, 0);                               /* bank_id[0].min */
  p = Put16 (p, MAX_PENDING_TX - 1);              /* bank_id[0].max */
  p += 3 * 4;
  p += 4 + 2 + 2;                                 /* q_state paddr, num_peers, num_tids: 0 */
  *p++ = 1;                                       /* HTT_TX_Q_STATE_ENTRY_SIZE */
  *p++ = 0;                                       /* HTT_TX_Q_STATE_ENTRY_MULTIPLIER */
  return HtcHttSend (m, sizeof (m));
}

/* ath10k_htt_send_rx_ring_cfg_64, wcn3990 descriptor offsets */
STATIC BOOLEAN SendRxRingCfg(VOID)
{
  UINT8 m[48], *p = m;

  ZeroMem (m, sizeof (m));
  *p++ = H2T_RX_RING_CFG;
  *p++ = 1;                                       /* num_rings */
  p = Put16 (p, 0);                               /* rsvd0 */
  p = Put64 (p, mIdxPa);                          /* fw_idx_shadow_reg_paddr */
  p = Put64 (p, mRingPa);                         /* rx_ring_base_paddr */
  p = Put16 (p, RING_SIZE);                       /* rx_ring_len */
  p = Put16 (p, BUF_SIZE);                        /* rx_ring_bufsize */
  p = Put16 (p, 0xFFFF);                          /* flags: hdr, payload, ppdu/mpdu/msdu start+end, attention,
                                                     frag info, ucast, mcast, ctrl, MGMT, null, phy data */
  p = Put16 (p, (UINT16)*mIdx);                   /* fw_idx_init_val */
  p = Put16 (p, OFF_HDR_STATUS);
  p = Put16 (p, OFF_MSDU_PAYLOAD);
  p = Put16 (p, OFF_PPDU_START);
  p = Put16 (p, OFF_PPDU_END);
  p = Put16 (p, OFF_MPDU_START);
  p = Put16 (p, OFF_MPDU_END);
  p = Put16 (p, OFF_MSDU_START);
  p = Put16 (p, OFF_MSDU_END);
  p = Put16 (p, OFF_ATTENTION);
  p = Put16 (p, OFF_FRAG_INFO);
  return HtcHttSend (m, sizeof (m));
}

/* ath10k_htt_h2t_aggr_cfg_msg_v2: ATH10K_HTT_MAX_NUM_AMPDU_DEFAULT 64 / AMSDU 3 */
STATIC BOOLEAN SendAggrCfg(VOID)
{
  UINT8 m[4] = { H2T_AGGR_CFG, 64, 3, 0 };

  return HtcHttSend (m, sizeof (m));
}

/* Everything after VERSION_CONF (or a timeout): config, fill the ring, then let the scan start. */
STATIC VOID HttConfigure(CONST CHAR8 *Why)
{
  BOOLEAN a, b, c;
  UINT32 i;

  mState = 2;
  a = SendFragBank ();
  b = SendRxRingCfg ();
  for (i = 0; i < RING_SIZE - 1; i++) {           /* rx_ring_fill_level = size - 1 (dual MAC) */
    PostBuf (mBufPa + (UINT64)i * BUF_SIZE);
  }
  PublishIdx ();
  c = SendAggrCfg ();
  Out ("  t=%u.%03u htt: (%a) frag bank %a, rx ring cfg %a (%u bufs posted, idx %u), aggr cfg %a\r\n", T, Why,
       a ? "ok" : "FAILED", b ? "ok" : "FAILED", RING_SIZE - 1, mAlloc, c ? "ok" : "FAILED");
  Step ("HTT configured");
  ScanStart ();
}

/* After WMI READY (ath10k_core_start: htt setup comes before the interface/scan). */
VOID HttStart(VOID)
{
  UINT8 m[4] = { H2T_VERSION_REQ, 0, 0, 0 };

  if (mState != 0) {
    return;
  }
  if (!HttAlloc ()) {
    ScanStart ();
    return;
  }
  mState = 1;
  mReqMs = ModemMs ();
  Out ("  t=%u.%03u htt: VERSION_REQ %a\r\n", T, HtcHttSend (m, sizeof (m)) ? "sent" : "SEND FAILED");
  Step ("HTT version req");
}

/* RX_IN_ORD_PADDR_IND: {type, info, peer_id, vdev_id, rsvd, msdu_count} + {u64 paddr, u16 len, u8 fw_desc, u8}[] */
STATIC VOID OnInOrd(CONST UINT8 *P, UINT32 Len)
{
  UINT32 n = *(CONST UINT16 *)&P[6], i;
  CONST UINT8 *d = P + 8;

  mInOrd++;
  for (i = 0; i < n && 8 + (i + 1) * 12 <= Len; i++, d += 12) {
    UINT64 pa = *(CONST UINT64 *)d;
    UINT32 mlen = *(CONST UINT16 *)(d + 8);
    if (pa < mBufPa || pa >= mBufPa + (UINT64)RING_SIZE * BUF_SIZE || ((pa - mBufPa) % BUF_SIZE) != 0) {
      Out ("  htt: in-order ind with foreign paddr %lx\r\n", pa);
      continue;
    }
    mMsdus++;
    if (mLogged < 12) {                           /* 802.11 header from rx_hdr_status */
      CONST UINT8 *h = mBuf + (pa - mBufPa) + 4 * OFF_HDR_STATUS;
      mLogged++;
      Out ("  t=%u.%03u htt rx: msdu len %u fw_desc %02x tid %u, fc %02x%02x a3 %02x:%02x:%02x:**:**:**\r\n", T, mlen,
           d[10], P[1] & 0x1F, h[0], h[1], h[16], h[17], h[18]);
    }
    PostBuf (pa);                                 /* recycle the same buffer */
    mRecycled++;
  }
  PublishIdx ();
}

/* Called by htc.c for every message on the HTT endpoint. */
VOID HttRx(CONST UINT8 *P, UINT32 Len)
{
  UINT8 type = Len != 0 ? P[0] : 0xFF;

  mMsgs++;
  if (type == T2H_VERSION_CONF && Len >= 3) {
    mVerMinor = P[1];
    mVerMajor = P[2];
    Out ("  t=%u.%03u *** HTT VERSION_CONF: %u.%u ***\r\n", T, mVerMajor, mVerMinor);
    if (mState == 1) {
      HttConfigure ("version conf");
    }
    return;
  }
  if (type == T2H_RX_IN_ORD_PADDR_IND && Len >= 8) {
    OnInOrd (P, Len);
    return;
  }
  if (mMsgs <= 24) {
    Out ("  t=%u.%03u htt: msg type %x, %u bytes\r\n", T, type, Len);
    ScanDump ("htt", P, Len, 8);
  }
}

VOID HttPoll(VOID)
{
  if (mState == 1 && ModemMs () - mReqMs > 2000) {
    Out ("  t=%u.%03u htt: no VERSION_CONF in 2 s, configuring anyway\r\n", T);
    HttConfigure ("timeout");
  }
}

VOID HttSummary(VOID)
{
  Out ("  htt: state %u, version %u.%u, %u msgs, %u in-order ind, %u msdus, %u recycled, alloc idx %u\r\n", mState,
       mVerMajor, mVerMinor, mMsgs, mInOrd, mMsdus, mRecycled, mAlloc);
}
