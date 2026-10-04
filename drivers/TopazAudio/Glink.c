/*
 * GLINK over SMEM towards the ADSP (Linux qcom_glink_smem.c + qcom_glink_native.c), polled.
 * Same FIFO layout as drivers/TopazModem/Glink.c (item 478 = ring indices, 479 = our TX FIFO,
 * 480 = the remote's TX FIFO) but in the apps<->adsp partition, doorbell APCS IPC bit 8, and
 * with a small channel table: IPCRTR (QRTR) and adsp_apps (GPR). Channels the ADSP opens that
 * nobody registered are acked and logged, like Linux does when no rpmsg driver binds.
 */
#include "Audio.h"

#define Out ModemOut

#define SMEM_GLINK_DESC    478
#define SMEM_GLINK_FIFO_TX 479
#define SMEM_GLINK_FIFO_RX 480
#define GLINK_TX_SIZE      SIZE_16KB

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

#define MAX_CHANS          8
#define OUR_INTENTS        8
#define OUR_INTENT_SIZE    0x4400
#define MAX_RINTENTS       256
#define TXQ_LEN            32

#pragma pack(1)
typedef struct { UINT16 Cmd, P1; UINT32 P2; } GLINK_MSG;
typedef struct { GLINK_MSG M; UINT32 Chunk, Left; } GLINK_DATA_HDR;
#pragma pack()

typedef struct { UINT32 Id, Size; BOOLEAN Used; } REMOTE_INTENT;

typedef struct {
  CHAR8         Name[32];
  GLINK_RX      Rx;                          /* NULL: not ours, only acked */
  BOOLEAN       OpenFirst;                   /* send OPEN without waiting for the remote */
  UINT16        Lcid;
  UINT32        Rcid;
  BOOLEAN       RemoteOpen, OpenSent, OpenAcked, IntentsSent, IntentReqSent;
  REMOTE_INTENT Ri[MAX_RINTENTS];
  UINT32        RiCount;
  UINT8        *Asm;                         /* reassembly buffer, OUR_INTENT_SIZE */
  UINT32        AsmLen;
  VOID         *Txq[TXQ_LEN];
  UINT32        TxqLen[TXQ_LEN];
  UINTN         TxqHead, TxqTail, TxqDrops;
  UINTN         RxPkts, TxPkts, RiLost, IntentReqs;
} CHAN;

STATIC volatile UINT32 *mDesc;               /* [0] tx tail [1] tx head [2] rx tail [3] rx head */
STATIC UINT8   *mTx, *mRx;
STATIC UINT32   mRxLen;
STATIC UINT32   mFeatures = 1;               /* INTENT_REUSE */
STATIC UINT32   mNextIntent = 1;
STATIC CHAN     mCh[MAX_CHANS];
STATIC UINT32   mChCount;
STATIC SMEM_PART_HDR *mPart;
STATIC UINTN    mRxMsgs, mTxMsgs;
STATIC BOOLEAN  mDead;

#define T  (UINT32)(AudMs () / 1000), (UINT32)(AudMs () % 1000)

/* ---------------- SMEM pipes ---------------- */

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
  return (Head >= GLINK_TX_SIZE) ? Head - GLINK_TX_SIZE : Head;
}

STATIC BOOLEAN TxWrite(CONST VOID *Hdr, UINT32 HLen, CONST VOID *Data, UINT32 DLen)
{
  UINT32 head = 0, tail = 0, avail, n;

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
  ApcsKick (ADSP_GLINK_BIT);
  mTxMsgs++;
  return TRUE;
}

STATIC VOID SendCmd(UINT16 Cmd, UINT16 P1, UINT32 P2)
{
  GLINK_MSG m = { Cmd, P1, P2 };
  TxWrite (&m, sizeof (m), NULL, 0);
}

/* ---------------- channels ---------------- */

STATIC CHAN *ByName(CONST CHAR8 *Name)
{
  UINT32 i;

  for (i = 0; i < mChCount; i++) {
    if (AsciiStrCmp (mCh[i].Name, Name) == 0) {
      return &mCh[i];
    }
  }
  return NULL;
}

STATIC CHAN *ByRcid(UINT32 Rcid)
{
  UINT32 i;

  for (i = 0; i < mChCount; i++) {
    if (mCh[i].RemoteOpen && mCh[i].Rcid == Rcid) {
      return &mCh[i];
    }
  }
  return NULL;
}

STATIC CHAN *AddChan(CONST CHAR8 *Name)
{
  CHAN *c;

  if (mChCount == MAX_CHANS) {
    return NULL;
  }
  c = &mCh[mChCount];
  ZeroMem (c, sizeof (*c));
  AsciiStrnCpyS (c->Name, sizeof (c->Name), Name, sizeof (c->Name) - 1);
  c->Lcid = (UINT16)(mChCount + 1);
  mChCount++;
  return c;
}

VOID GlinkRegister(CONST CHAR8 *Name, GLINK_RX Rx, BOOLEAN OpenFirst)
{
  CHAN *c = ByName (Name);

  if (c == NULL) {
    c = AddChan (Name);
  }
  if (c != NULL) {
    c->Rx = Rx;
    c->OpenFirst = OpenFirst;
    c->Asm = AllocateZeroPool (OUR_INTENT_SIZE);
  }
}

STATIC VOID SendOpen(CHAN *C)
{
  struct { GLINK_MSG M; CHAR8 Name[32]; } req;
  UINT32 len = (UINT32)AsciiStrLen (C->Name) + 1;

  ZeroMem (&req, sizeof (req));
  req.M.Cmd = CMD_OPEN;
  req.M.P1  = C->Lcid;
  req.M.P2  = len;
  CopyMem (req.Name, C->Name, len);
  TxWrite (&req, ALIGN_VALUE (8 + len, 8), NULL, 0);
  C->OpenSent = TRUE;
  Out ("  t=%u.%03u glink OPEN \"%a\" lcid %u\r\n", T, C->Name, C->Lcid);
}

STATIC VOID AdvertiseIntents(CHAN *C, UINT32 Count, UINT32 Size)
{
  struct { GLINK_MSG M; UINT32 Pair[2 * OUR_INTENTS]; } m;
  UINT32 i;

  Count = MIN (Count, OUR_INTENTS);
  m.M.Cmd = CMD_INTENT;
  m.M.P1  = C->Lcid;
  m.M.P2  = Count;
  for (i = 0; i < Count; i++) {
    m.Pair[2 * i]     = Size;
    m.Pair[2 * i + 1] = mNextIntent++;
  }
  TxWrite (&m, sizeof (GLINK_MSG) + 8 * Count, NULL, 0);
}

STATIC BOOLEAN Up(CONST CHAN *C)
{
  return C->RemoteOpen && C->OpenAcked;
}

BOOLEAN GlinkChanUp(CONST CHAR8 *Name)
{
  CHAN *c = ByName (Name);
  return c != NULL && Up (c);
}

STATIC BOOLEAN TrySend(CHAN *C, CONST VOID *Data, UINT32 Len)
{
  GLINK_DATA_HDR h;
  UINT32 i;

  for (i = 0; i < C->RiCount; i++) {
    if (!C->Ri[i].Used && C->Ri[i].Size >= Len) {
      break;
    }
  }
  if (i == C->RiCount) {
    if (!C->IntentReqSent) {
      SendCmd (CMD_RX_INTENT_REQ, C->Lcid, Len);
      C->IntentReqSent = TRUE;
      C->IntentReqs++;
    }
    return FALSE;
  }
  h.M.Cmd = CMD_TX_DATA;
  h.M.P1  = C->Lcid;
  h.M.P2  = C->Ri[i].Id;
  h.Chunk = Len;
  h.Left  = 0;
  C->Ri[i].Used = TRUE;
  C->TxPkts++;
  return TxWrite (&h, sizeof (h), Data, Len);
}

STATIC VOID Pump(CHAN *C)
{
  while (Up (C) && C->TxqHead != C->TxqTail && TrySend (C, C->Txq[C->TxqHead % TXQ_LEN], C->TxqLen[C->TxqHead % TXQ_LEN])) {
    FreePool (C->Txq[C->TxqHead % TXQ_LEN]);
    C->TxqHead++;
  }
}

/* Queue a packet; it goes out when the channel is up and the ADSP has a free intent. */
BOOLEAN GlinkSend(CONST CHAR8 *Name, CONST VOID *Data, UINT32 Len)
{
  CHAN *c = ByName (Name);

  if (c == NULL || c->TxqTail - c->TxqHead >= TXQ_LEN) {
    if (c != NULL) {
      c->TxqDrops++;
    }
    return FALSE;
  }
  c->Txq[c->TxqTail % TXQ_LEN]    = AllocateCopyPool (Len, Data);
  c->TxqLen[c->TxqTail % TXQ_LEN] = Len;
  c->TxqTail++;
  Pump (c);
  return TRUE;
}

/* ---------------- RX dispatcher ---------------- */

STATIC BOOLEAN RxOne(VOID)
{
  GLINK_MSG m;
  UINT32 avail = RxAvail (), n, i;
  CHAN *c;

  if (avail < sizeof (m)) {
    return FALSE;
  }
  RxPeek (0, &m, sizeof (m));
  mRxMsgs++;

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
    c = ByName (name);
    if (c == NULL) {
      c = AddChan (name);
    }
    Out ("  t=%u.%03u glink ADSP OPEN \"%a\" rcid %u%a\r\n", T, name, m.P1, (c != NULL && c->Rx != NULL) ? "" : " (no client)");
    if (c != NULL) {
      c->Rcid = m.P1;
      c->RemoteOpen = TRUE;
      SendCmd (CMD_OPEN_ACK, (UINT16)c->Rcid, 0);
      if (c->Rx != NULL && !c->OpenSent) {
        SendOpen (c);
      }
    }
    break;
  }
  case CMD_OPEN_ACK:
    RxAdvance (8);
    for (i = 0; i < mChCount; i++) {
      if (mCh[i].Lcid == m.P1) {
        mCh[i].OpenAcked = TRUE;
        Out ("  t=%u.%03u glink OPEN_ACK \"%a\"\r\n", T, mCh[i].Name);
      }
    }
    break;
  case CMD_CLOSE:
    RxAdvance (8);
    c = ByRcid (m.P1);
    Out ("  t=%u.%03u glink ADSP CLOSE rcid %u \"%a\"\r\n", T, m.P1, c != NULL ? c->Name : "?");
    SendCmd (CMD_CLOSE_ACK, m.P1, 0);
    if (c != NULL) {
      c->RemoteOpen = FALSE;
      c->RiCount = 0;
    }
    break;
  case CMD_INTENT:
    n = ALIGN_VALUE (8 + 8 * m.P2, 8);
    if (avail < n) {
      return FALSE;
    }
    c = ByRcid (m.P1);
    for (i = 0; c != NULL && i < m.P2; i++) {
      UINT32 pair[2];
      RxPeek (8 + 8 * i, pair, 8);
      if (c->RiCount < MAX_RINTENTS) {
        c->Ri[c->RiCount].Size = pair[0];
        c->Ri[c->RiCount].Id   = pair[1];
        c->Ri[c->RiCount].Used = FALSE;
        c->RiCount++;
      } else {
        c->RiLost++;                            /* table full: an intent we can never use */
      }
    }
    RxAdvance (n);
    if (c != NULL) {
      c->IntentReqSent = FALSE;
      Pump (c);
    }
    break;
  case CMD_RX_DONE:
  case CMD_RX_DONE_W_REUSE:
    RxAdvance (8);
    c = ByRcid (m.P1);
    for (i = 0; c != NULL && i < c->RiCount; i++) {
      if (c->Ri[i].Id == m.P2) {
        if (m.Cmd == CMD_RX_DONE_W_REUSE) {
          c->Ri[i].Used = FALSE;
        } else {
          c->Ri[i] = c->Ri[--c->RiCount];
        }
        break;
      }
    }
    if (c != NULL) {
      Pump (c);
    }
    break;
  case CMD_RX_INTENT_REQ:
    RxAdvance (8);
    c = ByRcid (m.P1);
    Out ("  t=%u.%03u glink \"%a\" wants an intent of %u bytes\r\n", T, c != NULL ? c->Name : "?", m.P2);
    if (c != NULL) {
      AdvertiseIntents (c, 1, MAX (m.P2, OUR_INTENT_SIZE));
      SendCmd (CMD_RX_INTENT_REQ_ACK, c->Lcid, 1);
    }
    break;
  case CMD_RX_INTENT_REQ_ACK:
    RxAdvance (8);
    c = ByRcid (m.P1);
    if (c != NULL && m.P2 == 0) {
      c->IntentReqSent = FALSE;
    }
    break;
  case CMD_CLOSE_ACK:
  case CMD_SIGNALS:
    RxAdvance (8);
    break;
  case CMD_READ_NOTIF:
    RxAdvance (8);
    ApcsKick (ADSP_GLINK_BIT);
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
    c = ByRcid (m.P1);
    if (c != NULL && c->Asm != NULL) {
      if (m.Cmd == CMD_TX_DATA) {
        c->AsmLen = 0;
      }
      if (c->AsmLen + hdr[0] <= OUR_INTENT_SIZE) {
        RxPeek (16, c->Asm + c->AsmLen, hdr[0]);
        c->AsmLen += hdr[0];
      }
    }
    RxAdvance (n);
    if (hdr[1] == 0 && c != NULL) {
      c->RxPkts++;
      if (c->Rx != NULL) {
        c->Rx (c->Asm, c->AsmLen);
      }
      if (mFeatures & 1) {
        SendCmd (CMD_RX_DONE_W_REUSE, c->Lcid, m.P2);
      } else {
        SendCmd (CMD_RX_DONE, c->Lcid, m.P2);
        AdvertiseIntents (c, 1, OUR_INTENT_SIZE);
      }
    }
    break;
  }
  default:
    Out ("  t=%u.%03u glink unknown cmd %u (%x %x), RX stopped\r\n", T, m.Cmd, m.P1, m.P2);
    mDead = TRUE;
    return FALSE;
  }
  return TRUE;
}

/* ---------------- edge ---------------- */

VOID GlinkInit(SMEM_PART_HDR *Part)
{
  BOOLEAN existed;

  mPart = Part;
  existed = SmemPrivGet (Part, SMEM_GLINK_DESC, NULL) != NULL;
  mDesc = SmemPrivAlloc (Part, SMEM_GLINK_DESC, 32);
  mTx   = SmemPrivAlloc (Part, SMEM_GLINK_FIFO_TX, GLINK_TX_SIZE);
  if (mDesc == NULL || mTx == NULL) {
    Out ("  glink: SMEM alloc failed (desc %p tx %p)\r\n", mDesc, mTx);
    mDead = TRUE;
    return;
  }
  Out ("  glink desc %p (%a) tx %p\r\n", mDesc, existed ? "existed" : "ours", mTx);
  mDesc[2] = 0;                                 /* rx tail */
  mDesc[1] = 0;                                 /* tx head */
  MemoryFence ();
  SendCmd (CMD_VERSION, 1, mFeatures);
}

BOOLEAN GlinkPoll(VOID)
{
  UINTN rx0 = mRxMsgs, tx0 = mTxMsgs;
  UINT32 sz = 0, i;

  if (mDead || mDesc == NULL) {
    return FALSE;
  }
  if (mRx == NULL) {
    mRx = SmemPrivGet (mPart, SMEM_GLINK_FIFO_RX, &sz);
    if (mRx == NULL) {
      return FALSE;
    }
    mRxLen = sz;
    Out ("  t=%u.%03u ADSP RX FIFO %p, %u bytes\r\n", T, mRx, sz);
  }
  for (i = 0; i < 64 && !mDead && RxOne (); i++) {
  }
  for (i = 0; i < mChCount; i++) {
    CHAN *c = &mCh[i];
    if (c->Rx == NULL) {
      continue;
    }
    /* normally the ADSP opens its channels first; if it has not after 8 s, open from our side */
    if (!c->OpenSent && AudMs () >= (c->OpenFirst ? 0 : 8000)) {
      SendOpen (c);
    }
    if (Up (c) && !c->IntentsSent) {
      AdvertiseIntents (c, OUR_INTENTS, OUR_INTENT_SIZE);
      c->IntentsSent = TRUE;
      Out ("  t=%u.%03u \"%a\" open both ways\r\n", T, c->Name);
    }
    Pump (c);
  }
  return mRxMsgs != rx0 || mTxMsgs != tx0;
}

VOID GlinkSummary(VOID)
{
  UINT32 i;

  Out ("  glink: rx %u msgs, tx %u%a\r\n", (UINT32)mRxMsgs, (UINT32)mTxMsgs, mDead ? " (DEAD)" : "");
  for (i = 0; i < mChCount; i++) {
    CHAN *c = &mCh[i];
    UINT32 j, used = 0, lo = MAX_UINT32, hi = 0;
    for (j = 0; j < c->RiCount; j++) {
      used += c->Ri[j].Used ? 1 : 0;
      lo = MIN (lo, c->Ri[j].Size);
      hi = MAX (hi, c->Ri[j].Size);
    }
    Out ("   chan \"%a\" lcid %u rcid %u %a%a rx %u tx %u, remote intents %u (%u used, %u..%u B, %u lost, %u req), txq %u (head %u B) drops %u\r\n",
         c->Name, c->Lcid, c->Rcid, Up (c) ? "UP" : "down", c->Rx == NULL ? " (no client)" : "", (UINT32)c->RxPkts,
         (UINT32)c->TxPkts, c->RiCount, used, c->RiCount ? lo : 0, hi, (UINT32)c->RiLost, (UINT32)c->IntentReqs,
         (UINT32)(c->TxqTail - c->TxqHead), c->TxqHead != c->TxqTail ? c->TxqLen[c->TxqHead % TXQ_LEN] : 0,
         (UINT32)c->TxqDrops);
  }
}
