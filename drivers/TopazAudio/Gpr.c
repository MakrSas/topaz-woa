/*
 * GPR (Generic Packet Router, AudioReach) on the GLINK channel "adsp_apps", after Linux
 * drivers/soc/qcom/apr.c (gpr) + sound/soc/qcom/qdsp6/q6apm.c. First milestone: ask APM
 * (module instance 1 on the ADSP) for the SPF state until it answers "ready".
 */
#include "Audio.h"

#define Out ModemOut
#define CHAN                 "adsp_apps"

#define GPR_DOMAIN_ADSP      2
#define GPR_DOMAIN_APPS      3
#define GPR_HDR_WORDS        6
#define GPR_OUR_PORT         3            /* downstream spf_core: gpr service reg = <3> */
#define APM_MODULE_IID       1

#define GPR_BASIC_RSP_RESULT     0x02001005u
#define APM_CMD_GET_SPF_STATE    0x01001021u
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007u

#pragma pack(1)
typedef struct {
  UINT32 Word0;                       /* version:4 hdr_size:4 (words) pkt_size:24 (bytes) */
  UINT8  DestDomain, SrcDomain;
  UINT16 Rsvd;
  UINT32 SrcPort, DestPort, Token, Opcode;
} GPR_HDR;
#pragma pack()

STATIC UINT32  mToken = 0x100, mSpfState = 0xFFFFFFFFu;
STATIC UINTN   mLastAsk, mAsks, mRx, mReadyMs;

#define T  (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)

STATIC VOID GprSend(UINT32 DestPort, UINT32 Opcode, CONST VOID *Payload, UINT32 Len)
{
  UINT8 buf[sizeof (GPR_HDR) + 256];
  GPR_HDR *h = (GPR_HDR *)buf;

  if (Len > 256) {
    return;
  }
  ZeroMem (buf, sizeof (buf));
  h->Word0      = 0 | (GPR_HDR_WORDS << 4) | ((sizeof (GPR_HDR) + Len) << 8);
  h->DestDomain = GPR_DOMAIN_ADSP;
  h->SrcDomain  = GPR_DOMAIN_APPS;
  h->SrcPort    = GPR_OUR_PORT;
  h->DestPort   = DestPort;
  h->Token      = mToken++;
  h->Opcode     = Opcode;
  CopyMem (buf + sizeof (GPR_HDR), Payload, Len);
  GlinkSend (CHAN, buf, sizeof (GPR_HDR) + Len);
}

STATIC VOID GprRx(CONST UINT8 *Data, UINT32 Len)
{
  CONST GPR_HDR *h = (CONST GPR_HDR *)Data;
  CONST UINT32 *p;
  UINT32 hlen;

  mRx++;
  if (Len < sizeof (GPR_HDR)) {
    Out ("  t=%u.%03u gpr: short packet %u B\r\n", T, Len);
    return;
  }
  hlen = ((h->Word0 >> 4) & 0xF) * 4;
  p = (CONST UINT32 *)(Data + MIN (hlen, Len));
  if (h->Opcode == APM_CMD_RSP_GET_SPF_STATE && Len >= hlen + 4) {
    if (p[0] != mSpfState) {
      Out ("  t=%u.%03u *** APM SPF state = %u%a (after %u asks) ***\r\n", T, p[0], p[0] == 1 ? " READY" : "", (UINT32)mAsks);
    }
    mSpfState = p[0];
    if (mSpfState == 1 && mReadyMs == 0) {
      mReadyMs = AudMs ();
    }
    return;
  }
  if (mRx <= 32) {
    Out ("  t=%u.%03u gpr rx %u B: dom %u->%u port %x->%x token %x opcode %08x payload %08x %08x\r\n", T, Len,
         h->SrcDomain, h->DestDomain, h->SrcPort, h->DestPort, h->Token, h->Opcode,
         Len >= hlen + 4 ? p[0] : 0, Len >= hlen + 8 ? p[1] : 0);
  }
}

VOID GprInit(VOID)
{
  GlinkRegister (CHAN, GprRx, FALSE);
}

VOID GprPoll(VOID)
{
  if (!GlinkChanUp (CHAN) || mSpfState == 1 || mAsks >= 120) {
    return;
  }
  if (mAsks == 0 || AudMs () - mLastAsk >= 1000) {
    mLastAsk = AudMs ();
    mAsks++;
    GprSend (APM_MODULE_IID, APM_CMD_GET_SPF_STATE, NULL, 0);
  }
}

VOID GprSummary(VOID)
{
  Out ("  gpr: channel %a, rx %u, GET_SPF_STATE asked %u, state %d%a\r\n", GlinkChanUp (CHAN) ? "UP" : "down",
       (UINT32)mRx, (UINT32)mAsks, (INT32)mSpfState, mSpfState == 1 ? " (APM READY)" : "");
}
