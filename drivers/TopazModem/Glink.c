/*
 * Wi-Fi P2 spike: talk to the running modem the way Linux does it, all by polling.
 *  - GLINK over SMEM (drivers/rpmsg/qcom_glink_smem.c + qcom_glink_native.c):
 *    item 478 = 4 ring indices, 479 = our TX FIFO (16 KiB), 480 = modem's TX FIFO (our RX),
 *    doorbell APCS IPC bit 12 (glink-edge mboxes = <&apcs_glb 12>).
 *  - channel "IPCRTR" carries QRTR (net/qrtr); we are node 1, the modem announces its
 *    services (NEW_SERVER) after HELLO. Goal: see WLFW (QMI service 0x45).
 * Windows copy of uefi/TopazOtgDxe/Glink.c: the loop runs until gModemStop or modem FATAL.
 */
#include "Modem.h"

#define Out ModemOut

#define SMEM_GLINK_DESC    478
#define SMEM_GLINK_FIFO_TX 479
#define SMEM_GLINK_FIFO_RX 480
#define GLINK_TX_SIZE      SIZE_16KB
#define GLINK_IRQ_BIT      12
#define GLINK_FEATURES     1                  /* INTENT_REUSE */

#define CMD_VERSION        0
#define CMD_VERSION_ACK    1
#define CMD_OPEN           2
#define CMD_CLOSE          3
#define CMD_OPEN_ACK       4
#define CMD_INTENT         5
#define CMD_RX_DONE        6
#define CMD_RX_INTENT_REQ  7
#define CMD_RX_INTENT_REQ_ACK 8
#define CMD_TX_DATA        9
#define CMD_CLOSE_ACK      11
#define CMD_TX_DATA_CONT   12
#define CMD_READ_NOTIF     13
#define CMD_RX_DONE_W_REUSE 14
#define CMD_SIGNALS        15

#define CHAN_NAME          "IPCRTR"
#define CHAN_LCID          1
#define OUR_INTENTS        8
#define OUR_INTENT_SIZE    0x4400

#define QRTR_NODE_APPS     1
#define QRTR_PORT_CTRL     0xFFFFFFFEu
#define QRTR_NODE_BCAST    0xFFFFFFFFu
#define QRTR_TYPE_DATA     1
#define QRTR_TYPE_HELLO    2
#define QRTR_TYPE_BYE      3
#define QRTR_TYPE_NEW_SERVER 4
#define QRTR_TYPE_DEL_SERVER 5
#define QRTR_TYPE_DEL_CLIENT 6
#define QRTR_TYPE_RESUME_TX 7
#define QRTR_TYPE_NEW_LOOKUP 10
#define QMI_SVC_WLFW       0x45

#pragma pack(1)
typedef struct { UINT16 Cmd, P1; UINT32 P2; } GLINK_MSG;
typedef struct { GLINK_MSG M; UINT32 Chunk, Left; } GLINK_DATA_HDR;
typedef struct {
  UINT32 Version, Type, SrcNode, SrcPort, ConfirmRx, Size, DstNode, DstPort;
} QRTR_HDR_V1;
typedef struct {
  UINT8  Version, Type, Flags, OptLen;
  UINT32 Size;
  UINT16 SrcNode, SrcPort, DstNode, DstPort;
} QRTR_HDR_V2;
typedef struct { UINT32 Cmd, A, B, C, D; } QRTR_CTRL;
#pragma pack()

typedef struct { UINT32 Id, Size; BOOLEAN Used; } REMOTE_INTENT;
typedef struct { UINT32 Svc, Inst, Node, Port; } QRTR_SERVER;

STATIC volatile UINT32 *mDesc;                 /* [0] tx tail [1] tx head [2] rx tail [3] rx head */
STATIC UINT8   *mTx, *mRx;
STATIC UINT32   mRxLen;
STATIC UINT32   mFeatures = GLINK_FEATURES;
STATIC UINT32   mRcid;
STATIC BOOLEAN  mRemoteOpen, mOpenSent, mOpenAcked, mIntentsSent, mIntentReqSent;
STATIC UINT32   mNextIntent = 1;
STATIC REMOTE_INTENT mRi[256];
STATIC UINTN    mRiCount;
STATIC UINT8    mAsm[OUR_INTENT_SIZE];
STATIC UINT32   mAsmLen;
STATIC UINT32   mQrtrVer = 1, mModemNode = QRTR_NODE_BCAST;
STATIC BOOLEAN  mHelloSent, mHelloRx, mAnnounced;
STATIC QRTR_SERVER mSrv[64];
STATIC UINTN    mSrvCount;
STATIC UINTN    mNowMs, mT0;
STATIC UINTN    mIntentReqs, mIntentDenied;
volatile UINT32 *gModemState;
UINTN           gTrace;                        /* >0: trace GLINK traffic (set by ModemSvc.c) */                  /* modem SMP2P slave-kernel entry, set by ModemPas.c */

/* wall-clock ms since the spike started (console output is slow, so loop counts lie) */
UINTN ModemMs(VOID)
{
  return (UINTN)(GetTimeInNanoSecond (GetPerformanceCounter ()) / 1000000) - mT0;
}
STATIC UINTN    mRxMsgs, mTxMsgs, mOther, mResumes, mLastStatus;

#define T  (UINT32)(mNowMs / 1000), (UINT32)(mNowMs % 1000)

/* ---------------- GLINK SMEM pipes ---------------- */

STATIC UINT32 RxAvail(VOID)
{
  UINT32 h = mDesc[3], t = mDesc[2];
  return (h >= t) ? h - t : mRxLen - t + h;
}

STATIC VOID RxPeek(UINT32 Off, VOID *Dst, UINT32 N)
{
  UINT32 t = mDesc[2] + Off, n1;

  if (t >= mRxLen) {
    t -= mRxLen;
  }
  n1 = MIN (N, mRxLen - t);
  CopyMem (Dst, mRx + t, n1);
  if (N > n1) {
    CopyMem ((UINT8 *)Dst + n1, mRx, N - n1);
  }
}

STATIC VOID RxAdvance(UINT32 N)
{
  UINT32 t = mDesc[2] + N;

  if (t >= mRxLen) {
    t -= mRxLen;
  }
  MemoryFence ();
  mDesc[2] = t;
}

STATIC UINT32 TxPut(UINT32 Head, CONST VOID *Src, UINT32 N)
{
  UINT32 n1 = MIN (N, GLINK_TX_SIZE - Head);

  CopyMem (mTx + Head, Src, n1);
  if (N > n1) {
    CopyMem (mTx, (CONST UINT8 *)Src + n1, N - n1);
  }
  Head += N;
  if (Head >= GLINK_TX_SIZE) {
    Head -= GLINK_TX_SIZE;
  }
  return Head;
}

STATIC BOOLEAN TxWrite(CONST VOID *Hdr, UINT32 HLen, CONST VOID *Data, UINT32 DLen)
{
  UINT32 head, tail, avail, n;

  for (n = 0; n < 1000; n++) {
    head = mDesc[1];
    tail = mDesc[0];
    avail = (tail <= head) ? GLINK_TX_SIZE - head + tail : tail - head;
    avail = (avail < 8) ? 0 : avail - 8;
    if (avail >= ALIGN_VALUE (HLen + DLen, 8)) {
      break;
    }
    gBS->Stall (100);
  }
  if (n == 1000) {
    Out ("  glink TX FIFO full (head %x tail %x)\r\n", head, tail);
    return FALSE;
  }
  head = TxPut (head, Hdr, HLen);
  if (DLen != 0) {
    head = TxPut (head, Data, DLen);
  }
  head = ALIGN_VALUE (head, 8);
  if (head >= GLINK_TX_SIZE) {
    head -= GLINK_TX_SIZE;
  }
  MemoryFence ();
  mDesc[1] = head;
  MemoryFence ();
  ApcsKick (GLINK_IRQ_BIT);
  mTxMsgs++;
  return TRUE;
}

STATIC VOID SendCmd(UINT16 Cmd, UINT16 P1, UINT32 P2)
{
  GLINK_MSG m = { Cmd, P1, P2 };
  TxWrite (&m, sizeof (m), NULL, 0);
}

/* ---------------- GLINK channel ---------------- */

STATIC VOID SendOpen(VOID)
{
  struct { GLINK_MSG M; CHAR8 Name[8]; } req;

  ZeroMem (&req, sizeof (req));
  req.M.Cmd = CMD_OPEN;
  req.M.P1  = CHAN_LCID;
  req.M.P2  = sizeof (CHAN_NAME);
  CopyMem (req.Name, CHAN_NAME, sizeof (CHAN_NAME));
  TxWrite (&req, sizeof (req), NULL, 0);
  mOpenSent = TRUE;
}

STATIC VOID AdvertiseIntents(UINT32 Count, UINT32 Size)
{
  struct { GLINK_MSG M; UINT32 Pair[2 * OUR_INTENTS]; } m;
  UINT32 i;

  m.M.Cmd = CMD_INTENT;
  m.M.P1  = CHAN_LCID;
  m.M.P2  = Count;
  for (i = 0; i < Count; i++) {
    m.Pair[2 * i]     = Size;
    m.Pair[2 * i + 1] = mNextIntent++;
  }
  TxWrite (&m, sizeof (GLINK_MSG) + 8 * Count, NULL, 0);
}

STATIC BOOLEAN ChanUp(VOID)
{
  return mRemoteOpen && mOpenAcked;
}

STATIC BOOLEAN GlinkTrySend(CONST VOID *Data, UINT32 Len)
{
  GLINK_DATA_HDR h;
  UINTN i;

  for (i = 0; i < mRiCount; i++) {
    if (!mRi[i].Used && mRi[i].Size >= Len) {
      break;
    }
  }
  if (i == mRiCount) {
    if (!mIntentReqSent) {
      SendCmd (CMD_RX_INTENT_REQ, CHAN_LCID, Len);
      mIntentReqSent = TRUE;
      if (gTrace > 0 && gTrace <= 50) {
        Out ("  [tx] RX_INTENT_REQ %u\r\n", Len);
        gTrace++;
      }
      if (mIntentReqs++ == 0) {
        Out ("  t=%u.%03u no modem intent for %u bytes, requesting (shown once)\r\n", T, Len);
      }
    }
    return FALSE;
  }
  if (gTrace > 0 && gTrace <= 50) {
    CONST UINT32 *q = Data;
    CONST UINT8  *t = (CONST UINT8 *)Data + 32;
    Out ("  [tx] iid %u (%u B) len %u qrtr t%u ->%x op %u blk %u\r\n", mRi[i].Id, mRi[i].Size, Len, q[1], q[7],
         t[1], (UINT32)(t[2] << 8 | t[3]));
    gTrace++;
  }
  h.M.Cmd = CMD_TX_DATA;
  h.M.P1  = CHAN_LCID;
  h.M.P2  = mRi[i].Id;
  h.Chunk = Len;
  h.Left  = 0;
  mRi[i].Used = TRUE;
  return TxWrite (&h, sizeof (h), Data, Len);
}

/* Packets wait here until the modem has a free RX intent for them. */
#define TXQ_LEN 64
STATIC VOID   *mTxq[TXQ_LEN];
STATIC UINT32  mTxqLen[TXQ_LEN];
STATIC UINTN   mTxqHead, mTxqTail, mTxqDrops;

STATIC VOID TxqPump(VOID)
{
  while (mTxqHead != mTxqTail && GlinkTrySend (mTxq[mTxqHead % TXQ_LEN], mTxqLen[mTxqHead % TXQ_LEN])) {
    FreePool (mTxq[mTxqHead % TXQ_LEN]);
    mTxqHead++;
  }
}

STATIC VOID GlinkSend(CONST VOID *Data, UINT32 Len)
{
  if (mTxqTail - mTxqHead >= TXQ_LEN) {
    mTxqDrops++;
    return;
  }
  mTxq[mTxqTail % TXQ_LEN]    = AllocateCopyPool (Len, Data);
  mTxqLen[mTxqTail % TXQ_LEN] = Len;
  mTxqTail++;
  TxqPump ();
}

/* ---------------- QRTR ---------------- */

VOID QrtrSend(UINT32 Type, UINT32 SrcPort, UINT32 DstNode, UINT32 DstPort, CONST VOID *Payload, UINT32 Len)
{
  UINT8 *buf = AllocateZeroPool (sizeof (QRTR_HDR_V1) + Len + 4);
  UINT32 n;

  if (buf == NULL) {
    return;
  }
  if (mQrtrVer == 2) {
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
    h->Version = 1;
    h->Type    = Type;
    h->SrcNode = QRTR_NODE_APPS;
    h->SrcPort = SrcPort;
    h->ConfirmRx = (Type == QRTR_TYPE_DATA) ? 1 : 0;   /* QRTR flow control: ask for RESUME_TX */
    h->Size    = Len;
    h->DstNode = DstNode;
    h->DstPort = DstPort;
    n = sizeof (*h);
  }
  CopyMem (buf + n, Payload, Len);
  GlinkSend (buf, ALIGN_VALUE (n + Len, 4));
  FreePool (buf);
}

UINT32 QrtrModemNode(VOID)
{
  return mModemNode;
}

STATIC VOID QrtrSendCtrl(UINT32 Type, CONST QRTR_CTRL *C)
{
  QrtrSend (Type, QRTR_PORT_CTRL, mModemNode, QRTR_PORT_CTRL, C, sizeof (*C));
}

STATIC VOID QrtrRx(CONST UINT8 *P, UINT32 Len)
{
  UINT32 type, srcNode, srcPort, dstNode, dstPort, size, hlen, confirm;
  CONST QRTR_CTRL *c;

  if (Len >= sizeof (QRTR_HDR_V1) && *(CONST UINT32 *)P == 1) {
    CONST QRTR_HDR_V1 *h = (CONST QRTR_HDR_V1 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort;
    dstNode = h->DstNode; dstPort = h->DstPort; size = h->Size; hlen = sizeof (*h);
    confirm = h->ConfirmRx;
    mQrtrVer = 1;
  } else if (Len >= sizeof (QRTR_HDR_V2) && P[0] == 2) {
    CONST QRTR_HDR_V2 *h = (CONST QRTR_HDR_V2 *)P;
    type = h->Type; srcNode = h->SrcNode; srcPort = h->SrcPort;
    dstNode = h->DstNode; dstPort = h->DstPort; size = h->Size; hlen = sizeof (*h) + h->OptLen * 4;
    srcPort = (srcPort == 0xFFFE) ? QRTR_PORT_CTRL : srcPort;
    dstPort = (dstPort == 0xFFFE) ? QRTR_PORT_CTRL : dstPort;
    confirm = h->Flags & 1;
    mQrtrVer = 2;
  } else {
    Out ("  t=%u.%03u qrtr: bad packet, %u bytes, %02x %02x %02x %02x\r\n", T, Len, P[0], P[1], P[2], P[3]);
    return;
  }
  c = (CONST QRTR_CTRL *)(P + hlen);

  switch (type) {
  case QRTR_TYPE_HELLO:
    mModemNode = srcNode;
    mHelloRx = TRUE;
    Out ("  t=%u.%03u qrtr v%u HELLO from node %u\r\n", T, mQrtrVer, srcNode);
    mHelloSent = FALSE;                         /* (re)answer with our HELLO */
    break;
  case QRTR_TYPE_NEW_SERVER:
    if (mSrvCount < ARRAY_SIZE (mSrv)) {
      mSrv[mSrvCount].Svc  = c->A;
      mSrv[mSrvCount].Inst = c->B;
      mSrv[mSrvCount].Node = c->C;
      mSrv[mSrvCount].Port = c->D;
      mSrvCount++;
    }
    if (c->A == QMI_SVC_WLFW) {
      Out ("  t=%u.%03u *** WLFW service 0x45 up: inst %x node %u port %x ***\r\n", T, c->B, c->C, c->D);
      WlfwArrive (c->C, c->D);
    }
    WwanArrive (c->A, c->C, c->D);
    break;
  case QRTR_TYPE_DATA:
    if (confirm) {
      /* full 20-byte qrtr_ctrl_pkt like Linux qrtr_send_resume_tx(); 12 bytes was ignored by the
         modem, which then blocked after 10 packets to one port (its QRTR_TX_FLOW_HIGH) */
      QRTR_CTRL r = { QRTR_TYPE_RESUME_TX, QRTR_NODE_APPS, dstPort, 0, 0 };
      QrtrSend (QRTR_TYPE_RESUME_TX, dstPort, srcNode, srcPort, &r, sizeof (r));
    }
    if (hlen + size > Len || !SvcRx (srcNode, srcPort, dstPort, P + hlen, size)) {
      mOther++;
      if (mOther <= 40) {
        Out ("  t=%u.%03u qrtr DATA %u:%x -> %u:%x %u B qmi %02x txn %04x msg %04x\r\n", T, srcNode, srcPort,
             dstNode, dstPort, size, P[hlen], *(CONST UINT16 *)(P + hlen + 1), *(CONST UINT16 *)(P + hlen + 3));
      }
    }
    break;
  case QRTR_TYPE_RESUME_TX:
    mResumes++;
    break;
  default:
    mOther++;
    if (mOther <= 40) {
      Out ("  t=%u.%03u qrtr type %u from %u:%x: %x %x %x %x\r\n", T, type, srcNode, srcPort, c->A, c->B, c->C, c->D);
    }
    break;
  }
}

/* ---------------- GLINK RX dispatcher ---------------- */

STATIC BOOLEAN RxOne(VOID)
{
  GLINK_MSG m;
  UINT32 avail = RxAvail (), n, i;

  if (avail < sizeof (m)) {
    return FALSE;
  }
  RxPeek (0, &m, sizeof (m));
  mRxMsgs++;
  if (gTrace > 0 && gTrace <= 50) {
    UINT32 q[8] = { 0 };
    if ((m.Cmd == CMD_TX_DATA || m.Cmd == CMD_TX_DATA_CONT) && avail >= 16 + 32) {
      RxPeek (16, q, 32);                       /* qrtr v1 header */
    }
    Out ("  [rx] cmd %u p1 %u p2 %u%a qrtr t%u %x->%x\r\n", m.Cmd, m.P1, m.P2,
         m.Cmd == CMD_TX_DATA ? " DATA" : "", q[1], q[3], q[7]);
    gTrace++;
  }

  switch (m.Cmd) {
  case CMD_VERSION:
    Out ("  t=%u.%03u glink VERSION %u features %x\r\n", T, m.P1, m.P2);
    RxAdvance (8);
    if (m.P1 != 0) {
      mFeatures &= m.P2;
      SendCmd (CMD_VERSION_ACK, 1, mFeatures);
    }
    break;
  case CMD_VERSION_ACK:
    Out ("  t=%u.%03u glink VERSION_ACK %u features %x\r\n", T, m.P1, m.P2);
    RxAdvance (8);
    if (m.P1 != 0 && m.P2 != mFeatures) {
      mFeatures &= m.P2;
      SendCmd (CMD_VERSION, 1, mFeatures);
    }
    break;
  case CMD_OPEN: {
    CHAR8 name[33];
    n = ALIGN_VALUE (8 + m.P2, 8);
    if (avail < n) {
      return FALSE;
    }
    ZeroMem (name, sizeof (name));
    RxPeek (8, name, MIN (m.P2, 32));
    RxAdvance (n);
    Out ("  t=%u.%03u glink modem OPEN \"%a\" rcid %u\r\n", T, name, m.P1);
    if (AsciiStrCmp (name, CHAN_NAME) == 0) {
      mRcid = m.P1;
      mRemoteOpen = TRUE;
      SendCmd (CMD_OPEN_ACK, (UINT16)mRcid, 0);
      if (!mOpenSent) {
        SendOpen ();
      }
    }
    break;
  }
  case CMD_OPEN_ACK:
    RxAdvance (8);
    Out ("  t=%u.%03u glink OPEN_ACK lcid %u\r\n", T, m.P1);
    if (m.P1 == CHAN_LCID) {
      mOpenAcked = TRUE;
    }
    break;
  case CMD_CLOSE:
    RxAdvance (8);
    Out ("  t=%u.%03u glink modem CLOSE rcid %u\r\n", T, m.P1);
    SendCmd (CMD_CLOSE_ACK, m.P1, 0);
    if (m.P1 == mRcid) {
      mRemoteOpen = FALSE;
    }
    break;
  case CMD_INTENT:
    n = ALIGN_VALUE (8 + 8 * m.P2, 8);
    if (avail < n) {
      return FALSE;
    }
    for (i = 0; i < m.P2; i++) {
      UINT32 pair[2];
      RxPeek (8 + 8 * i, pair, 8);
      if (m.P1 == mRcid && mRiCount == ARRAY_SIZE (mRi)) {
        Out ("  t=%u.%03u glink: intent table full!\r\n", T);
      }
      if (m.P1 == mRcid && mRiCount < ARRAY_SIZE (mRi)) {
        mRi[mRiCount].Size = pair[0];
        mRi[mRiCount].Id   = pair[1];
        mRi[mRiCount].Used = FALSE;
        mRiCount++;
      }
    }
    RxAdvance (n);
    mIntentReqSent = FALSE;
    if (mRiCount <= 3) {
      Out ("  t=%u.%03u glink modem INTENT x%u (chan %u), have %u\r\n", T, m.P2, m.P1, (UINT32)mRiCount);
    }
    break;
  case CMD_RX_DONE:
  case CMD_RX_DONE_W_REUSE:
    RxAdvance (8);
    for (i = 0; i < mRiCount; i++) {
      if (mRi[i].Id == m.P2) {
        if (m.Cmd == CMD_RX_DONE_W_REUSE) {
          mRi[i].Used = FALSE;
        } else {
          mRi[i] = mRi[--mRiCount];
        }
        break;
      }
    }
    break;
  case CMD_RX_INTENT_REQ:
    RxAdvance (8);
    Out ("  t=%u.%03u glink modem wants an intent of %u bytes\r\n", T, m.P2);
    AdvertiseIntents (1, MAX (m.P2, OUR_INTENT_SIZE));
    SendCmd (CMD_RX_INTENT_REQ_ACK, CHAN_LCID, 1);
    break;
  case CMD_RX_INTENT_REQ_ACK:
    RxAdvance (8);
    if (m.P2 == 0) {
      mIntentDenied++;
      if (mIntentDenied <= 3) {
        Out ("  t=%u.%03u glink: modem DENIED an intent (#%u)\r\n", T, (UINT32)mIntentDenied);
      }
      mIntentReqSent = FALSE;                   /* ask again on the next pump */
    }
    break;
  case CMD_CLOSE_ACK:
  case CMD_SIGNALS:
    RxAdvance (8);
    break;
  case CMD_READ_NOTIF:
    RxAdvance (8);
    ApcsKick (GLINK_IRQ_BIT);
    break;
  case CMD_TX_DATA:
  case CMD_TX_DATA_CONT: {
    UINT32 hdr[2];
    if (avail < 16) {
      return FALSE;
    }
    RxPeek (8, hdr, 8);                         /* chunk_size, left_size */
    n = ALIGN_VALUE (16 + hdr[0], 8);
    if (avail < n) {
      return FALSE;
    }
    if (m.Cmd == CMD_TX_DATA) {
      mAsmLen = 0;
    }
    if (mAsmLen + hdr[0] <= sizeof (mAsm)) {
      RxPeek (16, mAsm + mAsmLen, hdr[0]);
      mAsmLen += hdr[0];
    }
    RxAdvance (n);
    if (hdr[1] == 0) {
      if (m.P1 == mRcid) {
        QrtrRx (mAsm, mAsmLen);
      }
      if (mFeatures & 1) {
        SendCmd (CMD_RX_DONE_W_REUSE, CHAN_LCID, m.P2);
      } else {
        SendCmd (CMD_RX_DONE, CHAN_LCID, m.P2);
        AdvertiseIntents (1, OUR_INTENT_SIZE);
      }
    }
    break;
  }
  default:
    Out ("  t=%u.%03u glink unknown cmd %u (%x %x), RX stopped\r\n", T, m.Cmd, m.P1, m.P2);
    mRx = NULL;
    return FALSE;
  }
  return TRUE;
}

STATIC CONST CHAR8 *SvcName(UINT32 Svc)
{
  switch (Svc) {
  case 0x45: return "WLFW";
  case 0x0E: return "rmtfs";
  case 0x40: return "servreg-loc";
  case 0x42: return "servreg-notif";
  case 0x2B: return "ssctl";
  case 0x31: return "ipa";
  case 0x01: return "wds";
  case 0x02: return "dms";
  case 0x03: return "nas";
  case 0x0B: return "uim";
  case 0x1A: return "wda";
  default:   return "";
  }
}

VOID GlinkQrtrSpike(UINTN Seconds, EFI_FILE_PROTOCOL *Root)
{
  SMEM_PART_HDR *part = SmemPartition (0, 1);
  UINT32 sz = 0, i;
  BOOLEAN existed;

  Out ("  ---- P2: GLINK/QRTR ----\r\n");
  SvcInit (Root);
  existed = SmemPrivGet (part, SMEM_GLINK_DESC, NULL) != NULL;
  mDesc = SmemPrivAlloc (part, SMEM_GLINK_DESC, 32);
  mTx   = SmemPrivAlloc (part, SMEM_GLINK_FIFO_TX, GLINK_TX_SIZE);
  if (mDesc == NULL || mTx == NULL) {
    Out ("  glink: SMEM alloc failed (desc %p tx %p)\r\n", mDesc, mTx);
    return;
  }
  Out ("  glink desc %p (%a) tx %p\r\n", mDesc, existed ? "modem made it" : "ours", mTx);
  mDesc[2] = 0;                                 /* rx tail */
  mDesc[1] = 0;                                 /* tx head */
  MemoryFence ();
  SendCmd (CMD_VERSION, 1, mFeatures);

  mT0 = 0;
  mT0 = ModemMs ();
  LogSetLazy (TRUE);
  for (mNowMs = 0; (Seconds == 0 || mNowMs < Seconds * 1000) && !gModemStop; mNowMs = ModemMs ()) {
    BOOLEAN busy;
    UINTN rx0 = mRxMsgs, tx0 = mTxMsgs;
    if (gModemState != NULL && (*gModemState & 1)) {
      Out ("  t=%u.%03u *** modem FATAL (slave-kernel %08x) ***\r\n", T, *gModemState);
      break;
    }
    if (mNowMs - mLastStatus >= 30000) {
      mLastStatus = mNowMs;
      Out ("  t=%u.%03u status: slave-kernel %08x, glink rx %u tx %u, %u services, WLFW: %a\r\n", T,
           gModemState != NULL ? *gModemState : 0, (UINT32)mRxMsgs, (UINT32)mTxMsgs, (UINT32)mSrvCount, WlfwStep ());
    }
    if (mRx == NULL && mRxMsgs == 0) {
      mRx = SmemPrivGet (part, SMEM_GLINK_FIFO_RX, &sz);
      if (mRx != NULL) {
        mRxLen = sz;
        Out ("  t=%u.%03u modem RX FIFO %p, %u bytes\r\n", T, mRx, sz);
      }
    }
    if (mRx != NULL) {
      for (i = 0; i < 64 && mRx != NULL && RxOne (); i++) {
      }
    }
    TxqPump ();
    if (ChanUp () && !mIntentsSent) {
      AdvertiseIntents (OUR_INTENTS, OUR_INTENT_SIZE);
      mIntentsSent = TRUE;
      Out ("  t=%u.%03u IPCRTR open both ways, %u intents sent\r\n", T, OUR_INTENTS);
    }
    if (ChanUp () && !mOpenSent) {
      SendOpen ();
    }
    if (ChanUp () && !mHelloSent) {
      QRTR_CTRL c;
      ZeroMem (&c, sizeof (c));
      c.Cmd = QRTR_TYPE_HELLO;
      QrtrSendCtrl (QRTR_TYPE_HELLO, &c);
      mHelloSent = TRUE;
      Out ("  t=%u.%03u sent qrtr HELLO to node %x\r\n", T, mModemNode);
      if (mHelloRx && !mAnnounced) {
        SvcAnnounce ();
        mAnnounced = TRUE;
      }
    }
    /* Nobody opened IPCRTR after 2 s: open it from our side. */
    if (mNowMs >= 2000 && !mOpenSent && mRx != NULL) {
      Out ("  t=2.000 modem did not open IPCRTR, opening it\r\n");
      SendOpen ();
    }
    busy = CePoll ();                           /* WLAN copy engines (no-op until CeStart) */
    ScanPoll ();
    HttPoll ();
    WwanPoll ();                                /* SIM / network status (QMI, log only) */
#ifdef TOPAZ_WIFICX
    AssocPoll ();                               /* TopazWifi: station association */
    HttTxPoll ();                               /* TopazWifi: data frames from Windows */
#endif
    busy = busy || mRxMsgs != rx0 || mTxMsgs != tx0;
    ModemIdle (busy);
  }
  LogSetLazy (FALSE);

  {
    UINT32 lo = MAX_UINT32, hi = 0;
    for (i = 0; i < mRiCount; i++) {
      lo = MIN (lo, mRi[i].Size);
      hi = MAX (hi, mRi[i].Size);
    }
    Out ("  glink: rx %u msgs, tx %u, chan %a, modem intents %u (%u..%u B), %u requested, txq %u drops %u\r\n",
         (UINT32)mRxMsgs, (UINT32)mTxMsgs, ChanUp () ? "UP" : "down", (UINT32)mRiCount, lo, hi, (UINT32)mIntentReqs,
         (UINT32)(mTxqTail - mTxqHead), (UINT32)mTxqDrops);
  }
  {
    UINT32 used = 0;
    for (i = 0; i < mRiCount; i++) {
      used += mRi[i].Used ? 1 : 0;
    }
    Out ("  intents: %u used, req pending %u, denied %u; head pkt %u B; rx avail %u, tx head %x tail %x\r\n",
         used, mIntentReqSent, (UINT32)mIntentDenied,
         mTxqHead != mTxqTail ? mTxqLen[mTxqHead % TXQ_LEN] : 0, mRx != NULL ? RxAvail () : 0, mDesc[1], mDesc[0]);
  }
  SvcSummary ();
  WwanSummary ();
  HtcSummary ();
  WmiSummary ();
  ScanSummary ();
  HttSummary ();
  CeSummary ();
  Out ("  qrtr: modem node %x, hello rx %u, %u services, %u RESUME_TX\r\n", mModemNode, mHelloRx,
       (UINT32)mSrvCount, (UINT32)mResumes);
  for (i = 0; i < mSrvCount; i++) {
    if (*SvcName (mSrv[i].Svc) != 0 && mSrv[i].Svc != 0x42 && mSrv[i].Svc != 0x2B) {
      Out ("   %x:%x@%x %a\r\n", mSrv[i].Svc, mSrv[i].Inst, mSrv[i].Port, SvcName (mSrv[i].Svc));
    }
  }
}
