/*
 * Lab interface (taudio_ioctl.h): user mode drives the speaker-path bring-up through
 * \\.\TopazAudio while the polling thread keeps the ADSP link alive.
 *  - GPR: IOCTL -> TX ring -> LabPoll() on the polling thread -> GlinkSend("adsp_apps");
 *         every GPR packet except our own SPF-state polling -> RX ring -> IOCTL.
 *  - MMIO: mapped per request, only in the allowed ranges.
 *  - I2C: AmpI2cXfer() (amp.c, TopazBattery quiet window).
 *  - BUF: contiguous non-cached buffers below 4 GiB for ADSP shared memory.
 */
#include "Audio.h"
#include "taudio_ioctl.h"

#define Out ModemOut

#define RX_SLOTS 64
#define TX_SLOTS 16
#define BUF_MAX  8

typedef struct { UINT32 Len; UINT8 Data[TAUDIO_GPR_MAX]; } SLOT;

STATIC KSPIN_LOCK mLock;
STATIC SLOT     *mRx, *mTx;                  /* rings, nonpaged pool */
STATIC UINT32    mRxHead, mRxTail, mTxHead, mTxTail, mRxDropped;
STATIC struct { UINT8 *Va; UINT64 Pa; UINT32 Size; } mBuf[BUF_MAX];
STATIC BOOLEAN   mInit;

VOID LabInit(VOID)
{
  if (mInit) {
    return;
  }
  KeInitializeSpinLock (&mLock);
  mRx = AllocateZeroPool (sizeof (SLOT) * RX_SLOTS);
  mTx = AllocateZeroPool (sizeof (SLOT) * TX_SLOTS);
  mInit = (mRx != NULL && mTx != NULL);
}

/* polling thread: a GPR packet arrived */
VOID LabGprRx(CONST UINT8 *Data, UINT32 Len)
{
  KIRQL irql;

  if (!mInit) {
    return;
  }
  KeAcquireSpinLock (&mLock, &irql);
  if (mRxTail - mRxHead >= RX_SLOTS) {
    mRxDropped++;
  } else {
    SLOT *s = &mRx[mRxTail % RX_SLOTS];
    s->Len = MIN (Len, TAUDIO_GPR_MAX);
    CopyMem (s->Data, Data, s->Len);
    mRxTail++;
  }
  KeReleaseSpinLock (&mLock, irql);
}

/* polling thread: hand queued packets to GLINK */
VOID LabPoll(VOID)
{
  STATIC UINT8 pkt[TAUDIO_GPR_MAX];
  UINT32 len;
  KIRQL irql;

  while (mInit && GprIsUp ()) {
    KeAcquireSpinLock (&mLock, &irql);
    if (mTxHead == mTxTail) {
      KeReleaseSpinLock (&mLock, irql);
      return;
    }
    len = mTx[mTxHead % TX_SLOTS].Len;
    CopyMem (pkt, mTx[mTxHead % TX_SLOTS].Data, len);
    mTxHead++;
    KeReleaseSpinLock (&mLock, irql);
    GprSendRaw (pkt, len);
  }
}

STATIC BOOLEAN MmioAllowed(UINT64 Pa, UINT32 Bytes)
{
  STATIC CONST UINT64 r[][2] = {
    { 0x0A000000, 0x0B000000 },               /* LPASS */
    { 0x00400000, 0x01000000 },               /* TLMM */
    { 0x0C600000, 0x0C700000 },               /* apps SMMU */
  };
  UINTN i;

  if ((Pa & 3) != 0) {
    return FALSE;
  }
  for (i = 0; i < ARRAY_SIZE (r); i++) {
    if (Pa >= r[i][0] && Pa + Bytes <= r[i][1]) {
      return TRUE;
    }
  }
  return FALSE;
}

STATIC NTSTATUS DoMmio(TAUDIO_MMIO *M)
{
  UINT32 n = (M->Op == 0) ? MIN (MAX (M->Count, 1u), 64u) : 1, i;
  UINT64 base = M->Pa & ~0xFFFull;
  UINTN size = (UINTN)ALIGN_VALUE ((M->Pa - base) + n * 4, SIZE_4KB);
  UINT8 *va;

  if (M->Op > 1 || !MmioAllowed (M->Pa, n * 4)) {
    return STATUS_ACCESS_DENIED;
  }
  va = MapPhys (base, size, FALSE);
  if (va == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  if (M->Op == 1) {
    MmioWrite32 (va + (M->Pa - base), M->Value[0]);
  } else {
    for (i = 0; i < n; i++) {
      M->Value[i] = MmioRead32 (va + (M->Pa - base) + 4 * i);
    }
    M->Count = n;
  }
  UnmapPhys (va, size);
  return STATUS_SUCCESS;
}

STATIC NTSTATUS DoBuf(TAUDIO_BUF *B, UINT8 *Data, UINT32 DataMax, UINT32 *DataOut)
{
  UINT32 i;

  *DataOut = 0;
  if (B->Op == 0) {
    for (i = 0; i < BUF_MAX && mBuf[i].Va != NULL; i++) {
    }
    if (i == BUF_MAX || B->Size == 0 || B->Size > 16 * 1024 * 1024) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    mBuf[i].Va = PhysAlloc (B->Size, &mBuf[i].Pa);
    if (mBuf[i].Va == NULL) {
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    mBuf[i].Size = ALIGN_VALUE (B->Size, SIZE_4KB);
    B->Id = i;
    B->Pa = mBuf[i].Pa;
    B->Size = mBuf[i].Size;
    Out ("  lab: buffer %u = %lx + %x\r\n", i, mBuf[i].Pa, mBuf[i].Size);
    return STATUS_SUCCESS;
  }
  if (B->Id >= BUF_MAX || mBuf[B->Id].Va == NULL) {
    return STATUS_INVALID_PARAMETER;
  }
  if (B->Op == 3) {
    PhysFree (mBuf[B->Id].Va);
    mBuf[B->Id].Va = NULL;
    return STATUS_SUCCESS;
  }
  if (B->Op == 1 || B->Op == 2) {
    UINT32 n = (B->Op == 1) ? DataMax : MIN (B->Size, DataMax);
    if (B->Offset > mBuf[B->Id].Size || n > mBuf[B->Id].Size - B->Offset) {
      return STATUS_INVALID_PARAMETER;
    }
    if (B->Op == 1) {
      CopyMem (mBuf[B->Id].Va + B->Offset, Data, n);
    } else {
      CopyMem (Data, mBuf[B->Id].Va + B->Offset, n);
      *DataOut = n;
    }
    B->Pa = mBuf[B->Id].Pa;
    return STATUS_SUCCESS;
  }
  return STATUS_INVALID_PARAMETER;
}

/*
 * IOCTL dispatcher (PASSIVE_LEVEL, sequential queue). In = Out = the METHOD_BUFFERED system
 * buffer. Returns the number of bytes to copy back in *Info.
 */
NTSTATUS LabIoctl(ULONG Code, VOID *Buf, UINT32 InLen, UINT32 OutLen, UINT32 *Info)
{
  KIRQL irql;
  NTSTATUS st = STATUS_SUCCESS;

  *Info = 0;
  if (!mInit) {
    return STATUS_DEVICE_NOT_READY;
  }
  switch (Code) {
  case IOCTL_TAUDIO_STATE: {
    TAUDIO_STATE *s = Buf;
    if (OutLen < sizeof (*s)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    ZeroMem (s, sizeof (*s));
    s->AdspState = (gAdspState != NULL) ? *gAdspState : 0;
    s->GprUp = GprIsUp ();
    s->SpfState = GprSpfState ();
    KeAcquireSpinLock (&mLock, &irql);
    s->RxQueued = mRxTail - mRxHead;
    s->RxDropped = mRxDropped;
    s->TxQueued = mTxTail - mTxHead;
    KeReleaseSpinLock (&mLock, irql);
    *Info = sizeof (*s);
    break;
  }
  case IOCTL_TAUDIO_GPR_SEND:
    if (InLen < 24 || InLen > TAUDIO_GPR_MAX) {
      return STATUS_INVALID_PARAMETER;
    }
    KeAcquireSpinLock (&mLock, &irql);
    if (mTxTail - mTxHead >= TX_SLOTS) {
      st = STATUS_DEVICE_BUSY;
    } else {
      mTx[mTxTail % TX_SLOTS].Len = InLen;
      CopyMem (mTx[mTxTail % TX_SLOTS].Data, Buf, InLen);
      mTxTail++;
    }
    KeReleaseSpinLock (&mLock, irql);
    break;
  case IOCTL_TAUDIO_GPR_RECV:
    KeAcquireSpinLock (&mLock, &irql);
    if (mRxHead != mRxTail) {
      SLOT *s = &mRx[mRxHead % RX_SLOTS];
      if (OutLen < s->Len) {
        st = STATUS_BUFFER_TOO_SMALL;
      } else {
        CopyMem (Buf, s->Data, s->Len);
        *Info = s->Len;
        mRxHead++;
      }
    }
    KeReleaseSpinLock (&mLock, irql);
    break;
  case IOCTL_TAUDIO_MMIO:
    if (InLen < sizeof (TAUDIO_MMIO) || OutLen < sizeof (TAUDIO_MMIO)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    st = DoMmio (Buf);
    *Info = NT_SUCCESS (st) ? sizeof (TAUDIO_MMIO) : 0;
    break;
  case IOCTL_TAUDIO_I2C: {
    TAUDIO_I2C *t = Buf;
    if (InLen < sizeof (*t) || OutLen < sizeof (*t) || t->WLen > 16 || t->RLen > 16) {
      return STATUS_INVALID_PARAMETER;
    }
    t->Result = (UINT8)AmpI2cXfer (t->Addr, t->W, t->WLen, t->R, t->RLen);
    *Info = sizeof (*t);
    break;
  }
  case IOCTL_TAUDIO_BUF: {
    TAUDIO_BUF *b = Buf;
    UINT32 data = 0, max;
    if (InLen < sizeof (*b) || OutLen < sizeof (*b)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    max = (b->Op == 1) ? InLen - sizeof (*b) : OutLen - sizeof (*b);
    st = DoBuf (b, (UINT8 *)(b + 1), max, &data);
    *Info = NT_SUCCESS (st) ? sizeof (*b) + data : 0;
    break;
  }
  default:
    st = STATUS_INVALID_DEVICE_REQUEST;
    break;
  }
  return st;
}
