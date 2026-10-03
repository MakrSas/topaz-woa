/*
 * Host-side services the modem expects from the apps processor, after the linux-msm
 * userspace daemons (github.com/linux-msm):
 *  - pd-mapper: QMI servreg-locator 0x40 v0x101; maps "wlan/fw", "kernel/elf_loader", ...
 *    to msm/modem/{root_pd,wlan_pd} (modemr.jsn / modemuw.jsn, instance 180).
 *  - tqftpserv: TFTP over QRTR service 4096; the modem pulls /readonly/firmware/image/...
 *    (wlanmdsp.mbn) in seek/rsize chunks; served read-only from modem_a \image\.
 *    Writes (/readwrite/...) are acknowledged and dropped.
 *  - rmtfs: QMI service 14; modem EFS from modemst1/modemst2/fsc/fsg partitions into a
 *    shared buffer assigned to the modem through TrustZone. Writes are NOT persisted here.
 * Windows copy of uefi/TopazOtgDxe/ModemSvc.c: EFS comes from the partition backups in
 * C:\topaz\fw\efs\*.bin instead of BlockIo, the rmtfs buffer from PhysAlloc().
 */
#include "Modem.h"

#define Out ModemOut

#define QRTR_PORT_CTRL       0xFFFFFFFEu
#define QRTR_TYPE_DATA       1
#define QRTR_TYPE_NEW_SERVER 4

#define PORT_PDM     0x4001
#define PORT_TFTP    0x4002
#define PORT_RMTFS   0x4003
#define PORT_SESS    0x4100

#define QMI_REQ      0
#define QMI_RESP     2

/* ---------------- QMI helpers ---------------- */

typedef struct { UINT8 B[512]; UINT32 N; } QMSG;

STATIC VOID QmInit(QMSG *M, UINT16 Txn, UINT16 Msg)
{
  M->B[0] = QMI_RESP;
  *(UINT16 *)(M->B + 1) = Txn;
  *(UINT16 *)(M->B + 3) = Msg;
  M->N = 7;
}

STATIC VOID QmTlv(QMSG *M, UINT8 Type, CONST VOID *V, UINT16 Len)
{
  M->B[M->N] = Type;
  *(UINT16 *)(M->B + M->N + 1) = Len;
  CopyMem (M->B + M->N + 3, V, Len);
  M->N += 3 + Len;
}

STATIC VOID QmResult(QMSG *M, BOOLEAN Ok)
{
  UINT16 r[2] = { Ok ? 0 : 1, Ok ? 0 : 1 };
  QmTlv (M, 2, r, 4);
}

STATIC VOID QmSend(QMSG *M, UINT32 SrcPort, UINT32 Node, UINT32 Port)
{
  *(UINT16 *)(M->B + 5) = (UINT16)(M->N - 7);
  QrtrSend (QRTR_TYPE_DATA, SrcPort, Node, Port, M->B, M->N);
}

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

STATIC VOID QmString(CONST UINT8 *D, UINT32 Len, UINT8 Type, CHAR8 *Out, UINTN Max)
{
  UINT16 l = 0;
  CONST UINT8 *v = QmFind (D, Len, Type, &l);

  Out[0] = 0;
  if (v != NULL) {
    l = (UINT16)MIN ((UINTN)l, Max - 1);
    CopyMem (Out, v, l);
    Out[l] = 0;
  }
}

/* ---------------- pd-mapper ---------------- */

STATIC CONST struct { CONST CHAR8 *Svc, *Domain; } mPdMap[] = {
  { "tms/servreg",       "msm/modem/root_pd" },
  { "tms/pdr_enabled",   "msm/modem/root_pd" },
  { "gps/gps_service",   "msm/modem/root_pd" },
  { "kernel/elf_loader", "msm/modem/wlan_pd" },
  { "tms/servreg",       "msm/modem/wlan_pd" },
  { "wlan/fw",           "msm/modem/wlan_pd" },
};
STATIC UINTN mPdReqs;

STATIC VOID PdMapperRx(UINT32 Node, UINT32 Port, UINT16 Txn, UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  QMSG m;
  CHAR8 name[80];
  UINT8 list[256];
  UINT32 n = 1, i, cnt = 0;
  UINT16 v;

  QmInit (&m, Txn, Msg);
  if (Msg != 0x21) {                            /* PFR etc.: just say OK */
    Out ("  pd-mapper: msg %x from %u:%x\r\n", Msg, Node, Port);
    QmResult (&m, TRUE);
    QmSend (&m, PORT_PDM, Node, Port);
    return;
  }
  QmString (D, Len, 1, name, sizeof (name));
  for (i = 0; i < ARRAY_SIZE (mPdMap); i++) {
    if (AsciiStrCmp (mPdMap[i].Svc, name) == 0) {
      UINT32 dl = (UINT32)AsciiStrLen (mPdMap[i].Domain), inst = 180, zero = 0;
      list[n++] = (UINT8)dl;
      CopyMem (list + n, mPdMap[i].Domain, dl);
      n += dl;
      CopyMem (list + n, &inst, 4);
      n += 4;
      list[n++] = 0;                            /* service_data_valid */
      CopyMem (list + n, &zero, 4);
      n += 4;
      cnt++;
    }
  }
  list[0] = (UINT8)cnt;
  QmResult (&m, TRUE);
  v = (UINT16)cnt;
  QmTlv (&m, 0x10, &v, 2);
  v = 1;
  QmTlv (&m, 0x11, &v, 2);
  if (cnt != 0) {
    QmTlv (&m, 0x12, list, (UINT16)n);
  }
  QmSend (&m, PORT_PDM, Node, Port);
  mPdReqs++;
  Out ("  pd-mapper: \"%a\" -> %u domain(s)\r\n", name, cnt);
}

/* ---------------- tftp ---------------- */

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5
#define OP_OACK  6

typedef struct {
  BOOLEAN            Used, Write;
  UINT32             Port, Node, RPort;
  EFI_FILE_PROTOCOL *F;
  UINT64             Size;
  UINTN              Blk, Rsize, Wsize, Seek, Fi;
  UINT64             Sent;
  UINT32             LastAck, Acks;
  INTN               Rw;                    /* /readwrite file in RAM (mRw index), -1 = FAT file */
} TFTP_SESS;

/*
 * /readwrite/... files live in RAM for the life of the driver (tqftpserv keeps them on disk).
 * The modem writes and reads back its scratch files, e.g. mcfg.tmp while modem_cfg picks the
 * carrier config after a SIM appears; dropping the writes starved modem_cfg until the modem
 * watchdog fired ("dog_hb.c Task starvation: modem_cfg", TopazWifi v0.7).
 */
typedef struct { CHAR8 Path[96]; UINT8 *Data; UINTN Size, Cap; } RW_FILE;
STATIC RW_FILE mRw[12];

STATIC INTN RwFind(CONST CHAR8 *Path, BOOLEAN Create)
{
  UINTN i;

  for (i = 0; i < ARRAY_SIZE (mRw); i++) {
    if (mRw[i].Path[0] != 0 && AsciiStrnCmp (mRw[i].Path, Path, sizeof (mRw[i].Path) - 1) == 0) {
      return (INTN)i;
    }
  }
  if (!Create) {
    return -1;
  }
  for (i = 0; i < ARRAY_SIZE (mRw); i++) {
    if (mRw[i].Path[0] == 0) {
      AsciiStrnCpyS (mRw[i].Path, sizeof (mRw[i].Path), Path, sizeof (mRw[i].Path) - 1);
      return (INTN)i;
    }
  }
  return -1;
}

STATIC BOOLEAN RwWrite(INTN Rw, UINT64 Off, CONST UINT8 *Src, UINTN N)
{
  RW_FILE *f = &mRw[Rw];
  UINTN end = (UINTN)Off + N;

  if (Off > 0x100000 || N > 0x100000) {
    return FALSE;
  }
  if (end > f->Cap) {
    UINTN cap = MAX (end, f->Cap * 2);
    UINT8 *d = AllocatePool (cap);
    if (d == NULL) {
      return FALSE;
    }
    ZeroMem (d, cap);
    if (f->Data != NULL) {
      CopyMem (d, f->Data, f->Size);
      FreePool (f->Data);
    }
    f->Data = d;
    f->Cap  = cap;
  }
  CopyMem (f->Data + Off, Src, N);
  f->Size = MAX (f->Size, end);
  return TRUE;
}

STATIC EFI_FILE_PROTOCOL *mRoot;
STATIC UINT32            mCrc;
STATIC TFTP_SESS mSess[16];
STATIC UINT32    mNextSessPort = PORT_SESS;
STATIC struct { CHAR8 Name[48]; UINT32 Reqs; UINT64 Bytes; BOOLEAN Missing; } mFiles[16];
STATIC UINTN     mFileCount, mTftpWrites;

STATIC UINTN FileStat(CONST CHAR8 *Name, BOOLEAN Missing)
{
  UINTN i;

  for (i = 0; i < mFileCount; i++) {
    if (AsciiStrnCmp (mFiles[i].Name, Name, sizeof (mFiles[i].Name) - 1) == 0) {
      return i;
    }
  }
  if (mFileCount == ARRAY_SIZE (mFiles)) {
    return mFileCount - 1;
  }
  AsciiStrnCpyS (mFiles[mFileCount].Name, sizeof (mFiles[0].Name), Name, sizeof (mFiles[0].Name) - 1);
  mFiles[mFileCount].Missing = Missing;
  return mFileCount++;
}

STATIC VOID TftpSend(TFTP_SESS *S, CONST VOID *P, UINT32 Len)
{
  QrtrSend (QRTR_TYPE_DATA, S->Port, S->Node, S->RPort, P, Len);
}

STATIC VOID TftpError(TFTP_SESS *S, UINT16 Code, CONST CHAR8 *Msg)
{
  UINT8 b[64];
  UINT32 l = (UINT32)AsciiStrLen (Msg) + 1;

  b[0] = 0; b[1] = OP_ERROR; b[2] = (UINT8)(Code >> 8); b[3] = (UINT8)Code;
  CopyMem (b + 4, Msg, l);
  TftpSend (S, b, 4 + l);
}

STATIC VOID TftpClose(TFTP_SESS *S)
{
  if (!S->Write && S->Sent != 0) {
    Out ("    sess %x done: %lu B\r\n", S->Port, S->Sent);
  }
  if (S->F != NULL) {
    S->F->Close (S->F);
  }
  ZeroMem (S, sizeof (*S));
}

/* tqftpserv tftp_send_data() */
STATIC BOOLEAN TftpData(TFTP_SESS *S, UINT32 Block, UINT64 Offset, UINTN RespSize, UINTN FileIdx)
{
  UINT8 *b = AllocatePool (S->Blk + 4);
  UINTN n = S->Blk;
  UINT32 send;

  if (b == NULL) {
    return FALSE;
  }
  b[0] = 0; b[1] = OP_DATA; b[2] = (UINT8)(Block >> 8); b[3] = (UINT8)Block;
  if (Offset >= S->Size) {
    n = 0;
  } else if (S->Rw >= 0) {
    n = (UINTN)MIN ((UINT64)n, S->Size - Offset);
    CopyMem (b + 4, mRw[S->Rw].Data + Offset, n);
  } else if (EFI_ERROR (S->F->SetPosition (S->F, Offset)) || EFI_ERROR (S->F->Read (S->F, &n, b + 4))) {
    Out ("    sess %x: file read FAILED at %lu\r\n", S->Port, Offset);
    FreePool (b);
    TftpError (S, 0, "Read error");
    return FALSE;
  }
  send = (UINT32)(4 + n);
  if (RespSize != 0) {
    if (4 + RespSize > send) {
      Out ("    sess %x: short read %u < %u\r\n", S->Port, send - 4, (UINT32)RespSize);
      FreePool (b);
      return FALSE;
    }
    send = (UINT32)(4 + RespSize);
  }
  TftpSend (S, b, send);
  mFiles[FileIdx].Bytes += send - 4;
  S->Sent += send - 4;
  FreePool (b);
  return TRUE;
}

STATIC CHAR8 *Opt(CHAR8 *P, CHAR8 *End, CONST CHAR8 *Name, CONST CHAR8 *Val)
{
  UINTN a = AsciiStrLen (Name) + 1, b = AsciiStrLen (Val) + 1;

  if (P + a + b > End) {
    return P;
  }
  CopyMem (P, Name, a);
  CopyMem (P + a, Val, b);
  return P + a + b;
}

/* Map the modem's path to the NON-HLOS FAT: /readonly/firmware/image/x -> \image\x */
STATIC BOOLEAN MapPath(CONST CHAR8 *Path, CHAR16 *Out, UINTN Max)
{
  STATIC CONST CHAR8 *pre[] = {
    "/readonly/firmware/image/", "/readonly/vendor/firmware_mnt/image/", "/readonly/vendor/firmware/",
  };
  UINTN i, k = 0;
  CONST CHAR8 *rest = NULL;

  for (i = 0; i < ARRAY_SIZE (pre); i++) {
    if (AsciiStrnCmp (Path, pre[i], AsciiStrLen (pre[i])) == 0) {
      rest = Path + AsciiStrLen (pre[i]);
      UnicodeSPrint (Out, Max * sizeof (CHAR16), L"\\image\\");
      break;
    }
  }
  if (rest == NULL && AsciiStrnCmp (Path, "/readonly/firmware/", 19) == 0) {
    rest = Path + 19;
    UnicodeSPrint (Out, Max * sizeof (CHAR16), L"\\");
  }
  if (rest == NULL) {
    return FALSE;
  }
  k = StrLen (Out);
  for (; *rest != 0 && k + 1 < Max; rest++) {
    Out[k++] = (*rest == '/') ? L'\\' : (CHAR16)*rest;
  }
  Out[k] = 0;
  return TRUE;
}

STATIC VOID TftpRequest(UINT32 Node, UINT32 Port, CONST UINT8 *D, UINT32 Len)
{
  CHAR8 buf[512], *p, *end, *file, *mode, oack[256], *o, num[24];
  UINT16 op = (UINT16)(D[0] << 8 | D[1]);
  TFTP_SESS *s = NULL;
  BOOLEAN opts, wantTsize = FALSE;
  UINTN i, fi = 0, timeoutms = 1000;
  CHAR16 path[128];
  EFI_FILE_INFO *info;
  UINTN isz;

  if (Len < 4 || Len >= sizeof (buf)) {
    return;
  }
  CopyMem (buf, D, Len);
  buf[Len] = 0;
  end = buf + Len;
  file = buf + 2;
  mode = file + AsciiStrLen (file) + 1;
  if (mode >= end) {
    return;
  }
  p = mode + AsciiStrLen (mode) + 1;
  opts = p < end;

  for (i = 0; i < ARRAY_SIZE (mSess); i++) {
    if (!mSess[i].Used) {
      s = &mSess[i];
      break;
    }
  }
  if (s == NULL) {
    Out ("  tftp: out of sessions\r\n");
    return;
  }
  ZeroMem (s, sizeof (*s));
  s->Used  = TRUE;
  s->Write = (op == OP_WRQ);
  s->Port  = mNextSessPort++;
  s->Node  = Node;
  s->RPort = Port;
  s->Blk   = 512;
  s->Wsize = 1;
  s->Rw    = -1;
  while (p < end) {
    CHAR8 *name = p, *val = p + AsciiStrLen (p) + 1;
    UINTN v;
    if (val >= end) {
      break;
    }
    v = AsciiStrDecimalToUintn (val);
    if (AsciiStriCmp (name, "blksize") == 0) {
      s->Blk = MAX (8, MIN (v, 16384));
    } else if (AsciiStriCmp (name, "timeoutms") == 0) {
      timeoutms = v;
    } else if (AsciiStriCmp (name, "tsize") == 0) {
      wantTsize = TRUE;
    } else if (AsciiStriCmp (name, "rsize") == 0) {
      s->Rsize = v;
    } else if (AsciiStriCmp (name, "wsize") == 0) {
      s->Wsize = MAX (1, v);
    } else if (AsciiStriCmp (name, "seek") == 0) {
      s->Seek = v;
    }
    p = val + AsciiStrLen (val) + 1;
  }

  {
    CONST CHAR8 *b = file, *q;
    for (q = file; *q != 0; q++) {
      if (*q == '/') {
        b = q + 1;
      }
    }
    Out ("  t=%u.%03u tftp %a %a seek %lu rsize %lu blk %lu w %lu%a\r\n", (UINT32)(ModemMs () / 1000),
         (UINT32)(ModemMs () % 1000), op == OP_WRQ ? "WRQ" : "RRQ", b, (UINT64)s->Seek, (UINT64)s->Rsize,
         (UINT64)s->Blk, (UINT64)s->Wsize, wantTsize ? " tsize" : "");
  }
  if (AsciiStrnCmp (file, "/readwrite/", 11) == 0) {
    s->Rw = RwFind (file, s->Write);
  }
  if (s->Write) {
    mTftpWrites++;
    if (s->Rw < 0) {
      Out ("    -> no RAM slot, dropped\r\n");
    } else if (s->Seek == 0) {
      mRw[s->Rw].Size = 0;                      /* a write from the start replaces the file */
    }
  } else if (s->Rw >= 0) {
    s->Size = mRw[s->Rw].Size;
    fi = FileStat (file, FALSE);
    mFiles[fi].Reqs++;
    s->Fi = fi;
    Out ("    -> RAM file, %lu B\r\n", s->Size);
  } else {
    if (!MapPath (file, path, ARRAY_SIZE (path)) ||
        EFI_ERROR (mRoot->Open (mRoot, &s->F, path, EFI_FILE_MODE_READ, 0))) {
      s->F = NULL;
      fi = FileStat (file, TRUE);
      mFiles[fi].Reqs++;
      Out ("    -> not found\r\n");
      TftpError (s, 1, "file not found");
      TftpClose (s);
      return;
    }
    isz = SIZE_OF_EFI_FILE_INFO + 256;
    info = AllocatePool (isz);
    if (info != NULL && !EFI_ERROR (s->F->GetInfo (s->F, &gEfiFileInfoGuid, &isz, info))) {
      s->Size = info->FileSize;
    }
    if (info != NULL) {
      FreePool (info);
    }
    fi = FileStat (file, FALSE);
    mFiles[fi].Reqs++;
    s->Fi = fi;
  }

  if (!opts) {
    if (s->Write) {
      UINT8 ack[4] = { 0, OP_ACK, 0, 0 };
      TftpSend (s, ack, 4);
    } else {
      TftpData (s, 1, 0, 0, fi);
    }
    return;
  }
  o = oack;
  *o++ = 0;
  *o++ = OP_OACK;
  AsciiSPrint (num, sizeof (num), "%lu", (UINT64)s->Blk);
  o = Opt (o, oack + sizeof (oack), "blksize", num);
  AsciiSPrint (num, sizeof (num), "%lu", (UINT64)timeoutms);
  o = Opt (o, oack + sizeof (oack), "timeoutms", num);
  if (wantTsize && !s->Write) {
    Out ("    -> tsize %lu\r\n", s->Size);
    AsciiSPrint (num, sizeof (num), "%lu", s->Size);
    o = Opt (o, oack + sizeof (oack), "tsize", num);
  }
  AsciiSPrint (num, sizeof (num), "%lu", (UINT64)s->Wsize);
  o = Opt (o, oack + sizeof (oack), "wsize", num);
  if (s->Rsize != 0) {
    AsciiSPrint (num, sizeof (num), "%lu", (UINT64)s->Rsize);
    o = Opt (o, oack + sizeof (oack), "rsize", num);
  }
  if (s->Seek != 0) {
    AsciiSPrint (num, sizeof (num), "%lu", (UINT64)s->Seek);
    o = Opt (o, oack + sizeof (oack), "seek", num);
  }
  TftpSend (s, oack, (UINT32)(o - oack));
}

/* Traffic on a session port: ACKs for reads (tqftpserv handle_reader), DATA for writes */
STATIC BOOLEAN TftpSession(UINT32 Node, UINT32 Port, UINT32 DstPort, CONST UINT8 *D, UINT32 Len)
{
  TFTP_SESS *s = NULL;
  UINT16 op, blk;
  UINTN i, fi = 0;

  for (i = 0; i < ARRAY_SIZE (mSess); i++) {
    if (mSess[i].Used && mSess[i].Port == DstPort) {
      s = &mSess[i];
      break;
    }
  }
  if (s == NULL || Len < 4) {
    return s != NULL;
  }
  op  = (UINT16)(D[0] << 8 | D[1]);
  blk = (UINT16)(D[2] << 8 | D[3]);
  if (op == OP_ERROR) {
    CHAR8 msg[40];
    AsciiStrnCpyS (msg, sizeof (msg), (CONST CHAR8 *)D + 4, MIN (Len - 4, sizeof (msg) - 1));
    Out ("    t=%u.%03u sess %x: modem ERROR %u \"%a\" after %lu B\r\n", (UINT32)(ModemMs () / 1000),
         (UINT32)(ModemMs () % 1000), s->Port, blk, msg, s->Sent);
    TftpClose (s);
    return TRUE;
  }
  if (s->Write) {
    if (op == OP_DATA) {
      UINTN payload = Len - 4;
      if (s->Rw >= 0 && blk >= 1 && !RwWrite (s->Rw, s->Seek + (UINT64)(blk - 1) * s->Blk, D + 4, payload)) {
        Out ("    sess %x: RAM write failed at block %u\r\n", s->Port, blk);
      }
      if ((blk % s->Wsize) == 0 || payload < s->Blk) {
        UINT8 ack[4] = { 0, OP_ACK, (UINT8)(blk >> 8), (UINT8)blk };
        TftpSend (s, ack, 4);
      }
      if (payload < s->Blk) {
        if (s->Rw >= 0) {
          Out ("    sess %x: wrote %a, now %lu B\r\n", s->Port, mRw[s->Rw].Path, (UINT64)mRw[s->Rw].Size);
        }
        TftpClose (s);
      }
    }
    return TRUE;
  }
  if (op == OP_ACK) {
    s->LastAck = blk;
    s->Acks++;
    if (blk == 90 && gTrace == 0 && FALSE) {
      gTrace = 1;                               /* the stall comes after ack 90: trace from ack 80 */
      Out ("  ---- trace from sess %x ack 90 ----\r\n", s->Port);
    }
    if (s->Rsize > 1000000 && (s->Acks == 1 || (blk % 50) == 0)) {
      Out ("    t=%u.%03u sess %x ack %u (%lu B sent)\r\n", (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000),
           s->Port, blk, s->Sent);
    }
  }
  if (op != OP_ACK) {
    TftpError (s, 4, "Expected ACK opcode");
    TftpClose (s);
    return TRUE;
  }
  fi = s->Fi;
  if (s->Rsize == 0) {
    /*
     * rsize 0 = the whole file (seen for wlanmdsp.mbn: "seek 0 rsize 0 blk 7680 w 10 tsize").
     * Plain TFTP: blocks until a short one (empty if the size is a multiple of blksize).
     */
    UINT64 total = (s->Size > s->Seek) ? s->Size - s->Seek : 0;
    UINTN  lastNo = (UINTN)(total / s->Blk) + 1;
    if ((UINTN)blk >= lastNo) {
      TftpClose (s);
      return TRUE;
    }
    for (i = (UINTN)blk + 1; i <= (UINTN)blk + s->Wsize && i <= lastNo; i++) {
      if (!TftpData (s, (UINT32)i, s->Seek + (UINT64)(i - 1) * s->Blk, 0, fi)) {
        break;
      }
    }
    return TRUE;
  }
  if ((UINTN)blk * s->Blk > s->Rsize) {
    TftpClose (s);
    return TRUE;
  }
  for (i = blk; i < (UINTN)blk + s->Wsize; i++) {
    UINT64 off = s->Seek + (UINT64)i * s->Blk;
    UINTN rs = 0;
    if ((i + 1) * s->Blk > s->Rsize) {
      rs = s->Rsize % s->Blk;
    }
    if (!TftpData (s, (UINT32)(i + 1), off, rs, fi)) {
      break;
    }
    if ((i + 1) * s->Blk > s->Rsize) {
      break;
    }
  }
  return TRUE;
}

/* ---------------- rmtfs ---------------- */

#define RMTFS_BUF_SIZE 0x300000                 /* DTB qcom,rmtfs_sharedmem reg size */

STATIC CONST struct { CONST CHAR8 *Path; CONST CHAR16 *Part; } mRmtfsParts[] = {
  { "/boot/modem_fs1", L"\\efs\\modemst1.bin" },
  { "/boot/modem_fs2", L"\\efs\\modemst2.bin" },
  { "/boot/modem_fsc", L"\\efs\\fsc.bin" },
  { "/boot/modem_fsg", L"\\efs\\fsg.bin" },
};
STATIC EFI_FILE_PROTOCOL     *mRmtfsBio[ARRAY_SIZE (mRmtfsParts)];
STATIC EFI_PHYSICAL_ADDRESS   mRmtfsBuf;
STATIC UINT8                 *mRmtfsVa;
STATIC UINTN                  mRmtfsReads, mRmtfsWrites, mRmtfsErr;

/* "Partition" = the dd backup of it as a file under C:\topaz\fw */
STATIC EFI_FILE_PROTOCOL *FindPartition(CONST CHAR16 *Name)
{
  EFI_FILE_PROTOCOL *f = NULL;

  if (mRoot == NULL || EFI_ERROR (mRoot->Open (mRoot, &f, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0))) {
    return NULL;
  }
  return f;
}

/* Read Bytes at byte offset Off of a partition file into Dst (a VA inside the rmtfs buffer). */
STATIC BOOLEAN PartRead(EFI_FILE_PROTOCOL *F, UINT64 Off, UINT8 *Dst, UINTN Bytes)
{
  UINTN n = Bytes;

  if (Dst == NULL || EFI_ERROR (F->SetPosition (F, Off)) || EFI_ERROR (F->Read (F, &n, Dst))) {
    return FALSE;
  }
  if (n < Bytes) {
    ZeroMem (Dst + n, Bytes - n);               /* past the end of the backup: zeros */
  }
  return TRUE;
}

STATIC VOID RmtfsRx(UINT32 Node, UINT32 Port, UINT16 Txn, UINT16 Msg, CONST UINT8 *D, UINT32 Len)
{
  QMSG m;
  CHAR8 path[64];
  UINT16 l;
  CONST UINT8 *v;
  UINT32 caller = 0, i;
  BOOLEAN ok = TRUE;

  QmInit (&m, Txn, Msg);
  v = QmFind (D, Len, 1, &l);
  if (Msg != 1 && v != NULL && l >= 4) {
    caller = *(CONST UINT32 *)v;
  }
  switch (Msg) {
  case 1:                                       /* OPEN */
    QmString (D, Len, 1, path, sizeof (path));
    for (i = 0; i < ARRAY_SIZE (mRmtfsParts); i++) {
      if (AsciiStrCmp (path, mRmtfsParts[i].Path) == 0) {
        break;
      }
    }
    if (i < ARRAY_SIZE (mRmtfsParts) && mRmtfsBio[i] == NULL) {
      mRmtfsBio[i] = FindPartition (mRmtfsParts[i].Part);
    }
    ok = i < ARRAY_SIZE (mRmtfsParts) && mRmtfsBio[i] != NULL;
    Out ("  rmtfs: open %a -> %a\r\n", path, ok ? "ok" : "FAILED");
    QmResult (&m, ok);
    if (ok) {
      caller = i + 1;
      QmTlv (&m, 0x10, &caller, 4);
    }
    break;
  case 2:                                       /* CLOSE */
    QmResult (&m, TRUE);
    break;
  case 3: {                                     /* RW_IOVEC */
    CONST UINT8 *dir = QmFind (D, Len, 2, &l);
    CONST UINT8 *iov = QmFind (D, Len, 3, &l);
    BOOLEAN write = dir != NULL && *dir != 0;
    EFI_FILE_PROTOCOL *bio = (caller >= 1 && caller <= ARRAY_SIZE (mRmtfsParts)) ? mRmtfsBio[caller - 1] : NULL;
    ok = bio != NULL && iov != NULL && mRmtfsBuf != 0;
    for (i = 0; ok && i < iov[0]; i++) {
      CONST UINT32 *e = (CONST UINT32 *)(iov + 1 + 12 * i);   /* sector, phys, count */
      UINT64 bytes = (UINT64)e[2] * 512;
      if (e[1] < mRmtfsBuf || e[1] + bytes > mRmtfsBuf + RMTFS_BUF_SIZE) {
        ok = FALSE;
        break;
      }
      if (write) {
        mRmtfsWrites++;                         /* not persisted in this spike */
        Out ("  t=%u.%03u rmtfs: write caller %u sector %u, %u sectors (not persisted)\r\n",
             (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000), caller, e[0], e[2]);
      } else {
        ok = PartRead (bio, (UINT64)e[0] * 512, mRmtfsVa + (e[1] - mRmtfsBuf), (UINTN)bytes);
        mRmtfsReads++;
      }
    }
    if (!ok) {
      mRmtfsErr++;
      if (mRmtfsErr <= 3) {
        Out ("  rmtfs: iovec caller %u %a FAILED\r\n", caller, write ? "write" : "read");
      }
    }
    QmResult (&m, ok);
    break;
  }
  case 4: {                                     /* ALLOC_BUFF */
    UINT64 a;
    if (mRmtfsBuf == 0) {
      mRmtfsVa = PhysAlloc (RMTFS_BUF_SIZE, &mRmtfsBuf);   /* zeroed; never freed: the modem owns it */
      if (mRmtfsVa == NULL) {
        mRmtfsBuf = 0;
      } else {
        ScmAssignToModem (mRmtfsBuf, RMTFS_BUF_SIZE);
      }
    }
    v = QmFind (D, Len, 2, &l);
    Out ("  rmtfs: alloc %x for caller %u -> %lx\r\n", v != NULL ? *(CONST UINT32 *)v : 0, caller, mRmtfsBuf);
    ok = mRmtfsBuf != 0;
    QmResult (&m, ok);
    if (ok) {
      a = mRmtfsBuf;
      QmTlv (&m, 0x10, &a, 8);
    }
    break;
  }
  case 5: {                                     /* GET_DEV_ERROR */
    UINT8 st = 0;
    QmResult (&m, TRUE);
    QmTlv (&m, 0x10, &st, 1);
    break;
  }
  default:
    Out ("  rmtfs: msg %x\r\n", Msg);
    QmResult (&m, FALSE);
    break;
  }
  QmSend (&m, PORT_RMTFS, Node, Port);
}

/* ---------------- glue ---------------- */

/* CRC32 of a firmware file as UEFI's FAT driver returns it (compare with the copy on the PC) */
STATIC VOID FileCrc(CONST CHAR16 *Path)
{
  EFI_FILE_PROTOCOL *f;
  UINT8 *buf;
  UINTN n, total = 0;
  UINT32 crc = 0;

  if (EFI_ERROR (mRoot->Open (mRoot, &f, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0))) {
    return;
  }
  buf = AllocatePool (SIZE_4MB);
  if (buf != NULL) {
    n = SIZE_4MB;
    if (!EFI_ERROR (f->Read (f, &n, buf))) {
      total = n;
      gBS->CalculateCrc32 (buf, n, &crc);
    }
    FreePool (buf);
  }
  f->Close (f);
  Out ("  %s: %lu bytes, crc32 %08x\r\n", Path, (UINT64)total, crc);
  mCrc = crc;
}

EFI_FILE_PROTOCOL *SvcRoot(VOID)
{
  return mRoot;
}

VOID SvcInit(EFI_FILE_PROTOCOL *Root)
{
  mRoot = Root;
  FileCrc (L"\\image\\wlanmdsp.mbn");
}

VOID SvcAnnounce(VOID)
{
  STATIC CONST UINT32 svc[][3] = {
    { 0x40, 0x101, PORT_PDM },                  /* servreg-locator, version 0x101 instance 0 */
    { 4096, 1,     PORT_TFTP },                 /* tftp, version 1 instance 0 */
    { 14,   1,     PORT_RMTFS },                /* rmtfs, version 1 instance 0 */
  };
  UINT32 c[5], i;

  for (i = 0; i < ARRAY_SIZE (svc); i++) {
    c[0] = QRTR_TYPE_NEW_SERVER;
    c[1] = svc[i][0];
    c[2] = svc[i][1];                           /* (instance << 8) | version */
    c[3] = 1;                                   /* our node */
    c[4] = svc[i][2];
    QrtrSend (QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, QrtrModemNode (), QRTR_PORT_CTRL, c, sizeof (c));
  }
  Out ("  announced pd-mapper 0x40, tftp 0x1000, rmtfs 0x0E\r\n");
}

BOOLEAN SvcRx(UINT32 SrcNode, UINT32 SrcPort, UINT32 DstPort, CONST UINT8 *Data, UINT32 Len)
{
  UINT16 txn, msg;

  if (DstPort == PORT_TFTP) {
    if (Len >= 2 && (Data[1] == OP_RRQ || Data[1] == OP_WRQ)) {
      TftpRequest (SrcNode, SrcPort, Data, Len);
    }
    return TRUE;
  }
  if (DstPort == PORT_WLFWC) {
    WlfwRx (Data, Len);
    return TRUE;
  }
  if (WwanRx (DstPort, Data, Len)) {
    return TRUE;
  }
  if (DstPort >= PORT_SESS && DstPort < PORT_SESS + 0x10000) {
    return TftpSession (SrcNode, SrcPort, DstPort, Data, Len);
  }
  if (Len < 7 || Data[0] != QMI_REQ) {
    return FALSE;
  }
  txn = *(CONST UINT16 *)(Data + 1);
  msg = *(CONST UINT16 *)(Data + 3);
  if (DstPort == PORT_PDM) {
    PdMapperRx (SrcNode, SrcPort, txn, msg, Data, Len);
    return TRUE;
  }
  if (DstPort == PORT_RMTFS) {
    RmtfsRx (SrcNode, SrcPort, txn, msg, Data, Len);
    return TRUE;
  }
  return FALSE;
}

VOID SvcSummary(VOID)
{
  UINTN i;

  for (i = 0; i < ARRAY_SIZE (mSess); i++) {
    if (mSess[i].Used) {
      Out ("  open sess %x %a: last ack %u, acks %u, sent %lu B, rsize %lu\r\n", mSess[i].Port,
           mSess[i].Write ? "W" : "R", mSess[i].LastAck, mSess[i].Acks, mSess[i].Sent, (UINT64)mSess[i].Rsize);
    }
  }

  WlfwSummary ();
  Out ("  pd-mapper %u reqs; rmtfs reads %u writes %u (dropped) errors %u; tftp writes %u\r\n",
       (UINT32)mPdReqs, (UINT32)mRmtfsReads, (UINT32)mRmtfsWrites, (UINT32)mRmtfsErr, (UINT32)mTftpWrites);
  for (i = 0; i < mFileCount; i++) {
    Out ("   tftp %a: %u reqs, %lu bytes%a\r\n", mFiles[i].Name, mFiles[i].Reqs, mFiles[i].Bytes,
         mFiles[i].Missing ? " (missing)" : "");
  }
}
