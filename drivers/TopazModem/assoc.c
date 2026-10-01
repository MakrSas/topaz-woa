/*
 * P5 station association on the modem thread (TopazWifi only), the ath10k/mac80211 managed-mode
 * flow done by hand (docs/P5_connect.md): VDEV_START on the AP's channel, PEER_CREATE, open-system
 * AUTH and ASSOC REQ as WMI management frames, replies from WMI MGMT_RX, then PEER_ASSOC (legacy
 * rates for now) + VDEV_UP. WPA2: our RSN IE (CCMP + the AP's group cipher) goes in the assoc
 * request; Windows does the 4-way handshake over the data path afterwards.
 */
#include "Modem.h"
#include "wlanif.h"

#define Out ModemOut
#define Step WlfwSetStep
#define T   (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000)

#define CMD_VDEV_START          0x5003
#define CMD_VDEV_UP             0x5005
#define CMD_VDEV_STOP           0x5006
#define CMD_VDEV_DOWN           0x5007
#define CMD_PEER_CREATE         0x6001
#define CMD_PEER_DELETE         0x6002
#define CMD_PEER_SET_PARAM      0x6004
#define CMD_PEER_ASSOC          0x6005
#define CMD_MGMT_TX_SEND        0x7008
#define CMD_STA_PS_MODE         0x9001

#define TAG_ARRAY_BYTE          0x11
#define TAG_ARRAY_STRUCT        0x12
#define TAG_CHANNEL             0x50
#define TAG_VDEV_START          0x58
#define TAG_VDEV_UP             0x5c
#define TAG_VDEV_STOP           0x5d
#define TAG_VDEV_DOWN           0x5e
#define TAG_PEER_CREATE         0x61
#define TAG_PEER_DELETE         0x62
#define TAG_PEER_SET_PARAM      0x64
#define TAG_PEER_ASSOC          0x65
#define TAG_VHT_RATE_SET        0x66
#define TAG_STA_PS_MODE         0x6c
#define TAG_MGMT_TX             0x1a6

#define PEER_FLAG_AUTH          0x1
#define PEER_FLAG_NEED_PTK_4WAY 0x4
#define PEER_PARAM_AUTHORIZE    3
#define MODE_11A                0
#define MODE_11G                1
#define VDEV_ID                 0

#define FC_ASSOC_REQ            0x00
#define FC_ASSOC_RESP           0x10
#define FC_DISASSOC             0xA0
#define FC_AUTH                 0xB0
#define FC_DEAUTH               0xC0

#define TRIES                   4
#define WAIT_MS                 400                  /* per auth / assoc attempt */

typedef enum { A_IDLE, A_START, A_AUTH, A_ASSOC, A_UP } ASTATE;

STATIC WLAN_CONNECT    mReq;                         /* the connect being worked on */
STATIC volatile BOOLEAN mReqNew, mDiscReq;
STATIC volatile USHORT mDiscReason;
STATIC ASTATE          mState;
STATIC UINT32          mTries, mAid, mDescId;
STATIC UINTN           mDeadline;
STATIC UINT8           mAssocReq[512], mAssocResp[512];
STATIC UINT32          mAssocReqLen, mAssocRespLen;
STATIC UINT8          *mTxBuf;                       /* management frames for MGMT_TX_SEND (DMA) */
STATIC UINT64          mTxPa;

STATIC UINT8 *P32(UINT8 *P, UINT32 V)
{
  *(UINT32 *)P = V;
  return P + 4;
}

STATIC UINT8 *PMac(UINT8 *P, CONST UINT8 *Mac)
{
  ZeroMem (P, 8);                                    /* wmi_mac_addr: 6 bytes + 2 pad */
  CopyMem (P, Mac, 6);
  return P + 8;
}

/* wmi_channel for the AP's channel, legacy mode (ath10k_wmi_put_wmi_channel) */
STATIC UINT8 *PutChannel(UINT8 *P, UINT32 Freq)
{
  UINT8 *c = WmiPutTlv (P, TAG_CHANNEL, 24);

  c = P32 (c, Freq);
  c = P32 (c, Freq);
  c = P32 (c, 0);
  c = P32 (c, Freq < 4000 ? MODE_11G : MODE_11A);
  c = P32 (c, (40u << 8) | (40u << 16));
  return P32 (c, 40u << 8);
}

/* ---------------- WMI commands (layouts: ath10k wmi-tlv.h / wmi-tlv.c) ---------------- */

/* wmi_tlv_vdev_start_cmd (72 B) + wmi_channel + empty NoA array; STA: no SSID (ath10k) */
STATIC BOOLEAN SendVdevStart(UINT32 Freq, UINT32 BcnIntval, UINT32 Dtim)
{
  UINT8 m[4 + 72 + 4 + 24 + 4], *p;

  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, TAG_VDEV_START, 72);
  p = P32 (p, VDEV_ID);
  p = P32 (p, 0);                                    /* requestor_id */
  p = P32 (p, BcnIntval);
  p = P32 (p, Dtim);
  p = P32 (p, 0);                                    /* flags */
  p += 36;                                           /* wmi_ssid {len, 32}: empty for STA */
  p += 16;                                           /* bcn_tx_rate/power, num_noa, disable_hw_ack */
  p = PutChannel (p, Freq);
  WmiPutTlv (p, TAG_ARRAY_STRUCT, 0);
  return WmiSend (CMD_VDEV_START, m, sizeof (m));
}

STATIC BOOLEAN SendPeerCreate(CONST UINT8 *Mac)
{
  UINT8 m[4 + 16], *p;

  p = WmiPutTlv (m, TAG_PEER_CREATE, 16);
  p = P32 (p, VDEV_ID);
  p = PMac (p, Mac);
  P32 (p, 0);                                        /* peer_type default */
  return WmiSend (CMD_PEER_CREATE, m, sizeof (m));
}

STATIC BOOLEAN SendPeerDelete(CONST UINT8 *Mac)
{
  UINT8 m[4 + 12], *p;

  p = WmiPutTlv (m, TAG_PEER_DELETE, 12);
  p = P32 (p, VDEV_ID);
  PMac (p, Mac);
  return WmiSend (CMD_PEER_DELETE, m, sizeof (m));
}

STATIC BOOLEAN SendVdevUp(UINT32 Aid, CONST UINT8 *Bssid)
{
  UINT8 m[4 + 16], *p;

  p = WmiPutTlv (m, TAG_VDEV_UP, 16);
  p = P32 (p, VDEV_ID);
  p = P32 (p, Aid);
  PMac (p, Bssid);
  return WmiSend (CMD_VDEV_UP, m, sizeof (m));
}

/* VDEV_DOWN / VDEV_STOP: just the vdev id */
STATIC BOOLEAN SendVdevSimple(UINT32 Cmd, UINT16 Tag)
{
  UINT8 m[4 + 4], *p;

  p = WmiPutTlv (m, Tag, 4);
  P32 (p, VDEV_ID);
  return WmiSend (Cmd, m, sizeof (m));
}

STATIC BOOLEAN SendPsOff(VOID)
{
  UINT8 m[4 + 8], *p;

  p = WmiPutTlv (m, TAG_STA_PS_MODE, 8);
  p = P32 (p, VDEV_ID);
  P32 (p, 0);                                        /* sta_ps_mode: disabled */
  return WmiSend (CMD_STA_PS_MODE, m, sizeof (m));
}

STATIC BOOLEAN SendPeerParam(CONST UINT8 *Mac, UINT32 Param, UINT32 Value)
{
  UINT8 m[4 + 20], *p;

  p = WmiPutTlv (m, TAG_PEER_SET_PARAM, 20);
  p = P32 (p, VDEV_ID);
  p = PMac (p, Mac);
  p = P32 (p, Param);
  P32 (p, Value);
  return WmiSend (CMD_PEER_SET_PARAM, m, sizeof (m));
}

/*
 * MGMT_TX_SEND (ath10k_wmi_tlv_op_gen_mgmt_tx_send): the whole frame sits in DMA memory (paddr,
 * reachable through the identity SMMU bank), the ARRAY_BYTE carries only its first
 * min(len, 64) bytes rounded up to 4; frame_len = full length, buf_len = that copy's length.
 */
#define MGMT_COPY_MAX 64

STATIC BOOLEAN SendMgmt(CONST UINT8 *Frame, UINT32 Len)
{
  UINT8 m[4 + 28 + 4 + MGMT_COPY_MAX], *p;
  UINT32 buf = ALIGN_VALUE (MIN (Len, MGMT_COPY_MAX), 4);

  if (mTxBuf == NULL) {
    mTxBuf = PhysAlloc (SIZE_4KB, &mTxPa);
  }
  if (mTxBuf == NULL || Len > SIZE_4KB - 8) {
    return FALSE;
  }
  ZeroMem (mTxBuf, ALIGN_VALUE (Len, 4) + 4);
  CopyMem (mTxBuf, Frame, Len);
  MemoryFence ();
  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, TAG_MGMT_TX, 28);
  p = P32 (p, VDEV_ID);
  p = P32 (p, ++mDescId);
  p = P32 (p, 0);                                    /* chanfreq: the vdev's channel */
  *(UINT64 *)p = mTxPa;
  p += 8;
  p = P32 (p, Len);                                  /* frame_len */
  p = P32 (p, buf);                                  /* buf_len */
  p = WmiPutTlv (p, TAG_ARRAY_BYTE, buf);
  CopyMem (p, mTxBuf, buf);
  return WmiSend (CMD_MGMT_TX_SEND, m, 4 + 28 + 4 + buf);
}

/*
 * PEER_ASSOC (wmi_tlv_peer_assoc_cmd, 76 B) + ARRAY_BYTE legacy rates + ARRAY_BYTE HT rates (none)
 * + VHT_RATE_SET (zeros). Legacy only for now. Rates: 500 kb/s units, BIT(7) on CCK rates
 * (ath10k_mac_bitrate_to_rate), i.e. the AP's rates IE with the "basic" bit dropped.
 */
STATIC BOOLEAN SendPeerAssoc(CONST UINT8 *Mac, UINT32 Aid, UINT32 Caps, CONST UINT8 *Rates, UINT32 NRates,
                             UINT32 Flags, UINT32 PhyMode)
{
  UINT8 m[4 + 76 + 4 + 16 + 4 + 4 + 16], *p, *r;
  UINT32 i, rl = ALIGN_VALUE (MIN (NRates, 16), 4);

  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, TAG_PEER_ASSOC, 76);
  p = PMac (p, Mac);
  p = P32 (p, VDEV_ID);
  p = P32 (p, 1);                                    /* new_assoc */
  p = P32 (p, Aid);
  p = P32 (p, Flags);
  p = P32 (p, Caps);
  p = P32 (p, 1);                                    /* listen_intval */
  p = P32 (p, 0);                                    /* ht_caps */
  p = P32 (p, 0);                                    /* max_mpdu */
  p = P32 (p, 0);                                    /* mpdu_density */
  p = P32 (p, 0);                                    /* rate_caps */
  p = P32 (p, 1);                                    /* nss */
  p = P32 (p, 0);                                    /* vht_caps */
  p = P32 (p, PhyMode);
  p += 8;                                            /* ht_info[2] */
  p = P32 (p, MIN (NRates, 16));                     /* num_legacy_rates */
  p = P32 (p, 0);                                    /* num_ht_rates */
  r = WmiPutTlv (p, TAG_ARRAY_BYTE, rl);
  for (i = 0; i < NRates && i < 16; i++) {
    UINT8 v = Rates[i] & 0x7F;
    r[i] = (v == 2 || v == 4 || v == 11 || v == 22) ? (UINT8)(v | 0x80) : v;
  }
  p = r + rl;
  p = WmiPutTlv (p, TAG_ARRAY_BYTE, 0);
  WmiPutTlv (p, TAG_VHT_RATE_SET, 16);
  return WmiSend (CMD_PEER_ASSOC, m, 4 + 76 + 4 + rl + 4 + 4 + 16);
}

/* ---------------- 802.11 frames ---------------- */

/* First IE with this id in a beacon/probe-response body (IEs start after 12 fixed bytes) */
STATIC CONST UINT8 *FindIe(CONST UINT8 *Body, UINT32 Len, UINT8 Id)
{
  UINT32 o;

  for (o = 12; o + 2 <= Len && o + 2 + Body[o + 1] <= Len; o += 2 + Body[o + 1]) {
    if (Body[o] == Id) {
      return Body + o;
    }
  }
  return NULL;
}

/* 24-byte management header to the AP: addr1 = addr3 = BSSID, addr2 = us */
STATIC UINT8 *MgmtHdr(UINT8 *F, UINT8 Fc)
{
  ZeroMem (F, 24);
  F[0] = Fc;
  CopyMem (F + 4, mReq.Bssid, 6);
  CopyMem (F + 10, ScanOurMac (), 6);
  CopyMem (F + 16, mReq.Bssid, 6);
  return F + 24;
}

STATIC BOOLEAN SendAuth(VOID)
{
  UINT8 f[24 + 6], *b = MgmtHdr (f, FC_AUTH);

  ZeroMem (b, 6);                                    /* algorithm 0 open, transaction 1, status 0 */
  b[2] = 1;
  return SendMgmt (f, sizeof (f));
}

/* AP's rates: Supported Rates (1) + Extended Supported Rates (50), max 16 */
STATIC UINT32 ApRates(UINT8 *Out)
{
  CONST UINT8 *ie;
  UINT32 n = 0, i, k;
  STATIC CONST UINT8 ids[2] = { 1, 50 };

  for (k = 0; k < 2; k++) {
    ie = FindIe (mReq.Body, mReq.BodyLen, ids[k]);
    for (i = 0; ie != NULL && i < ie[1] && n < 16; i++) {
      Out[n++] = ie[2 + i];
    }
  }
  return n;
}

/*
 * Assoc request body: capability, listen interval, SSID, rates (8 + extended), our RSN IE for WPA2
 * (one pairwise suite CCMP, the AP's group cipher, AKM from Windows), then IEs Windows gave us.
 */
STATIC UINT32 BuildAssocReq(UINT8 *F, UINT32 Max)
{
  UINT8 *b = MgmtHdr (F, FC_ASSOC_REQ), *p, rates[16];
  UINT32 n = ApRates (rates), cap = 0, k;
  CONST UINT8 *rsn = FindIe (mReq.Body, mReq.BodyLen, 48);

  if (mReq.BodyLen >= 12) {
    cap = (UINT32)mReq.Body[10] | ((UINT32)mReq.Body[11] << 8);
  }
  cap &= 0x0001 | 0x0010 | 0x0020 | 0x0400;          /* ESS, privacy, short preamble, short slot */
  p = b;
  *p++ = (UINT8)cap;
  *p++ = (UINT8)(cap >> 8);
  *p++ = 10;                                         /* listen interval */
  *p++ = 0;
  *p++ = 0;                                          /* SSID */
  *p++ = (UINT8)mReq.SsidLen;
  CopyMem (p, mReq.Ssid, mReq.SsidLen);
  p += mReq.SsidLen;
  *p++ = 1;                                          /* supported rates */
  *p++ = (UINT8)MIN (n, 8);
  for (k = 0; k < n && k < 8; k++) {
    *p++ = rates[k];
  }
  if (n > 8) {
    *p++ = 50;                                       /* extended supported rates */
    *p++ = (UINT8)(n - 8);
    for (k = 8; k < n; k++) {
      *p++ = rates[k];
    }
  }
  if (mReq.Rsn) {
    STATIC CONST UINT8 hdr[] = { 48, 20, 1, 0 };
    UINT8 group = 4;                                 /* CCMP unless the AP says otherwise */
    if (rsn != NULL && rsn[1] >= 8) {
      group = rsn[2 + 2 + 3];                        /* version(2), group OUI(3), type */
    }
    CopyMem (p, hdr, sizeof (hdr));
    p += sizeof (hdr);
    *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = group;                       /* group */
    *p++ = 1; *p++ = 0; *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = 4;       /* pairwise CCMP */
    *p++ = 1; *p++ = 0; *p++ = 0x00; *p++ = 0x0F; *p++ = 0xAC; *p++ = mReq.Akm;
    *p++ = 0; *p++ = 0;                                                         /* RSN caps */
  }
  if (mReq.ExtraIeLen != 0 && (UINT32)(p - F) + mReq.ExtraIeLen <= Max) {
    CopyMem (p, mReq.ExtraIe, mReq.ExtraIeLen);
    p += mReq.ExtraIeLen;
  }
  return (UINT32)(p - F);
}

/* ---------------- state machine (modem thread) ---------------- */

STATIC KSPIN_LOCK      mLock;                        /* static 0 = released spin lock */
STATIC WLAN_CONNECT    mPending;
STATIC volatile BOOLEAN mStartResp;
STATIC INT32           mStartStatus;

STATIC VOID Teardown(VOID)
{
  if (mState >= A_START) {
    if (mState == A_UP) {
      SendVdevSimple (CMD_VDEV_DOWN, TAG_VDEV_DOWN);
    }
    if (mState >= A_AUTH) {
      SendPeerDelete (mReq.Bssid);
    }
    SendVdevSimple (CMD_VDEV_STOP, TAG_VDEV_STOP);
  }
  mState = A_IDLE;
}

STATIC VOID Fail(LONG Status)
{
  Out ("  t=%u.%03u assoc: failed (%d) in state %u\r\n", T, Status, mState);
  Teardown ();
  WlanOnAssocResult (Status, 0, mAssocReq + 24, mAssocReqLen > 24 ? mAssocReqLen - 24 : 0,
                     mAssocResp + 24, mAssocRespLen > 24 ? mAssocRespLen - 24 : 0);
  Step ("assoc failed");
}

STATIC VOID Arm(VOID)
{
  mDeadline = ModemMs () + WAIT_MS;
}

STATIC VOID StartAuth(VOID)
{
  mState = A_AUTH;
  mTries = 1;
  SendPeerCreate (mReq.Bssid);
  SendAuth ();
  Arm ();
}

STATIC VOID StartAssoc(VOID)
{
  mState = A_ASSOC;
  mTries = 1;
  mAssocReqLen = BuildAssocReq (mAssocReq, sizeof (mAssocReq));
  SendMgmt (mAssocReq, mAssocReqLen);
  Arm ();
}

STATIC VOID Begin(VOID)
{
  UINT32 bcn = 100, dtim = 1;
  CONST UINT8 *tim;

  if (mReq.BodyLen >= 12) {
    bcn = (UINT32)mReq.Body[8] | ((UINT32)mReq.Body[9] << 8);
  }
  tim = FindIe (mReq.Body, mReq.BodyLen, 5);
  if (tim != NULL && tim[1] >= 2 && tim[3] != 0) {
    dtim = tim[3];
  }
  mAssocReqLen = mAssocRespLen = 0;
  mStartResp = FALSE;
  Out ("  t=%u.%03u assoc: %02x:%02x:%02x:**:**:** %u MHz bcn %u dtim %u %a\r\n", T, mReq.Bssid[0], mReq.Bssid[1],
       mReq.Bssid[2], mReq.Freq, bcn, dtim, mReq.Rsn ? "WPA2" : "open");
  SendPsOff ();
  if (!SendVdevStart (mReq.Freq, bcn, dtim)) {
    Fail (-2);
    return;
  }
  mState = A_START;
  mDeadline = ModemMs () + 1500;
  Step ("assoc: vdev start");
}

STATIC VOID OnUp(VOID)
{
  UINT8 rates[16];
  UINT32 n = ApRates (rates), cap = 0, flags = PEER_FLAG_AUTH;

  if (mAssocRespLen >= 26) {
    cap = (UINT32)mAssocResp[24] | ((UINT32)mAssocResp[25] << 8);
  }
  if (mReq.Rsn) {
    flags |= PEER_FLAG_NEED_PTK_4WAY;
  }
  SendPeerAssoc (mReq.Bssid, mAid, cap, rates, n, flags, mReq.Freq < 4000 ? MODE_11G : MODE_11A);
  SendVdevUp (mAid, mReq.Bssid);
  if (!mReq.Rsn) {
    SendPeerParam (mReq.Bssid, PEER_PARAM_AUTHORIZE, 1);
  }
  mState = A_UP;
  Out ("  t=%u.%03u *** associated, aid %u ***\r\n", T, mAid);
  Step ("associated");
  WlanOnAssocResult (0, mAid, mAssocReq + 24, mAssocReqLen - 24, mAssocResp + 24, mAssocRespLen - 24);
}

/* A management frame that is not a beacon / probe response (from scan.c) */
VOID AssocMgmtRx(CONST UINT8 *F, UINT32 Len)
{
  UINT8 fc = F[0];
  UINT16 v;

  if (Len < 26 || mState == A_IDLE || CompareMem (F + 10, mReq.Bssid, 6) != 0) {
    return;
  }
  v = (UINT16)(F[24] | (F[25] << 8));
  if (fc == FC_AUTH && mState == A_AUTH && Len >= 30 && F[26] == 2) {
    v = (UINT16)(F[28] | (F[29] << 8));              /* status */
    Out ("  t=%u.%03u assoc: auth reply status %u\r\n", T, v);
    if (v == 0) {
      StartAssoc ();
    } else {
      Fail (v);
    }
  } else if (fc == FC_ASSOC_RESP && mState == A_ASSOC && Len >= 30) {
    mAssocRespLen = MIN (Len, sizeof (mAssocResp));
    CopyMem (mAssocResp, F, mAssocRespLen);
    v = (UINT16)(F[26] | (F[27] << 8));              /* status */
    mAid = (UINT32)(F[28] | (F[29] << 8)) & 0x3FFF;
    Out ("  t=%u.%03u assoc: assoc reply status %u aid %u\r\n", T, v, mAid);
    if (v == 0) {
      OnUp ();
    } else {
      Fail (v);
    }
  } else if (fc == FC_DEAUTH || fc == FC_DISASSOC) {
    Out ("  t=%u.%03u assoc: AP ended the link, reason %u (state %u)\r\n", T, v, mState);
    if (mState == A_UP) {
      Teardown ();
      WlanOnDisconnected (v, TRUE);
    } else {
      Fail (v);
    }
  }
}

/* WMI events for us (wmi.c): first TLV's value */
VOID AssocEvent(UINT32 Id, CONST UINT8 *Tlvs, UINT32 Len)
{
  CONST UINT32 *v = (CONST UINT32 *)(Tlvs + 4);
  UINT32 l = Len >= 4 ? *(CONST UINT16 *)Tlvs : 0;

  if (Len < 4 + l) {
    return;
  }
  switch (Id) {
  case 0x5001:                                       /* VDEV_START_RESP {vdev, req, type, status} */
    if (l >= 16) {
      mStartStatus = (INT32)v[3];
      mStartResp = TRUE;
      Out ("  t=%u.%03u assoc: vdev start resp status %d\r\n", T, mStartStatus);
    }
    break;
  case 0x5002:                                       /* VDEV_STOPPED */
    Out ("  t=%u.%03u assoc: vdev stopped\r\n", T);
    break;
  case 0x6001:                                       /* PEER_STA_KICKOUT {mac} */
    Out ("  t=%u.%03u assoc: peer kickout (state %u)\r\n", T, mState);
    if (mState == A_UP) {
      Teardown ();
      WlanOnDisconnected (0, TRUE);
    }
    break;
  case 0x7006:                                       /* MGMT_TX_COMPLETION {desc_id, status} */
    if (l >= 8 && v[1] != 0) {
      Out ("  t=%u.%03u assoc: mgmt tx %u status %u\r\n", T, v[0], v[1]);
    }
    break;
  }
}

/* From the GLINK loop */
STATIC VOID KeysPoll(VOID);

VOID AssocPoll(VOID)
{
  KIRQL irql;
  BOOLEAN start = FALSE;

  KeysPoll ();

  if (mDiscReq) {
    mDiscReq = FALSE;
    mReqNew = FALSE;
    Out ("  t=%u.%03u assoc: disconnect requested (state %u)\r\n", T, mState);
    Teardown ();
    WlanOnDisconnected (mDiscReason, FALSE);
    Step ("disconnected");
  }
  if (mReqNew && !ScanBusy ()) {
    KeAcquireSpinLock (&mLock, &irql);
    CopyMem (&mReq, &mPending, sizeof (mReq));
    mReqNew = FALSE;
    KeReleaseSpinLock (&mLock, irql);
    start = TRUE;
  }
  if (start) {
    Teardown ();
    Begin ();
    return;
  }
  if (mState == A_START) {
    if (mStartResp) {
      if (mStartStatus == 0) {
        StartAuth ();
      } else {
        Fail (-3);
      }
    } else if (ModemMs () > mDeadline) {
      Fail (-4);
    }
  } else if ((mState == A_AUTH || mState == A_ASSOC) && ModemMs () > mDeadline) {
    if (mTries >= TRIES) {
      Fail (-1);
    } else {
      mTries++;
      if (mState == A_AUTH) {
        SendAuth ();
      } else {
        SendMgmt (mAssocReq, mAssocReqLen);
      }
      Arm ();
    }
  }
}

/* ---- front end -> core ---- */

VOID WlanConnectRequest(CONST WLAN_CONNECT *Req)
{
  KIRQL irql;

  KeAcquireSpinLock (&mLock, &irql);
  CopyMem (&mPending, Req, sizeof (mPending));
  mReqNew = TRUE;
  KeReleaseSpinLock (&mLock, irql);
}

VOID WlanDisconnectRequest(USHORT Reason)
{
  mDiscReason = Reason;
  mDiscReq = TRUE;
}

BOOLEAN WlanIsConnecting(VOID)
{
  return mReqNew || mState == A_START || mState == A_AUTH || mState == A_ASSOC;
}

BOOLEAN AssocIsUp(VOID)
{
  return mState == A_UP;
}

CONST UINT8 *AssocBssid(VOID)
{
  return mReq.Bssid;
}

/* ---------------- keys (WDI_SET_ADD_CIPHER_KEYS -> VDEV_INSTALL_KEY) ---------------- */

#define CMD_VDEV_INSTALL_KEY    0x5009
#define TAG_VDEV_INSTALL_KEY    0x60
#define KEY_FLAG_GROUP          0x1
#define MAX_KEYS                4

typedef struct { BOOLEAN Used, Group; UINT32 Idx, Cipher, Len; UINT8 Key[32]; } PEND_KEY;
STATIC PEND_KEY mKeys[MAX_KEYS];
STATIC volatile BOOLEAN mKeysNew;

/* wmi_vdev_install_key_cmd (92 B) + ARRAY_BYTE key; STA: pairwise and group both on the AP's address */
STATIC BOOLEAN SendKey(CONST PEND_KEY *K)
{
  UINT8 m[4 + 92 + 4 + 32], *p;
  UINT32 kl = ALIGN_VALUE (K->Len, 4);

  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, TAG_VDEV_INSTALL_KEY, 92);
  p = P32 (p, VDEV_ID);
  p = PMac (p, mReq.Bssid);
  p = P32 (p, K->Idx);
  p = P32 (p, K->Group ? KEY_FLAG_GROUP : 0);
  p = P32 (p, K->Cipher);
  p += 3 * 8 + 16 + 16;                              /* rsc/global rsc/tsc counters, wpi counters */
  p = P32 (p, K->Len);
  p = P32 (p, K->Cipher == 2 ? 8 : 0);               /* TKIP tx/rx mic */
  p = P32 (p, K->Cipher == 2 ? 8 : 0);
  p = WmiPutTlv (p, TAG_ARRAY_BYTE, kl);
  CopyMem (p, K->Key, K->Len);
  return WmiSend (CMD_VDEV_INSTALL_KEY, m, 4 + 92 + 4 + kl);
}

STATIC VOID KeysPoll(VOID)
{
  PEND_KEY k[MAX_KEYS];
  KIRQL irql;
  UINT32 i;

  if (!mKeysNew) {
    return;
  }
  KeAcquireSpinLock (&mLock, &irql);
  CopyMem (k, mKeys, sizeof (k));
  ZeroMem (mKeys, sizeof (mKeys));
  mKeysNew = FALSE;
  KeReleaseSpinLock (&mLock, irql);
  for (i = 0; i < MAX_KEYS; i++) {
    if (!k[i].Used || mState != A_UP) {
      continue;
    }
    Out ("  t=%u.%03u assoc: install %a key idx %u cipher %u len %u -> %a\r\n", T, k[i].Group ? "group" : "pairwise",
         k[i].Idx, k[i].Cipher, k[i].Len, SendKey (&k[i]) ? "sent" : "FAILED");
    if (!k[i].Group) {
      SendPeerParam (mReq.Bssid, PEER_PARAM_AUTHORIZE, 1);
      Step ("WPA2 keys installed");
    }
    ZeroMem (k[i].Key, sizeof (k[i].Key));
  }
}

/* Cipher: 2 TKIP, 4 CCMP (WMI-TLV values). Any thread. */
VOID WlanInstallKey(BOOLEAN Group, ULONG KeyIdx, ULONG Cipher, const UCHAR *Key, ULONG KeyLen)
{
  KIRQL irql;
  UINT32 i;

  if (KeyLen > 32) {
    return;
  }
  KeAcquireSpinLock (&mLock, &irql);
  for (i = 0; i < MAX_KEYS && mKeys[i].Used; i++) {
  }
  if (i < MAX_KEYS) {
    mKeys[i].Used = TRUE;
    mKeys[i].Group = Group;
    mKeys[i].Idx = KeyIdx;
    mKeys[i].Cipher = Cipher;
    mKeys[i].Len = KeyLen;
    CopyMem (mKeys[i].Key, Key, KeyLen);
    mKeysNew = TRUE;
  }
  KeReleaseSpinLock (&mLock, irql);
}
