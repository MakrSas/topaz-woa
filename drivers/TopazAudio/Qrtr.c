/*
 * QRTR over the ADSP's IPCRTR channel (net/qrtr) + the apps services the ADSP asks for:
 *  - name service: answer HELLO, log the ADSP's NEW_SERVERs, answer NEW_LOOKUP (our servers +
 *    the empty end-of-list record, like Linux qrtr ns);
 *  - pd-mapper (QMI servreg-locator 0x40 v0x101): domains from adspr/adsps/adspua.jsn,
 *    msm/adsp/{root_pd,sensor_pd,audio_pd}, instance 74.
 * Same packet formats as drivers/TopazModem/Glink.c + ModemSvc.c.
 */
#include "Audio.h"

#define Out ModemOut
#define CHAN               "IPCRTR"

#define QRTR_NODE_APPS     1
#define QRTR_PORT_CTRL     0xFFFFFFFEu
#define QRTR_TYPE_DATA     1
#define QRTR_TYPE_HELLO    2
#define QRTR_TYPE_BYE      3
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_DEL_CLIENT 6
#define QRTR_TYPE_RESUME_TX 7
#define QRTR_TYPE_EXIT     8
#define QRTR_TYPE_PING     9
#define QRTR_TYPE_NEW_LOOKUP 10
#define QRTR_TYPE_DEL_LOOKUP 11

#define SVC_PDM            0x40
#define PORT_PDM           0x4001
#define QMI_REQ            0
#define QMI_RESP           2

#pragma pack(1)
typedef struct { UINT32 Version, Type, SrcNode, SrcPort, ConfirmRx, Size, DstNode, DstPort; } QRTR_HDR_V1;
typedef struct { UINT8 Version, Type, Flags, OptLen; UINT32 Size; UINT16 SrcNode, SrcPort, DstNode, DstPort; } QRTR_HDR_V2;
typedef struct { UINT32 Cmd, A, B, C, D; } QRTR_CTRL;
#pragma pack()

STATIC UINT32  mVer = 1, mNode = 0xFFFFFFFFu;
STATIC BOOLEAN mHelloRx, mHelloSent, mAnnounced;
STATIC struct { UINT32 Svc, Inst, Node, Port; } mSrv[64];
STATIC UINTN   mSrvCount, mLookups, mPdReqs, mOther, mResumes;

#define T  (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)

STATIC VOID QrtrSend(UINT32 Type, UINT32 SrcPort, UINT32 DstNode, UINT32 DstPort, CONST VOID *Payload, UINT32 Len)
{
  UINT8 buf[sizeof (QRTR_HDR_V1) + 512];
  UINT32 n;

  if (Len > 512) {
    return;
  }
  ZeroMem (buf, sizeof (buf));
  if (mVer == 2) {
    QRTR_HDR_V2 *h = (QRTR_HDR_V2 *)buf;
    h->Version = 2;
    h->Type    = (UINT8)Type;
    h->Flags   = (Type == QRTR_TYPE_DATA) ? 1 : 0;
    h->Size    = Len;
    h->SrcNode = QRTR_NODE_APPS;
    h->SrcPort = (UINT16)SrcPort;
    h->DstNode = (UINT16)DstNode;
    h->DstPort = (UINT16)DstPort;
    n = sizeof (*h);
  } else {
    QRTR_HDR_V1 *h = (QRTR_HDR_V1 *)buf;
    h->Version   = 1;
    h->Type      = Type;
    h->SrcNode   = QRTR_NODE_APPS;
    h->SrcPort   = SrcPort;
    h->ConfirmRx = (Type == QRTR_TYPE_DATA) ? 1 : 0;
    h->Size      = Len;
    h->DstNode   = DstNode;
    h->DstPort   = DstPort;
    n = sizeof (*h);
  }
  CopyMem (buf + n, Payload, Len);
  GlinkSend (CHAN, buf, ALIGN_VALUE (n + Len, 4));
}

STATIC VOID SendCtrl(UINT32 Type, UINT32 DstNode, UINT32 DstPort, UINT32 A, UINT32 B, UINT32 C, UINT32 D)
{
  QRTR_CTRL c = { Type, A, B, C, D };
  QrtrSend (Type, QRTR_PORT_CTRL, DstNode, DstPort, &c, sizeof (c));
}

STATIC CONST CHAR8 *SvcName(UINT32 Svc)
{
  switch (Svc) {
  case 0x2B: return "ssctl";
  case 0x40: return "servreg-loc";
  case 0x42: return "servreg-notif";
  default:   return "";
  }
}

/* ---------------- pd-mapper ---------------- */

STATIC CONST struct { CONST CHAR8 *Svc, *Domain; } mPdMap[] = {
  { "tms/servreg", "msm/adsp/root_pd" },
  { "tms/servreg", "msm/adsp/sensor_pd" },
  { "tms/servreg", "msm/adsp/audio_pd" },
  { "avs/audio",   "msm/adsp/audio_pd" },
};

STATIC CONST UINT8 *QmFind(CONST UINT8 *D, UINT32 Len, UINT8 Type, UINT16 *TLen)
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

STATIC UINT32 QmTlv(UINT8 *M, UINT32 N, UINT8 Type, CONST VOID *V, UINT16 Len)
{
  M[N] = Type;
  *(UINT16 *)(M + N + 1) = Len;
  CopyMem (M + N + 3, V, Len);
  return N + 3 + Len;
}

STATIC VOID PdMapperRx(UINT32 Node, UINT32 Port, UINT16 Txn, UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  UINT8 m[400], list[300];
  CHAR8 name[80];
  UINT16 r[2] = { 0, 0 }, v, l = 0;
  CONST UINT8 *s;
  UINT32 n = 7, ln = 1, i, cnt = 0, inst = 74, zero = 0;

  m[0] = QMI_RESP;
  *(UINT16 *)(m + 1) = Txn;
  *(UINT16 *)(m + 3) = Msg;
  name[0] = 0;
  s = QmFind (D, Len, 1, &l);
  if (s != NULL) {
    l = (UINT16)MIN ((UINTN)l, sizeof (name) - 1);
    CopyMem (name, s, l);
    name[l] = 0;
  }
  n = QmTlv (m, n, 2, r, 4);
  if (Msg == 0x21) {                            /* GET_DOMAIN_LIST */
    for (i = 0; i < ARRAY_SIZE (mPdMap); i++) {
      UINT32 dl;
      if (AsciiStrCmp (mPdMap[i].Svc, name) != 0) {
        continue;
      }
      dl = (UINT32)AsciiStrLen (mPdMap[i].Domain);
      list[ln++] = (UINT8)dl;
      CopyMem (list + ln, mPdMap[i].Domain, dl);
      ln += dl;
      CopyMem (list + ln, &inst, 4);
      ln += 4;
      list[ln++] = 0;                           /* service_data_valid */
      CopyMem (list + ln, &zero, 4);
      ln += 4;
      cnt++;
    }
    list[0] = (UINT8)cnt;
    v = (UINT16)cnt;
    n = QmTlv (m, n, 0x10, &v, 2);             /* total_domains */
    v = 1;
    n = QmTlv (m, n, 0x11, &v, 2);             /* db_rev_count */
    if (cnt != 0) {
      n = QmTlv (m, n, 0x12, list, (UINT16)ln);
    }
  }
  *(UINT16 *)(m + 5) = (UINT16)(n - 7);
  QrtrSend (QRTR_TYPE_DATA, PORT_PDM, Node, Port, m, n);
  mPdReqs++;
  Out ("  t=%u.%03u pd-mapper: msg %x \"%a\" -> %u domain(s)\r\n", T, Msg, name, cnt);
}

/* ---------------- RX ---------------- */

STATIC VOID QrtrRx(CONST UINT8 *P, UINT32 Len)
{
  UINT32 type, srcNode, srcPort, dstPort, size, hlen, confirm;
  CONST QRTR_CTRL *c;

  if (Len >= sizeof (QRTR_HDR_V1) && *(CONST UINT32 *)P == 1) {
    CONST QRTR_HDR_V1 *h = (CONST QRTR_HDR_V1 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort; dstPort = h->DstPort;
    size = h->Size; hlen = sizeof (*h); confirm = h->ConfirmRx;
    mVer = 1;
  } else if (Len >= sizeof (QRTR_HDR_V2) && P[0] == 2) {
    CONST QRTR_HDR_V2 *h = (CONST QRTR_HDR_V2 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort; dstPort = h->DstPort;
    size = h->Size; hlen = sizeof (*h) + h->OptLen * 4; confirm = h->Flags & 1;
    srcPort = (srcPort == 0xFFFE) ? QRTR_PORT_CTRL : srcPort;
    dstPort = (dstPort == 0xFFFE) ? QRTR_PORT_CTRL : dstPort;
    mVer = 2;
  } else {
    Out ("  t=%u.%03u qrtr: bad packet, %u bytes\r\n", T, Len);
    return;
  }
  if (hlen + size > Len) {
    Out ("  t=%u.%03u qrtr: truncated (%u + %u > %u)\r\n", T, hlen, size, Len);
    return;
  }
  c = (CONST QRTR_CTRL *)(P + hlen);

  switch (type) {
  case QRTR_TYPE_HELLO:
    mNode = srcNode;
    mHelloRx = TRUE;
    mHelloSent = FALSE;                         /* (re)answer with our HELLO */
    Out ("  t=%u.%03u qrtr v%u HELLO from node %u\r\n", T, mVer, srcNode);
    break;
  case QRTR_TYPE_NEW_SERVER:
    if (c->A == 0 && c->B == 0) {
      break;                                    /* end of a lookup answer */
    }
    if (mSrvCount < ARRAY_SIZE (mSrv)) {
      mSrv[mSrvCount].Svc  = c->A;
      mSrv[mSrvCount].Inst = c->B;
      mSrv[mSrvCount].Node = c->C;
      mSrv[mSrvCount].Port = c->D;
      mSrvCount++;
    }
    Out ("  t=%u.%03u ADSP service %x v%u inst %x node %u port %x %a\r\n", T, c->A, c->B & 0xFF, c->B >> 8, c->C, c->D,
         SvcName (c->A));
    break;
  case QRTR_TYPE_NEW_LOOKUP:
    mLookups++;
    Out ("  t=%u.%03u ADSP lookup svc %x inst %x from %u:%x\r\n", T, c->A, c->B, srcNode, srcPort);
    if (c->A == 0 || c->A == SVC_PDM) {
      SendCtrl (QRTR_TYPE_NEW_SERVER, srcNode, srcPort, SVC_PDM, 0x101, QRTR_NODE_APPS, PORT_PDM);
    }
    SendCtrl (QRTR_TYPE_NEW_SERVER, srcNode, srcPort, 0, 0, 0, 0);
    break;
  case QRTR_TYPE_DATA:
    if (confirm) {
      QRTR_CTRL r = { QRTR_TYPE_RESUME_TX, QRTR_NODE_APPS, dstPort, 0, 0 };
      QrtrSend (QRTR_TYPE_RESUME_TX, dstPort, srcNode, srcPort, &r, sizeof (r));
    }
    if (dstPort == PORT_PDM && size >= 7 && P[hlen] == QMI_REQ) {
      PdMapperRx (srcNode, srcPort, *(CONST UINT16 *)(P + hlen + 1), *(CONST UINT16 *)(P + hlen + 3), P + hlen, size);
    } else if (mOther++ < 16) {
      Out ("  t=%u.%03u qrtr DATA %u:%x -> port %x, %u B, qmi %02x txn %04x msg %04x\r\n", T, srcNode, srcPort, dstPort,
           size, P[hlen], *(CONST UINT16 *)(P + hlen + 1), *(CONST UINT16 *)(P + hlen + 3));
    }
    break;
  case QRTR_TYPE_RESUME_TX:
    mResumes++;
    break;
  default:
    if (mOther++ < 16) {
      Out ("  t=%u.%03u qrtr type %u from %u:%x: %x %x %x %x\r\n", T, type, srcNode, srcPort, c->A, c->B, c->C, c->D);
    }
    break;
  }
}

VOID QrtrInit(VOID)
{
  GlinkRegister (CHAN, QrtrRx, FALSE);
}

VOID QrtrPoll(VOID)
{
  if (!GlinkChanUp (CHAN)) {
    return;
  }
  if (!mHelloSent) {
    SendCtrl (QRTR_TYPE_HELLO, mNode, QRTR_PORT_CTRL, 0, 0, 0, 0);
    mHelloSent = TRUE;
    Out ("  t=%u.%03u sent qrtr HELLO to node %x\r\n", T, mNode);
  }
  if (mHelloRx && !mAnnounced) {
    SendCtrl (QRTR_TYPE_NEW_SERVER, mNode, QRTR_PORT_CTRL, SVC_PDM, 0x101, QRTR_NODE_APPS, PORT_PDM);
    mAnnounced = TRUE;
    Out ("  t=%u.%03u announced pd-mapper 0x40\r\n", T);
  }
}

VOID QrtrSummary(VOID)
{
  UINTN i;

  Out ("  qrtr: adsp node %x, hello %u, %u services, %u lookups, pd-mapper %u reqs, %u RESUME_TX\r\n", mNode, mHelloRx,
       (UINT32)mSrvCount, (UINT32)mLookups, (UINT32)mPdReqs, (UINT32)mResumes);
  for (i = 0; i < mSrvCount; i++) {
    Out ("   svc %x v%u inst %x port %x %a\r\n", mSrv[i].Svc, mSrv[i].Inst & 0xFF, mSrv[i].Inst >> 8, mSrv[i].Port,
         SvcName (mSrv[i].Svc));
  }
}
