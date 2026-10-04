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

/*
 * Only blocks that were read safely or are documented (stock DT / Linux drivers). Some LPASS
 * pages hang the bus when read without their clock (0x0A7E0100, parts of 0x0A6A9000 /
 * 0x0A7A0000 pages, 2026-10-02: two watchdog reboots), so no wide LPASS window any more.
 */
STATIC BOOLEAN MmioAllowed(UINT64 Pa, UINT32 Bytes)
{
  STATIC CONST UINT64 r[][2] = {
    { 0x0A600000, 0x0A601000 },               /* RX macro */
    { 0x0A610000, 0x0A612000 },               /* RX SoundWire master */
    { 0x0A620000, 0x0A621000 },               /* TX macro */
    { 0x0A730000, 0x0A731000 },               /* VA macro */
    { 0x0A740000, 0x0A742000 },               /* VA SoundWire master */
    { 0x0A6A9090, 0x0A6A90A0 },               /* RX SWR CGCR (HCTL 0x98) */
    { 0x0A7EC100, 0x0A7EC104 },               /* VA/TX SWR CGCR (HCTL) */
    { 0x0A5640C0, 0x0A5640E0 },               /* RX MCLK mux select (0xD8) */
    { 0x0A7A0000, 0x0A7A0010 },               /* VA MCLK mux select */
    { 0x0A7C0000, 0x0A7D3000 },               /* LPI TLMM pins 0..18 */
    { 0x0A95A000, 0x0A95A004 },               /* LPI slew */
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

/* ---------------- PMIC (read only, SPMI arbiter v5 observer) ---------------- */

#define SPMI_CORE_PA     0x01C40000u
#define SPMI_CORE_SIZE   0x1100u
#define SPMI_APID_MAP    0x900u
#define SPMI_OBS_PA      0x03E00000u            /* + 0x80 * apid (EE 0 = apps) */
#define SPMI_OBS_STRIDE  0x80u

STATIC NTSTATUS DoPmic(TAUDIO_PMIC *P)
{
  UINT8 *core, *obs;
  UINT32 ppid = ((P->Sid & 0xF) << 8) | ((P->Addr >> 8) & 0xFF), apid = MAX_UINT32, n, v, i, st;

  if (P->Count == 0 || P->Count > 32 || (P->Addr & 0xFF) + P->Count > 0x100) {
    return STATUS_INVALID_PARAMETER;
  }
  core = MapPhys (SPMI_CORE_PA, SPMI_CORE_SIZE, FALSE);
  if (core == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  for (n = 0; n < (SPMI_CORE_SIZE - SPMI_APID_MAP) / 4; n++) {
    v = MmioRead32 ((UINTN)core + SPMI_APID_MAP + 4 * n);
    if (v != 0 && ((v >> 8) & 0xFFF) == ppid) {
      apid = n;
      break;
    }
  }
  UnmapPhys (core, SPMI_CORE_SIZE);
  if (apid == MAX_UINT32) {
    return STATUS_NOT_FOUND;
  }
  obs = MapPhys (SPMI_OBS_PA + (UINT64)apid * SPMI_OBS_STRIDE, SPMI_OBS_STRIDE, FALSE);
  if (obs == NULL) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  for (n = 0; n < P->Count; n++) {
    /* observer: cmd = read (opcode 1 << 27) | reg << 4, poll status, data in RDATA0 */
    MmioWrite32 ((UINTN)obs + 0x00, (1u << 27) | (((P->Addr + n) & 0xFF) << 4));
    P->Value[n] = 0x1EE;
    for (i = 0; i < 1000; i++) {
      st = MmioRead32 ((UINTN)obs + 0x08);
      if (st & 1) {
        if ((st & 0xE) == 0) {
          P->Value[n] = (UINT16)(MmioRead32 ((UINTN)obs + 0x18) & 0xFF);
        }
        break;
      }
      KeStallExecutionProcessor (1);
    }
  }
  UnmapPhys (obs, SPMI_OBS_STRIDE);
  return STATUS_SUCCESS;
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
  case IOCTL_TAUDIO_PMIC:
    if (InLen < sizeof (TAUDIO_PMIC) || OutLen < sizeof (TAUDIO_PMIC)) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    st = DoPmic (Buf);
    *Info = NT_SUCCESS (st) ? sizeof (TAUDIO_PMIC) : 0;
    break;
  default:
    st = STATUS_INVALID_DEVICE_REQUEST;
    break;
  }
  return st;
}
