/*
 * Wi-Fi P3: WLFW QMI client (service 0x45 on the modem), the handshake Linux ath10k/qmi.c does
 * before the WLAN data path starts:
 *   IND_REGISTER -> HOST_CAP -> MSA_INFO (+ SCM assign of the MSA regions) -> MSA_READY -> CAP,
 *   then on MSA_READY_IND: board data (bdwlan.*) download -> CAL_REPORT, then wait for FW_READY_IND.
 * Windows copy of uefi/TopazOtgDxe/Wlfw.c.
 */
#include "Modem.h"

#define Out ModemOut

#define QRTR_TYPE_DATA      1
#define QMI_REQ             0
#define QMI_RESP            2
#define QMI_IND             4

#define WLFW_IND_REGISTER   0x20
#define WLFW_FW_READY_IND   0x21
#define WLFW_CAP            0x24
#define WLFW_BDF_DOWNLOAD   0x25
#define WLFW_CAL_REPORT     0x26
#define WLFW_MSA_READY_IND  0x2B
#define WLFW_MSA_INFO       0x2D
#define WLFW_MSA_READY      0x2E
#define WLFW_HOST_CAP       0x34

#define WLFW_MSA_BASE       0x51900000u         /* DTB wlan_msa_region, inside "PIL Reserved" */
#define WLFW_MSA_SIZE       0x00100000u
#define WLFW_CLIENT_ID      0x4b4e454c          /* ath10k ATH10K_QMI_CLIENT_ID */
#define WLFW_SEG            6144                /* QMI_WLFW_MAX_DATA_SIZE_V01 */

#define VMID_MSS_MSA        0x0F
#define VMID_WLAN           0x18
#define VMID_WLAN_CE        0x19

STATIC UINT32  mNode, mPort;
STATIC UINT16  mTxn = 1;
STATIC UINT16  mPending;                        /* msg id we wait a response for, 0 = none */
STATIC BOOLEAN mMsaInd, mCapDone, mBdfStarted, mFwReady;
STATIC UINT32  mBoard = 0xFF;
STATIC UINT8  *mBdf;
STATIC UINTN   mBdfLen, mBdfOff;
STATIC UINT32  mBdfSeg;
STATIC CHAR8   mStep[48] = "not started";

typedef struct { UINT8 *B; UINT32 N; } WMSG;

STATIC BOOLEAN MsgNew(WMSG *M, UINT16 Msg)
{
  M->B = AllocateZeroPool (WLFW_SEG + 128);
  if (M->B == NULL) {
    return FALSE;
  }
  M->B[0] = QMI_REQ;
  *(UINT16 *)(M->B + 1) = mTxn++;
  *(UINT16 *)(M->B + 3) = Msg;
  M->N = 7;
  return TRUE;
}

STATIC VOID MsgTlv(WMSG *M, UINT8 Type, CONST VOID *V, UINT16 Len)
{
  M->B[M->N] = Type;
  *(UINT16 *)(M->B + M->N + 1) = Len;
  if (Len != 0) {
    CopyMem (M->B + M->N + 3, V, Len);
  }
  M->N += 3 + Len;
}

STATIC VOID MsgU8(WMSG *M, UINT8 Type, UINT8 V)
{
  MsgTlv (M, Type, &V, 1);
}

STATIC VOID MsgU32(WMSG *M, UINT8 Type, UINT32 V)
{
  MsgTlv (M, Type, &V, 4);
}

STATIC VOID MsgSend(WMSG *M)
{
  *(UINT16 *)(M->B + 5) = (UINT16)(M->N - 7);
  mPending = *(UINT16 *)(M->B + 3);
  QrtrSend (QRTR_TYPE_DATA, PORT_WLFWC, mNode, mPort, M->B, M->N);
  FreePool (M->B);
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

/* QMI result TLV 2: {u16 result, u16 error}; TRUE when result == 0 */
STATIC BOOLEAN ResultOk(CONST UINT8 *D, UINT32 Len, UINT16 *Err)
{
  UINT16 l = 0;
  CONST UINT8 *r = Tlv (D, Len, 2, &l);

  *Err = (r != NULL && l >= 4) ? *(CONST UINT16 *)(r + 2) : 0xFFFF;
  return r != NULL && l >= 4 && *(CONST UINT16 *)r == 0;
}

STATIC VOID Step(CONST CHAR8 *S)
{
  AsciiStrCpyS (mStep, sizeof (mStep), S);
}

/* ---------------- requests ---------------- */

STATIC VOID SendIndRegister(VOID)
{
  WMSG m;

  if (!MsgNew (&m, WLFW_IND_REGISTER)) {
    return;
  }
  MsgU8 (&m, 0x10, 1);                          /* fw_ready_enable */
  MsgU8 (&m, 0x13, 1);                          /* msa_ready_enable */
  MsgU32 (&m, 0x15, WLFW_CLIENT_ID);            /* client_id */
  MsgSend (&m);
  Step ("ind_register sent");
}

STATIC VOID SendHostCap(VOID)
{
  WMSG m;

  if (!MsgNew (&m, WLFW_HOST_CAP)) {
    return;
  }
  MsgU32 (&m, 0x10, 0);                         /* daemon_support = 0 */
  MsgSend (&m);
  Step ("host_cap sent");
}

STATIC VOID SendMsaInfo(VOID)
{
  WMSG m;
  UINT64 a = WLFW_MSA_BASE;

  if (!MsgNew (&m, WLFW_MSA_INFO)) {
    return;
  }
  MsgTlv (&m, 0x01, &a, 8);                     /* msa_addr */
  MsgU32 (&m, 0x02, WLFW_MSA_SIZE);             /* size */
  MsgSend (&m);
  Step ("msa_info sent");
}

STATIC VOID SendEmpty(UINT16 Msg, CONST CHAR8 *S)
{
  WMSG m;

  if (!MsgNew (&m, Msg)) {
    return;
  }
  MsgSend (&m);
  Step (S);
}

/* Board data file: bdwlan.bNN for ids < 0x100, bdwlan.NNN otherwise, bdwlan.bin as fallback. */
STATIC BOOLEAN LoadBdf(VOID)
{
  EFI_FILE_PROTOCOL *root = SvcRoot (), *f = NULL;
  CHAR16 name[40];
  EFI_FILE_INFO *info;
  UINTN isz = SIZE_OF_EFI_FILE_INFO + 256, n;

  if (root == NULL) {
    return FALSE;
  }
  if (mBoard == 0xFF) {
    UnicodeSPrint (name, sizeof (name), L"\\image\\bdwlan.bin");
  } else if (mBoard < 0x100) {
    UnicodeSPrint (name, sizeof (name), L"\\image\\bdwlan.b%02x", mBoard);
  } else {
    UnicodeSPrint (name, sizeof (name), L"\\image\\bdwlan.%03x", mBoard);
  }
  if (EFI_ERROR (root->Open (root, &f, name, EFI_FILE_MODE_READ, 0))) {
    Out ("  wlfw: %s missing, using bdwlan.bin\r\n", name);
    UnicodeSPrint (name, sizeof (name), L"\\image\\bdwlan.bin");
    if (EFI_ERROR (root->Open (root, &f, name, EFI_FILE_MODE_READ, 0))) {
      Out ("  wlfw: no board data file\r\n");
      return FALSE;
    }
  }
  info = AllocatePool (isz);
  if (info == NULL || EFI_ERROR (f->GetInfo (f, &gEfiFileInfoGuid, &isz, info))) {
    f->Close (f);
    return FALSE;
  }
  n = (UINTN)info->FileSize;
  FreePool (info);
  mBdf = AllocatePool (n);
  if (mBdf == NULL || EFI_ERROR (f->Read (f, &n, mBdf))) {
    f->Close (f);
    return FALSE;
  }
  f->Close (f);
  mBdfLen = n;
  mBdfOff = 0;
  mBdfSeg = 0;
  Out ("  wlfw: board data %s, %u bytes\r\n", name, (UINT32)n);
  return TRUE;
}

STATIC VOID SendBdfSegment(VOID)
{
  WMSG m;
  UINTN chunk = MIN (mBdfLen - mBdfOff, (UINTN)WLFW_SEG);
  UINT8 *t;
  BOOLEAN end = (mBdfOff + chunk >= mBdfLen);

  if (!MsgNew (&m, WLFW_BDF_DOWNLOAD)) {
    return;
  }
  MsgU8 (&m, 0x01, 1);                          /* valid */
  MsgU32 (&m, 0x10, 0);                         /* file_id */
  MsgU32 (&m, 0x11, (UINT32)mBdfLen);           /* total_size */
  MsgU32 (&m, 0x12, mBdfSeg);                   /* seg_id */
  /* data: u16 data_len + bytes (QMI_DATA_LEN elem_size u16) */
  t = m.B + m.N;
  t[0] = 0x13;
  *(UINT16 *)(t + 1) = (UINT16)(2 + chunk);
  *(UINT16 *)(t + 3) = (UINT16)chunk;
  CopyMem (t + 5, mBdf + mBdfOff, chunk);
  m.N += 5 + (UINT32)chunk;
  MsgU8 (&m, 0x14, end ? 1 : 0);                /* end */
  MsgSend (&m);
  mBdfOff += chunk;
  mBdfSeg++;
  Step ("bdf download");
}

STATIC VOID SendCalReport(VOID)
{
  WMSG m;
  UINT8 none = 0;

  if (!MsgNew (&m, WLFW_CAL_REPORT)) {
    return;
  }
  MsgTlv (&m, 0x01, &none, 1);                  /* meta_data_len = 0 */
  MsgSend (&m);
  Step ("cal_report sent");
}

STATIC VOID StartBdf(VOID)
{
  if (mBdfStarted || !mMsaInd || !mCapDone) {
    return;
  }
  mBdfStarted = TRUE;
  if (LoadBdf ()) {
    SendBdfSegment ();
  }
}

/* ---------------- responses / indications ---------------- */

STATIC VOID OnCap(CONST UINT8 *D, UINT32 Len)
{
  UINT16 l;
  CONST UINT8 *v;
  CHAR8 s[80];

  v = Tlv (D, Len, 0x10, &l);
  if (v != NULL && l >= 8) {
    Out ("  wlfw: chip_id %x family %x\r\n", *(CONST UINT32 *)v, *(CONST UINT32 *)(v + 4));
  }
  v = Tlv (D, Len, 0x11, &l);
  if (v != NULL && l >= 4) {
    mBoard = *(CONST UINT32 *)v;
  }
  Out ("  wlfw: board_id %x\r\n", mBoard);
  v = Tlv (D, Len, 0x12, &l);
  if (v != NULL && l >= 4) {
    Out ("  wlfw: soc_id %x\r\n", *(CONST UINT32 *)v);
  }
  v = Tlv (D, Len, 0x13, &l);
  if (v != NULL && l >= 5) {
    UINTN n = MIN ((UINTN)v[4], MIN ((UINTN)(l - 5), sizeof (s) - 1));
    CopyMem (s, v + 5, n);
    s[n] = 0;
    Out ("  wlfw: fw_version %x built %a\r\n", *(CONST UINT32 *)v, s);
  }
  v = Tlv (D, Len, 0x14, &l);
  if (v != NULL) {
    UINTN n = MIN ((UINTN)l, sizeof (s) - 1);
    CopyMem (s, v, n);
    s[n] = 0;
    Out ("  wlfw: fw_build_id %a\r\n", s);
  }
}

STATIC VOID OnMsaInfo(CONST UINT8 *D, UINT32 Len)
{
  UINT16 l;
  CONST UINT8 *v = Tlv (D, Len, 0x03, &l);
  UINT32 i, n;

  n = (v != NULL && l >= 1) ? v[0] : 0;
  Out ("  wlfw: %u MSA region(s)\r\n", n);
  gBS->Stall (20 * 1000);                       /* ath10k: give the modem time before the XPU update */
  for (i = 0; i < n && 1 + 13 * (i + 1) <= l; i++) {
    CONST UINT8 *e = v + 1 + 13 * i;            /* u64 addr, u32 size, u8 secure */
    UINT64 addr = *(CONST UINT64 *)e;
    UINT32 size = *(CONST UINT32 *)(e + 8);
    UINT8  sec  = e[12];
    UINT32 vm[3] = { VMID_MSS_MSA, VMID_WLAN, VMID_WLAN_CE };

    Out ("   region %u: %lx + %x secure %u\r\n", i, addr, size, sec);
    if (addr < WLFW_MSA_BASE || addr + size > (UINT64)WLFW_MSA_BASE + WLFW_MSA_SIZE) {
      Out ("   out of the MSA window, skipped\r\n");
      continue;
    }
    ScmAssign (addr, size, vm, sec ? 2 : 3);
  }
}

VOID WlfwArrive(UINT32 Node, UINT32 Port)
{
  if (mNode == 0 && mPort == 0) {
    mNode = Node;
    mPort = Port;
    SendIndRegister ();
  }
}

VOID WlfwRx(CONST UINT8 *D, UINT32 Len)
{
  UINT8 type;
  UINT16 msg, err = 0, l;
  BOOLEAN ok;
  CONST UINT8 *v;

  if (Len < 7) {
    return;
  }
  type = D[0];
  msg  = *(CONST UINT16 *)(D + 3);

  if (type == QMI_IND) {
    if (msg == WLFW_MSA_READY_IND) {
      Out ("  wlfw: MSA_READY indication\r\n");
      mMsaInd = TRUE;
      StartBdf ();
    } else if (msg == WLFW_FW_READY_IND) {
      mFwReady = TRUE;
      Step ("FW READY");
      Out ("  t=%u.%03u *** WLAN FIRMWARE READY ***\r\n", (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000));
    } else {
      Out ("  wlfw: indication %x\r\n", msg);
    }
    return;
  }
  if (type != QMI_RESP) {
    return;
  }
  ok = ResultOk (D, Len, &err);
  if (msg == mPending) {
    mPending = 0;
  }
  switch (msg) {
  case WLFW_IND_REGISTER:
    v = Tlv (D, Len, 0x10, &l);
    Out ("  wlfw: ind_register %a err %u, fw_status %lx\r\n", ok ? "ok" : "FAILED", err,
         (v != NULL && l >= 8) ? *(CONST UINT64 *)v : 0);
    SendHostCap ();
    break;
  case WLFW_HOST_CAP:
    Out ("  wlfw: host_cap %a err %u\r\n", ok ? "ok" : "rejected (not fatal)", err);
    SendMsaInfo ();
    break;
  case WLFW_MSA_INFO:
    Out ("  wlfw: msa_info %a err %u\r\n", ok ? "ok" : "FAILED", err);
    if (ok) {
      OnMsaInfo (D, Len);
      SendEmpty (WLFW_MSA_READY, "msa_ready sent");
    }
    break;
  case WLFW_MSA_READY:
    Out ("  wlfw: msa_ready %a err %u\r\n", ok ? "ok" : "FAILED", err);
    SendEmpty (WLFW_CAP, "cap sent");
    break;
  case WLFW_CAP:
    Out ("  wlfw: cap %a err %u\r\n", ok ? "ok" : "FAILED", err);
    if (ok) {
      OnCap (D, Len);
    }
    mCapDone = TRUE;
    Step ("waiting MSA_READY ind");
    StartBdf ();
    break;
  case WLFW_BDF_DOWNLOAD:
    if (!ok) {
      Out ("  wlfw: bdf segment %u FAILED err %u\r\n", mBdfSeg - 1, err);
      break;
    }
    if (mBdfOff < mBdfLen) {
      SendBdfSegment ();
    } else {
      Out ("  wlfw: board data sent in %u segment(s)\r\n", mBdfSeg);
      SendCalReport ();
    }
    break;
  case WLFW_CAL_REPORT:
    Out ("  wlfw: cal_report %a err %u, waiting for FW_READY\r\n", ok ? "ok" : "FAILED", err);
    Step ("waiting FW_READY");
    break;
  default:
    Out ("  wlfw: response %x %a err %u\r\n", msg, ok ? "ok" : "FAILED", err);
    break;
  }
}

VOID WlfwSummary(VOID)
{
  Out ("  WLFW: %a%a\r\n", mStep, mFwReady ? " (firmware ready)" : "");
}

CONST CHAR8 *WlfwStep(VOID)
{
  return mStep;
}
