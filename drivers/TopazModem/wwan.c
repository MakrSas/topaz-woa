/*
 * SIM / mobile network, stage 1: QMI clients towards the modem over QRTR, log only.
 *
 *  - DMS  (svc 0x02): IMEI, firmware revision, operating mode; puts the modem ONLINE.
 *  - UIM  (svc 0x0B): card status (SIM present, application state, PIN state / retries).
 *  - NAS  (svc 0x03): registration, operator (MCC/MNC/name), radio technology, LTE signal.
 *
 * Message ids and TLV layouts follow libqmi's data/qmi-service-{dms,uim,nas}.json. Every QRTR
 * port is its own QMI client, so there is no CTL service and no client-id allocation.
 * Requests go out when the service appears (QRTR NEW_SERVER), status is re-read every 10 s and
 * logged when it changes. Nothing here touches the data path (WDS / IPA come in stage 2).
 */
#include "Modem.h"

#define Out ModemOut

#define QRTR_TYPE_DATA  1
#define QMI_REQ         0
#define QMI_RESP        2
#define QMI_IND         4

#define SVC_DMS  0x02
#define SVC_NAS  0x03
#define SVC_UIM  0x0B

/* our client ports (PORT_WWAN_BASE + index), see SvcRx */
enum { CL_DMS, CL_NAS, CL_UIM, CL_COUNT };

#define DMS_GET_REVISION     0x0023
#define DMS_GET_IDS          0x0025
#define DMS_GET_OPER_MODE    0x002D
#define DMS_SET_OPER_MODE    0x002E
#define UIM_GET_CARD_STATUS  0x002F
#define UIM_GET_SLOT_STATUS  0x0047
#define NAS_GET_SIGNAL_INFO  0x004F
#define NAS_GET_SERVING_SYS  0x0024

#define POLL_MS  10000

typedef struct {
  UINT32  Svc, Node, Port;
  BOOLEAN Up;
} CLIENT;

STATIC CLIENT  mCl[CL_COUNT] = { { SVC_DMS }, { SVC_NAS }, { SVC_UIM } };
STATIC UINT16  mTxn = 1;
STATIC UINTN   mLastPoll;
STATIC BOOLEAN mOnlineSent;

/* last logged state, to log only changes */
STATIC CHAR8   mImei[20], mRev[64];
STATIC INT32   mOperMode = -1;
STATIC UINT8   mCard[8];                      /* slot of the first present card: state, error, app type/state, pin1 state/retries, puk1 */
STATIC UINT8   mCardRaw[96];                  /* last card-status TLV, to log only changes */
STATIC UINT8   mSlotRaw[64];                  /* last slot-status TLV */
STATIC UINT8   mReg[6];                       /* reg, cs, ps, net, radio, n_radio */
STATIC UINT16  mMcc, mMnc;
STATIC CHAR8   mOper[32];
STATIC INT32   mRsrp = 1, mRsrq, mSnr, mRssi;

STATIC CONST CHAR8 *ModeName(UINT8 M)
{
  STATIC CONST CHAR8 *n[] = { "online", "low-power", "factory-test", "offline", "resetting", "shutting-down",
                              "persistent-low-power", "mode-only-low-power" };
  return M < ARRAY_SIZE (n) ? n[M] : "?";
}

STATIC CONST CHAR8 *RadioName(UINT8 R)
{
  switch (R) {
  case 0: return "none";
  case 1: return "cdma1x";
  case 2: return "evdo";
  case 4: return "gsm";
  case 5: return "umts";
  case 8: return "lte";
  case 9: return "td-scdma";
  case 12: return "5g-nr";
  default: return "?";
  }
}

/* ---------------- QMI framing ---------------- */

STATIC VOID Send(UINTN C, UINT16 Msg, CONST VOID *Tlvs, UINT16 TlvLen)
{
  UINT8 b[128];

  if (!mCl[C].Up || 7 + TlvLen > sizeof (b)) {
    return;
  }
  b[0] = QMI_REQ;
  *(UINT16 *)(b + 1) = mTxn++;
  *(UINT16 *)(b + 3) = Msg;
  *(UINT16 *)(b + 5) = TlvLen;
  if (TlvLen != 0) {
    CopyMem (b + 7, Tlvs, TlvLen);
  }
  QrtrSend (QRTR_TYPE_DATA, PORT_WWAN_BASE + (UINT32)C, mCl[C].Node, mCl[C].Port, b, 7 + TlvLen);
}

STATIC CONST UINT8 *Tlv(CONST UINT8 *D, UINT32 Len, UINT8 Type, UINT16 *TLen)
{
  UINT32 o = 7;

  while (o + 3 <= Len) {
    UINT16 l = *(CONST UINT16 *)(D + o + 1);
    if (o + 3 + l > Len) {
      break;
    }
    if (D[o] == Type) {
      *TLen = l;
      return D + o + 3;
    }
    o += 3 + l;
  }
  return NULL;
}

/* result TLV 0x02: {u16 result, u16 error}; returns the QMI error (0 = ok) */
STATIC UINT16 Result(CONST UINT8 *D, UINT32 Len)
{
  UINT16 l;
  CONST UINT8 *v = Tlv (D, Len, 0x02, &l);

  return (v != NULL && l >= 4) ? *(CONST UINT16 *)(v + 2) : 0xFFFF;
}

STATIC VOID Str(CONST UINT8 *V, UINT16 L, CHAR8 *Out, UINTN Max)
{
  UINTN n = MIN ((UINTN)L, Max - 1);

  CopyMem (Out, V, n);
  Out[n] = 0;
}

/* ---------------- requests ---------------- */

STATIC VOID Poll(VOID)
{
  Send (CL_DMS, DMS_GET_OPER_MODE, NULL, 0);
  Send (CL_UIM, UIM_GET_CARD_STATUS, NULL, 0);
  Send (CL_UIM, UIM_GET_SLOT_STATUS, NULL, 0);
  Send (CL_NAS, NAS_GET_SERVING_SYS, NULL, 0);
  Send (CL_NAS, NAS_GET_SIGNAL_INFO, NULL, 0);
}

VOID WwanArrive(UINT32 Svc, UINT32 Node, UINT32 Port)
{
  UINTN c;

  for (c = 0; c < CL_COUNT; c++) {
    if (mCl[c].Svc == Svc && !mCl[c].Up) {
      mCl[c].Node = Node;
      mCl[c].Port = Port;
      mCl[c].Up   = TRUE;
      Out ("  t=%u.%03u wwan: service %x up (node %u port %x)\r\n",
           (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000), Svc, Node, Port);
      if (c == CL_DMS) {
        Send (CL_DMS, DMS_GET_IDS, NULL, 0);
        Send (CL_DMS, DMS_GET_REVISION, NULL, 0);
      }
      Poll ();
    }
  }
}

VOID WwanPoll(VOID)
{
  UINTN now = ModemMs ();

  if (now - mLastPoll >= POLL_MS) {
    mLastPoll = now;
    Poll ();
  }
}

/* ---------------- responses ---------------- */

STATIC VOID DmsRx(UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  UINT16 l, err = Result (D, Len);
  CONST UINT8 *v;

  if (err != 0) {
    Out ("  wwan: dms msg %04x error %u\r\n", Msg, err);
    return;
  }
  switch (Msg) {
  case DMS_GET_IDS:
    if ((v = Tlv (D, Len, 0x11, &l)) != NULL) {
      Str (v, l, mImei, sizeof (mImei));
    }
    Out ("  wwan: IMEI %a\r\n", mImei);
    break;
  case DMS_GET_REVISION:
    if ((v = Tlv (D, Len, 0x01, &l)) != NULL) {
      Str (v, l, mRev, sizeof (mRev));
    }
    Out ("  wwan: modem firmware %a\r\n", mRev);
    break;
  case DMS_GET_OPER_MODE:
    if ((v = Tlv (D, Len, 0x01, &l)) != NULL && l >= 1 && (INT32)v[0] != mOperMode) {
      mOperMode = v[0];
      Out ("  wwan: operating mode %u (%a)\r\n", v[0], ModeName (v[0]));
    }
    if (mOperMode != 0 && mOperMode != 4 && !mOnlineSent) {
      UINT8 t[4] = { 0x01, 0x01, 0x00, 0x00 };     /* TLV 0x01 len 1: mode 0 = online */
      mOnlineSent = TRUE;
      Out ("  wwan: requesting ONLINE\r\n");
      Send (CL_DMS, DMS_SET_OPER_MODE, t, 4);
    }
    break;
  case DMS_SET_OPER_MODE:
    Out ("  wwan: set operating mode ok\r\n");
    break;
  }
}

STATIC CONST CHAR8 *CardErr(UINT8 E)
{
  STATIC CONST CHAR8 *n[] = { "unknown", "power down", "poll error", "no ATR", "voltage mismatch", "parity error",
                              "possibly removed", "technical problem", "null bytes", "sap connected" };
  return E < ARRAY_SIZE (n) ? n[E] : "?";
}

STATIC VOID Dump(CONST CHAR8 *Tag, CONST UINT8 *V, UINT16 L)
{
  UINT16 i;
  CHAR8 h[3 * 48 + 1];

  for (i = 0; i < L && i < 48; i++) {
    CONST CHAR8 *x = "0123456789abcdef";
    h[3 * i] = x[V[i] >> 4]; h[3 * i + 1] = x[V[i] & 15]; h[3 * i + 2] = ' ';
  }
  h[3 * i] = 0;
  Out ("  wwan: %a (%u B): %a\r\n", Tag, L, h);
}

/*
 * Card status TLV 0x10: u16 x4 (gw/1x primary/secondary index), u8 n_slots, per slot: card_state,
 * upin_state, upin_retries, upuk_retries, error, n_apps, per app: type, state, perso_state,
 * perso_feature, perso_retries, perso_unblock_retries, aid_len, aid[], univ_pin,
 * pin1_state, pin1_retries, puk1_retries, pin2_state, pin2_retries, puk2_retries.
 */
STATIC VOID CardStatus(CONST UINT8 *V, UINT16 L)
{
  CONST UINT8 *p = V + 9, *end = V + L;
  UINT8 slot, nSlots = (L >= 9) ? V[8] : 0, app;
  BOOLEAN first = TRUE;

  ZeroMem (mCard, sizeof (mCard));
  for (slot = 0; slot < nSlots && p + 6 <= end; slot++) {
    UINT8 state = p[0], err = p[4], nApps = p[5];
    Out ("  wwan: SIM slot %u: card %a%a%a, upin state %u, %u app(s)\r\n", slot + 1,
         state == 1 ? "present" : state == 0 ? "absent" : "ERROR", state == 2 ? " - " : "", state == 2 ? CardErr (err) : "",
         p[1], nApps);
    if (state == 1 && first) {
      mCard[0] = state;
    }
    p += 6;
    for (app = 0; app < nApps && p + 7 <= end; app++) {
      UINT8 aidLen = p[6];
      CONST UINT8 *q = p + 7 + aidLen;
      if (q + 7 > end) {
        return;
      }
      Out ("  wwan:   app %u: type %u (%a) state %u%a, pin1 state %u retries %u, puk1 retries %u\r\n", app,
           p[0], p[0] == 1 ? "SIM" : p[0] == 2 ? "USIM" : p[0] == 3 ? "RUIM" : p[0] == 4 ? "CSIM" : p[0] == 5 ? "ISIM" : "?",
           p[1], p[1] == 7 ? " READY" : p[1] == 2 ? " PIN REQUIRED" : p[1] == 3 ? " PUK REQUIRED" : p[1] == 1 ? " detected" : "",
           q[1], q[2], q[3]);
      if (state == 1 && first) {
        mCard[2] = p[0]; mCard[3] = p[1]; mCard[4] = q[1]; mCard[5] = q[2]; mCard[6] = q[3];
      }
      p = q + 7;
    }
    if (state == 1) {
      first = FALSE;
    }
  }
}

STATIC VOID UimRx(UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  UINT16 l, err = Result (D, Len);
  CONST UINT8 *v;

  if (err != 0) {
    Out ("  wwan: uim msg %04x error %u\r\n", Msg, err);
    return;
  }
  if (Msg == UIM_GET_CARD_STATUS) {
    v = Tlv (D, Len, 0x10, &l);
    if (v != NULL && (l > sizeof (mCardRaw) || CompareMem (v, mCardRaw, l) != 0)) {
      ZeroMem (mCardRaw, sizeof (mCardRaw));
      CopyMem (mCardRaw, v, MIN (l, sizeof (mCardRaw)));
      Dump ("card status", v, l);
      CardStatus (v, l);
    }
  } else if (Msg == UIM_GET_SLOT_STATUS) {
    /* TLV 0x10: u8 n, per physical slot: u32 card_state (0 unknown, 1 absent, 2 present),
       u32 slot_state (0 inactive, 1 active), u8 logical_slot, u8 iccid_len, iccid[] */
    v = Tlv (D, Len, 0x10, &l);
    if (v != NULL && (l > sizeof (mSlotRaw) || CompareMem (v, mSlotRaw, l) != 0)) {
      CONST UINT8 *p = v + 1, *end = v + l;
      UINT8 i;
      ZeroMem (mSlotRaw, sizeof (mSlotRaw));
      CopyMem (mSlotRaw, v, MIN (l, sizeof (mSlotRaw)));
      Dump ("slot status", v, l);
      for (i = 0; i < v[0] && p + 10 <= end; i++) {
        UINT32 card = *(CONST UINT32 *)p, act = *(CONST UINT32 *)(p + 4);
        Out ("  wwan: physical slot %u: card %a, slot %a, logical slot %u, iccid %u B\r\n", i + 1,
             card == 2 ? "present" : card == 1 ? "absent" : "unknown", act ? "active" : "inactive", p[8], p[9]);
        p += 10 + p[9];
      }
    }
  }
}

STATIC VOID NasRx(UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  UINT16 l, err = Result (D, Len);
  CONST UINT8 *v;

  if (err != 0) {
    if (Msg == NAS_GET_SIGNAL_INFO && err == 74) {   /* QMI_ERR_INFO_UNAVAILABLE: no signal yet */
      return;
    }
    Out ("  wwan: nas msg %04x error %u\r\n", Msg, err);
    return;
  }
  if (Msg == NAS_GET_SERVING_SYS) {
    UINT8 r[6];
    ZeroMem (r, sizeof (r));
    /* TLV 0x01: reg_state, cs_attach, ps_attach, selected_network, n_radio_if, radio_if[] */
    if ((v = Tlv (D, Len, 0x01, &l)) != NULL && l >= 5) {
      r[0] = v[0]; r[1] = v[1]; r[2] = v[2]; r[3] = v[3]; r[5] = v[4];
      r[4] = (v[4] >= 1 && l >= 6) ? v[5] : 0;
    }
    /* TLV 0x12: current PLMN: u16 mcc, u16 mnc, u8 desc_len, desc */
    if ((v = Tlv (D, Len, 0x12, &l)) != NULL && l >= 5) {
      mMcc = *(CONST UINT16 *)v;
      mMnc = *(CONST UINT16 *)(v + 2);
      Str (v + 5, MIN ((UINT16)v[4], (UINT16)(l - 5)), mOper, sizeof (mOper));
    }
    if (CompareMem (r, mReg, sizeof (r)) != 0) {
      STATIC CONST CHAR8 *reg[] = { "not registered", "REGISTERED", "searching", "denied", "unknown" };
      CopyMem (mReg, r, sizeof (r));
      Out ("  wwan: network %a, cs %u ps %u, radio %a, operator %03u-%02u \"%a\"\r\n",
           r[0] < ARRAY_SIZE (reg) ? reg[r[0]] : "?", r[1], r[2], RadioName (r[4]), mMcc, mMnc, mOper);
    }
  } else if (Msg == NAS_GET_SIGNAL_INFO) {
    /* TLV 0x14 LTE: i8 rssi, i8 rsrq, i16 rsrp, i16 snr (0.1 dB) */
    if ((v = Tlv (D, Len, 0x14, &l)) != NULL && l >= 6) {
      INT32 rssi = (INT8)v[0], rsrq = (INT8)v[1], rsrp = *(CONST INT16 *)(v + 2), snr = *(CONST INT16 *)(v + 4);
      if (rsrp / 3 != mRsrp / 3 || mRsrp == 1) {      /* log on >= 3 dB changes */
        mRssi = rssi; mRsrq = rsrq; mRsrp = rsrp; mSnr = snr;
        Out ("  wwan: LTE signal rssi %d dBm, rsrp %d dBm, rsrq %d dB, snr %d.%u dB\r\n",
             rssi, rsrp, rsrq, snr / 10, (UINT32)((snr < 0 ? -snr : snr) % 10));
      }
    }
  }
}

/* SvcRx: DstPort in [PORT_WWAN_BASE, PORT_WWAN_BASE + CL_COUNT) */
BOOLEAN WwanRx(UINT32 DstPort, CONST UINT8 *D, UINT32 Len)
{
  UINT16 msg;

  if (DstPort < PORT_WWAN_BASE || DstPort >= PORT_WWAN_BASE + CL_COUNT) {
    return FALSE;
  }
  if (Len < 7 || D[0] == QMI_REQ) {
    return TRUE;
  }
  msg = *(CONST UINT16 *)(D + 3);
  if (D[0] == QMI_IND) {
    return TRUE;                                   /* not registered for any yet */
  }
  switch (DstPort - PORT_WWAN_BASE) {
  case CL_DMS: DmsRx (msg, D, Len); break;
  case CL_UIM: UimRx (msg, D, Len); break;
  case CL_NAS: NasRx (msg, D, Len); break;
  }
  return TRUE;
}

VOID WwanSummary(VOID)
{
  Out ("  wwan: dms %u nas %u uim %u, IMEI %a, mode %d, SIM %u/%u, reg %u radio %a, %03u-%02u \"%a\", rsrp %d\r\n",
       mCl[CL_DMS].Up, mCl[CL_NAS].Up, mCl[CL_UIM].Up, mImei, mOperMode, mCard[0], mCard[3], mReg[0],
       RadioName (mReg[4]), mMcc, mMnc, mOper, mRsrp);
}
