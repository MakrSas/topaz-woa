/*
 * P4 HTC (ath10k htc.c) over the polled copy engines: the firmware sends HTC READY on the
 * control endpoint 0 (RSVD_CTRL: out CE0, in CE2); we connect HTT_DATA and WMI_CONTROL like
 * ath10k_core_start, then SETUP_COMPLETE_EX, and log what the WMI/HTT endpoints deliver.
 */
#include "Modem.h"

#define Out ModemOut
#define Step WlfwSetStep
#define T   (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000)

#define HTC_MSG_READY          1
#define HTC_MSG_CONNECT        2
#define HTC_MSG_CONNECT_RESP   3
#define HTC_MSG_SETUP_COMPLETE_EX 5

#define HTC_FLAG_NEED_CREDIT_UPDATE 0x01
#define HTC_FLAG_TRAILER       0x02
#define HTC_CONN_NO_CREDIT_FLOW 0x0008
#define HTC_CONN_RECV_ALLOC_LSB 8

#define SVC_RSVD_CTRL          0x0001
#define SVC_WMI_CONTROL        0x0100
#define SVC_HTT_DATA           0x0300

#define EP_MAX                 8

#pragma pack(1)
typedef struct { UINT8 Eid, Flags; UINT16 Len; UINT8 Trailer, Seq; UINT16 Pad; } HTC_HDR;
#pragma pack()

typedef struct { UINT16 Svc; UINT8 Ul, Dl; } SVC_PIPE;

/* target_service_to_ce_map_wlan, the subset we connect */
STATIC CONST SVC_PIPE mSvcPipe[] = {
  { SVC_RSVD_CTRL, 0, 2 }, { SVC_WMI_CONTROL, 3, 2 }, { SVC_HTT_DATA, 4, 1 },
};

typedef struct { UINT16 Svc, MaxMsg; UINT8 Ul, Dl, Seq, Credits; BOOLEAN Up, CreditFlow; UINT32 Rx; } HTC_EP;

STATIC HTC_EP   mEp[EP_MAX];
STATIC UINT16   mCreditCount, mCreditSize;
STATIC UINT8    mMaxEp;
STATIC BOOLEAN  mReady, mSetupDone;
STATIC UINT8    mWmiEid = 0xFF, mHttEid = 0xFF;
STATIC UINT32   mDumps, mWmiEvents;

STATIC VOID Hex(CONST CHAR8 *Tag, CONST UINT8 *D, UINT32 Len)
{
  CHAR8 line[3 * 32 + 1];
  UINT32 i, n = MIN (Len, 64), o;

  for (o = 0; o < n; o += 32) {
    for (i = 0; i < 32 && o + i < n; i++) {
      AsciiSPrint (line + 3 * i, 4, "%02x ", D[o + i]);
    }
    line[3 * i] = 0;
    Out ("    %a +%02x: %a\r\n", Tag, o, line);
  }
}

STATIC CONST SVC_PIPE *SvcPipe(UINT16 Svc)
{
  UINT32 i;

  for (i = 0; i < ARRAY_SIZE (mSvcPipe); i++) {
    if (mSvcPipe[i].Svc == Svc) {
      return &mSvcPipe[i];
    }
  }
  return NULL;
}

/* ath10k_htc_send: 8-byte header + payload on the endpoint's UL pipe, transfer id = eid. */
STATIC BOOLEAN HtcSend(UINT8 Eid, CONST VOID *Payload, UINT32 Len)
{
  UINT8 buf[256];
  HTC_HDR *h = (HTC_HDR *)buf;
  HTC_EP *ep;

  if (Eid >= EP_MAX || Len + sizeof (*h) > sizeof (buf)) {
    return FALSE;
  }
  ep = &mEp[Eid];
  ZeroMem (h, sizeof (*h));
  h->Eid   = Eid;
  h->Len   = (UINT16)Len;
  h->Flags = ep->CreditFlow ? HTC_FLAG_NEED_CREDIT_UPDATE : 0;
  h->Seq   = ep->Seq++;
  CopyMem (buf + sizeof (*h), Payload, Len);
  if (!CeSend (ep->Ul, buf, (UINT32)(sizeof (*h) + Len), Eid)) {
    Out ("  t=%u.%03u htc: send on ep %u (ce%u) FAILED\r\n", T, Eid, ep->Ul);
    return FALSE;
  }
  return TRUE;
}

/* ath10k_htc_connect_service: CONNECT_SERVICE on ep 0; only WMI_CONTROL gets credit flow. */
STATIC VOID HtcConnect(UINT16 Svc)
{
  UINT8 m[8];
  UINT16 flags = 0;

  if (Svc == SVC_WMI_CONTROL) {
    flags |= (UINT16)(1u << HTC_CONN_RECV_ALLOC_LSB);   /* all tx credits: use_fw_tx_credits = 0 -> 1 */
  } else {
    flags |= HTC_CONN_NO_CREDIT_FLOW;
  }
  ZeroMem (m, sizeof (m));
  *(UINT16 *)&m[0] = HTC_MSG_CONNECT;
  *(UINT16 *)&m[2] = Svc;
  *(UINT16 *)&m[4] = flags;
  Out ("  t=%u.%03u htc: connect svc %x flags %x\r\n", T, Svc, flags);
  HtcSend (0, m, sizeof (m));
}

STATIC VOID HtcSetupComplete(VOID)
{
  UINT8 m[2 + 10];                              /* msg id + setup_complete_extended, all zero */

  ZeroMem (m, sizeof (m));
  *(UINT16 *)&m[0] = HTC_MSG_SETUP_COMPLETE_EX;
  if (HtcSend (0, m, sizeof (m))) {
    mSetupDone = TRUE;
    Out ("  t=%u.%03u htc: SETUP_COMPLETE_EX sent, waiting for WMI service ready\r\n", T);
    Step ("HTC up, waiting WMI");
  }
}

STATIC VOID OnReady(CONST UINT8 *M, UINT32 Len)
{
  mCreditCount = *(CONST UINT16 *)&M[2];
  mCreditSize  = *(CONST UINT16 *)&M[4];
  mMaxEp       = M[6];
  mReady = TRUE;
  mEp[0].Svc = SVC_RSVD_CTRL;                  /* pseudo-connected control endpoint */
  mEp[0].Ul = 0;
  mEp[0].Dl = 2;
  mEp[0].Up = TRUE;
  Out ("  t=%u.%03u *** HTC READY: credit_count %u credit_size %u max_endpoints %u%a ***\r\n", T,
       mCreditCount, mCreditSize, mMaxEp, Len >= 12 ? " (extended)" : "");
  if (Len >= 12) {
    Out ("  htc: version %u, max msgs per bundle %u, alt data %x\r\n", M[8], M[9],
         *(CONST UINT16 *)&M[10] & 0xFFF);
  }
  Step ("HTC READY");
  HtcConnect (SVC_HTT_DATA);                    /* ath10k_core_start: htt connect, then wmi */
}

STATIC VOID OnConnectResp(CONST UINT8 *M, UINT32 Len)
{
  UINT16 svc = *(CONST UINT16 *)&M[2], max = *(CONST UINT16 *)&M[6];
  UINT8 status = M[4], eid = M[5];
  CONST SVC_PIPE *p = SvcPipe (svc);

  Out ("  t=%u.%03u htc: connect resp svc %x status %u eid %u max_msg %u\r\n", T, svc, status, eid, max);
  if (status != 0 || eid >= EP_MAX || p == NULL) {
    return;
  }
  mEp[eid].Svc = svc;
  mEp[eid].MaxMsg = max;
  mEp[eid].Ul = p->Ul;
  mEp[eid].Dl = p->Dl;
  mEp[eid].Up = TRUE;
  mEp[eid].CreditFlow = svc == SVC_WMI_CONTROL;
  mEp[eid].Credits = mEp[eid].CreditFlow ? 1 : 0;
  if (svc == SVC_HTT_DATA) {
    mHttEid = eid;
    HtcConnect (SVC_WMI_CONTROL);
  } else if (svc == SVC_WMI_CONTROL) {
    mWmiEid = eid;
    HtcSetupComplete ();
  }
}

/* Trailer records {id, len, data}: id 1 = credit report {eid, credits}* (ath10k_htc_process_trailer). */
STATIC VOID OnTrailer(CONST UINT8 *P, UINT32 Len)
{
  UINT32 o = 0, i;

  while (o + 2 <= Len) {
    UINT8 id = P[o], rl = P[o + 1];
    if (o + 2 + rl > Len) {
      break;
    }
    if (id == 1) {
      for (i = 0; i + 1 < rl; i += 2) {
        UINT8 eid = P[o + 2 + i];
        if (eid < EP_MAX) {
          mEp[eid].Credits = (UINT8)(mEp[eid].Credits + P[o + 3 + i]);
        }
      }
    }
    o += 2 + rl;
  }
}

STATIC VOID OnWmi(CONST UINT8 *P, UINT32 Len)
{
  UINT32 id = Len >= 4 ? (*(CONST UINT32 *)P & 0xFFFFFF) : 0;

  mWmiEvents++;
  if (mWmiEvents <= 24) {
    Out ("  t=%u.%03u wmi: event %x (%a), %u bytes\r\n", T, id,
         id == 1 ? "SERVICE_READY" : id == 2 ? "READY" : "?", Len);
    Hex ("wmi", P, Len);
  }
  if (id == 1) {
    Step ("WMI SERVICE_READY");
  }
}

/* Called by CePoll for every completed receive buffer. */
VOID HtcRx(UINT32 Ce, CONST UINT8 *D, UINT32 Len)
{
  CONST HTC_HDR *h = (CONST HTC_HDR *)D;
  CONST UINT8 *p = D + sizeof (HTC_HDR);
  UINT32 plen;

  if (Len < sizeof (HTC_HDR)) {
    Out ("  t=%u.%03u htc: ce%u short rx %u\r\n", T, Ce, Len);
    return;
  }
  plen = MIN (h->Len, Len - (UINT32)sizeof (HTC_HDR));
  if (mDumps++ < 16) {
    Out ("  t=%u.%03u htc rx ce%u: eid %u flags %x len %u trailer %u (buf %u)\r\n", T, Ce, h->Eid,
         h->Flags, h->Len, h->Trailer, Len);
    Hex ("rx", D, Len);
  }
  if ((h->Flags & HTC_FLAG_TRAILER) != 0 && h->Trailer <= plen) {
    OnTrailer (p + plen - h->Trailer, h->Trailer);
    plen -= h->Trailer;
  }
  if (h->Eid < EP_MAX) {
    mEp[h->Eid].Rx++;
  }
  if (h->Eid == 0) {
    UINT16 msg = plen >= 2 ? *(CONST UINT16 *)p : 0;
    if (msg == HTC_MSG_READY && plen >= 8) {
      OnReady (p, plen);
    } else if (msg == HTC_MSG_CONNECT_RESP && plen >= 8) {
      OnConnectResp (p, plen);
    } else if (plen != 0) {
      Out ("  t=%u.%03u htc: control msg %u, %u bytes\r\n", T, msg, plen);
    }
  } else if (h->Eid == mWmiEid) {
    OnWmi (p, plen);
  } else if (h->Eid == mHttEid) {
    if (mEp[h->Eid].Rx <= 8) {
      Out ("  t=%u.%03u htt: msg type %u, %u bytes\r\n", T, plen ? p[0] : 0xFF, plen);
    }
  } else {
    Out ("  t=%u.%03u htc: rx on unknown eid %u (ce%u)\r\n", T, h->Eid, Ce);
  }
}

VOID HtcSummary(VOID)
{
  UINT32 i;

  Out ("  htc: ready %u (credits %u x %u, %u eps), setup %u, wmi eid %d htt eid %d, %u wmi events\r\n",
       mReady, mCreditCount, mCreditSize, mMaxEp, mSetupDone, mWmiEid == 0xFF ? -1 : mWmiEid,
       mHttEid == 0xFF ? -1 : mHttEid, mWmiEvents);
  for (i = 0; i < EP_MAX; i++) {
    if (mEp[i].Rx != 0 || mEp[i].Up) {
      Out ("   ep%u: svc %x ul ce%u dl ce%u rx %u credits %u\r\n", i, mEp[i].Svc, mEp[i].Ul, mEp[i].Dl,
           mEp[i].Rx, mEp[i].Credits);
    }
  }
}
