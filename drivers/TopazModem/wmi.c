/*
 * P4 WMI (ath10k wmi-tlv.c; this firmware speaks TLV WMI, ABI 1.0 "QCA_ML"). The firmware sends
 * SERVICE_AVAILABLE, SERVICE_READY, SERVICE_READY_EXT(2) after HTC setup; we answer SERVICE_READY
 * with INIT (abi + resource config like ath10k for wcn3990 + host memory chunks) and wait for READY.
 * Every message: wmi_cmd_hdr (u32, id in [23:0]) + TLVs {u16 len, u16 tag, value}.
 */
#include "Modem.h"

#define Out ModemOut
#define Step WlfwSetStep
#define T   (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000)

#define WMI_EV_SERVICE_READY      0x1
#define WMI_EV_READY              0x2
#define WMI_EV_SERVICE_AVAILABLE  0x3
#define WMI_EV_SERVICE_READY_EXT  0x4009          /* qcacld numbering, unused by ath10k */
#define WMI_EV_SERVICE_READY_EXT2 0x4022
#define WMI_EV_SCAN               0x3001
#define WMI_EV_MGMT_RX            0x7001
#define WMI_EV_CHAN_INFO          0x4002
#define WMI_CMD_INIT              0x1

#define TAG_ARRAY_UINT32          0x10
#define TAG_ARRAY_STRUCT          0x12
#define TAG_SERVICE_READY         0x20
#define TAG_HAL_REG_CAPS          0x21
#define TAG_HOST_MEM_REQ          0x22
#define TAG_READY_EVENT           0x23
#define TAG_INIT_CMD              0x4A
#define TAG_RESOURCE_CONFIG       0x4B
#define TAG_HOST_MEM_CHUNK        0x4C
#define TAG_SERVICE_AVAILABLE     0x22F

#define ABI_VER0                  0x01000000u      /* major 1, minor 0 */
#define ABI_VER1                  53
#define ABI_NS0                   0x5F414351u      /* "QCA_" */
#define ABI_NS1                   0x00004C4Du      /* "ML" */

#define SVC_RX_FULL_REORDER       65               /* WMI_TLV_SERVICE_* bit numbers */
#define SVC_EXTEND_ADDRESS        179

#define NUM_PEERS                 33               /* TARGET_HL_TLV_NUM_PEERS (wcn3990 hw_params) */
#define NUM_VDEVS                 4                /* TARGET_TLV_NUM_VDEVS */
#define MAX_MEM_REQS              8
#define MAX_MEM_TOTAL             (16u << 20)

typedef struct { UINT32 Ver0, Ver1, Ns0, Ns1, Ns2, Ns3; } WMI_ABI;

typedef struct {                                   /* wmi_tlv_svc_rdy_ev (head) */
  UINT32  FwBuild;
  WMI_ABI Abi;
  UINT32  PhyCap, MaxFrag, NumRfChains, HtCap, VhtCap, VhtMcs, MinTxPower, MaxTxPower, SysCap;
  UINT32  MinPktSize, MaxBcnIe, NumMemReqs, MaxScanChans, HwBdId;
} WMI_SVC_RDY;

typedef struct { UINT32 ReqId, UnitSize, NumUnitInfo, NumUnits; } WMI_MEM_REQ;

STATIC BOOLEAN  mInitSent, mReadyRx;
STATIC UINT32   mSvcMap[32];                       /* TLV base map: 4 services per u32 word */
STATIC UINT32   mSvcExt[4];                        /* SERVICE_AVAILABLE: services 128.., 32 per word */
STATIC UINT32   mNumReqs;
STATIC WMI_MEM_REQ mReq[MAX_MEM_REQS];
STATIC UINT64   mChunkPa[MAX_MEM_REQS];
STATIC UINT32   mChunkLen[MAX_MEM_REQS];
STATIC UINT8    mMac[6];
STATIC UINT32   mEvents, mUnknown, mChanInfo;

/* Walk TLVs: Fn(tag, value, len) for each one at this level. */
typedef VOID (*TLV_FN)(UINT16 Tag, CONST UINT8 *V, UINT32 Len);

STATIC VOID TlvWalk(CONST UINT8 *P, UINT32 Len, TLV_FN Fn)
{
  UINT32 o = 0;

  while (o + 4 <= Len) {
    UINT16 l = *(CONST UINT16 *)&P[o], tag = *(CONST UINT16 *)&P[o + 2];
    if (o + 4 + l > Len) {
      Out ("  wmi: tlv tag %x len %u overruns %u at %u\r\n", tag, l, Len, o);
      return;
    }
    Fn (tag, P + o + 4, l);
    o += 4 + l;
  }
}

/* WMI_SERVICE_IS_ENABLED (BIT(id % sizeof(u32)): 4 per word) / WMI_TLV_EXT_SERVICE_IS_ENABLED */
STATIC BOOLEAN SvcBit(UINT32 Bit)
{
  if (Bit < 128) {
    return (mSvcMap[Bit / 4] & (1u << (Bit % 4))) != 0;
  }
  Bit -= 128;
  return Bit < 128 && (mSvcExt[Bit / 32] & (1u << (Bit % 32))) != 0;
}

STATIC BOOLEAN mSeenBmap, mSeenReqs;

STATIC VOID OnMemReqTlv(UINT16 Tag, CONST UINT8 *V, UINT32 Len)
{
  if (Tag != TAG_HOST_MEM_REQ || Len < sizeof (WMI_MEM_REQ) || mNumReqs >= MAX_MEM_REQS) {
    return;
  }
  CopyMem (&mReq[mNumReqs], V, sizeof (WMI_MEM_REQ));
  Out ("  wmi: mem_req %u: id %u unit_size %u num_unit_info %x num_units %u\r\n", mNumReqs,
       mReq[mNumReqs].ReqId, mReq[mNumReqs].UnitSize, mReq[mNumReqs].NumUnitInfo, mReq[mNumReqs].NumUnits);
  mNumReqs++;
}

/* ath10k_wmi_tlv_svc_rdy_parse */
STATIC VOID OnSvcRdyTlv(UINT16 Tag, CONST UINT8 *V, UINT32 Len)
{
  CONST UINT32 *u = (CONST UINT32 *)V;
  CONST WMI_SVC_RDY *ev = (CONST WMI_SVC_RDY *)V;

  switch (Tag) {
  case TAG_SERVICE_READY:
    if (Len < sizeof (*ev)) {
      break;
    }
    Out ("  wmi: fw %08x abi %08x/%u ns %08x %08x, phy_cap %x rf_chains %u ht %08x vht %08x/%x\r\n",
         ev->FwBuild, ev->Abi.Ver0, ev->Abi.Ver1, ev->Abi.Ns0, ev->Abi.Ns1, ev->PhyCap, ev->NumRfChains,
         ev->HtCap, ev->VhtCap, ev->VhtMcs);
    Out ("  wmi: tx power %u..%u, sys_cap %x, max_frag %u, num_mem_reqs %u, max_scan_chans %u, bd %x\r\n",
         ev->MinTxPower, ev->MaxTxPower, ev->SysCap, ev->MaxFrag, ev->NumMemReqs, ev->MaxScanChans, ev->HwBdId);
    if (ev->Abi.Ver0 != ABI_VER0 || ev->Abi.Ns0 != ABI_NS0 || ev->Abi.Ns1 != ABI_NS1) {
      Out ("  wmi: ABI differs from ath10k's TLV ABI 1.0 QCA_ML (continuing anyway)\r\n");
    }
    break;
  case TAG_HAL_REG_CAPS:
    if (Len >= 9 * 4) {
      Out ("  wmi: regdomain %x ext %x cap1 %x cap2 %x modes %x, 2G %u-%u 5G %u-%u\r\n", u[0], u[1], u[2], u[3],
           u[4], u[5], u[6], u[7], u[8]);
    }
    break;
  case TAG_ARRAY_UINT32:
    if (!mSeenBmap) {
      mSeenBmap = TRUE;
      {
        CHAR8 nib[33];
        UINT32 i;
        CopyMem (mSvcMap, V, MIN (Len, sizeof (mSvcMap)));
        for (i = 0; i < 32; i++) {
          nib[i] = "0123456789abcdef"[mSvcMap[i] & 0xF];
        }
        nib[32] = 0;
        Out ("  wmi: service map (%u B, nibble per 4 services 0..127) %a\r\n", Len, nib);
      }
    }
    break;
  case TAG_ARRAY_STRUCT:
    if (!mSeenReqs) {
      mSeenReqs = TRUE;
      TlvWalk (V, Len, OnMemReqTlv);
    }
    break;
  default:
    break;
  }
}

/* wmi_tlv_resource_config, field order as ath10k (44 x u32) */
typedef struct {
  UINT32 NumVdevs, NumPeers, NumOffloadPeers, NumOffloadReorderBufs, NumPeerKeys, NumTids, AstSkidLimit;
  UINT32 TxChainMask, RxChainMask, RxTimeoutPri[4], RxDecapMode, ScanMaxPendingReqs, BmissOffloadMaxVdev;
  UINT32 RoamOffloadMaxVdev, RoamOffloadMaxApProfiles, NumMcastGroups, NumMcastTableElems, Mcast2UcastMode;
  UINT32 TxDbgLogSize, NumWdsEntries, DmaBurstSize, MacAggrDelim, RxSkipDefragTimeoutDupDetectionCheck;
  UINT32 VowConfig, GtkOffloadMaxVdev, NumMsduDesc, MaxFragEntries, NumTdlsVdevs, NumTdlsConnTableEntries;
  UINT32 BeaconTxOffloadMaxVdev, NumMulticastFilterEntries, NumWowFilters, NumKeepAlivePattern;
  UINT32 KeepAlivePatternSize, MaxTdlsConcurrentSleepSta, MaxTdlsConcurrentBufferSta, WmiSendSeparate;
  UINT32 NumOcbVdevs, NumOcbChannels, NumOcbSchedules, HostCapab;
} WMI_RES_CFG;

/* ath10k_wmi_tlv_op_gen_init, values for wcn3990 (snoc, HL TLV peers, native wifi decap) */
STATIC VOID FillResCfg(WMI_RES_CFG *C)
{
  ZeroMem (C, sizeof (*C));
  C->NumVdevs = NUM_VDEVS;
  C->NumPeers = NUM_PEERS;
  C->AstSkidLimit = 16;                            /* TARGET_HL_TLV_AST_SKID_LIMIT */
  C->NumWdsEntries = 2;                            /* TARGET_HL_TLV_NUM_WDS_ENTRIES */
  if (SvcBit (SVC_RX_FULL_REORDER)) {
    C->NumOffloadPeers = NUM_VDEVS;
    C->NumOffloadReorderBufs = NUM_VDEVS;
  }
  C->NumPeerKeys = 2;
  C->NumTids = NUM_PEERS * 2;
  C->TxChainMask = 0x7;
  C->RxChainMask = 0x7;
  C->RxTimeoutPri[0] = 0x64;
  C->RxTimeoutPri[1] = 0x64;
  C->RxTimeoutPri[2] = 0x64;
  C->RxTimeoutPri[3] = 0x28;
  C->RxDecapMode = 1;                              /* ATH10K_HW_TXRX_NATIVE_WIFI */
  C->ScanMaxPendingReqs = 4;
  C->BmissOffloadMaxVdev = NUM_VDEVS;
  C->RoamOffloadMaxVdev = NUM_VDEVS;
  C->RoamOffloadMaxApProfiles = 8;
  C->TxDbgLogSize = 0x400;
  C->GtkOffloadMaxVdev = 2;
  C->NumMsduDesc = 1024 + 32;                      /* TARGET_TLV_NUM_MSDU_DESC */
  C->MaxFragEntries = 2;
  C->NumTdlsVdevs = 1;
  C->NumTdlsConnTableEntries = 0x20;
  C->BeaconTxOffloadMaxVdev = 2;
  C->NumMulticastFilterEntries = 5;
  C->NumWowFilters = 22;                           /* TARGET_TLV_NUM_WOW_PATTERNS */
  C->NumKeepAlivePattern = 6;
  C->MaxTdlsConcurrentSleepSta = 1;
  C->MaxTdlsConcurrentBufferSta = 1;
  C->HostCapab = 1u << 9;                          /* WMI_TLV_FLAG_MGMT_BUNDLE_TX_COMPL */
}

/* ath10k_wmi_event_service_ready_work + ath10k_wmi_alloc_host_mem: one PhysAlloc block. */
STATIC UINT32 AllocChunks(VOID)
{
  UINT32 i, units, total = 0, off = 0;
  UINT64 pa = 0;

  for (i = 0; i < mNumReqs; i++) {
    units = mReq[i].NumUnits;
    if (mReq[i].NumUnitInfo & 4) {                 /* NUM_UNITS_IS_NUM_ACTIVE_PEERS */
      units = NUM_PEERS + 1;
    } else if (mReq[i].NumUnitInfo & 2) {          /* NUM_UNITS_IS_NUM_PEERS (+ self peer) */
      units = NUM_PEERS + 1;
    } else if (mReq[i].NumUnitInfo & 1) {          /* NUM_UNITS_IS_NUM_VDEVS */
      units = NUM_VDEVS + 1;
    }
    mChunkLen[i] = units * ALIGN_VALUE (mReq[i].UnitSize, 4);
    total += ALIGN_VALUE (mChunkLen[i], SIZE_4KB);
  }
  if (total == 0) {
    return 0;
  }
  if (total > MAX_MEM_TOTAL || PhysAlloc (total, &pa) == NULL) {
    Out ("  wmi: host memory for %u req(s), %u bytes: %a\r\n", mNumReqs, total,
         total > MAX_MEM_TOTAL ? "too big, not given" : "PhysAlloc failed");
    return 0;
  }
  for (i = 0; i < mNumReqs; i++) {
    mChunkPa[i] = pa + off;
    off += ALIGN_VALUE (mChunkLen[i], SIZE_4KB);
    Out ("  wmi: chunk %u: req %u, %u bytes at %lx\r\n", i, mReq[i].ReqId, mChunkLen[i], mChunkPa[i]);
  }
  return mNumReqs;
}

UINT8 *WmiPutTlv(UINT8 *P, UINT16 Tag, UINT32 Len)
{
  *(UINT16 *)&P[0] = (UINT16)Len;
  *(UINT16 *)&P[2] = Tag;
  return P + 4;
}

/* ath10k_wmi_tlv_op_gen_init + wmi_cmd_hdr */
STATIC VOID SendInit(VOID)
{
  STATIC UINT8 m[1024];
  UINT32 n = AllocChunks (), i, len;
  UINT8 *p = m;
  WMI_ABI *abi;

  ZeroMem (m, sizeof (m));
  *(UINT32 *)p = WMI_CMD_INIT;
  p += 4;
  p = WmiPutTlv (p, TAG_INIT_CMD, sizeof (WMI_ABI) + 4);
  abi = (WMI_ABI *)p;
  abi->Ver0 = ABI_VER0;
  abi->Ver1 = ABI_VER1;
  abi->Ns0 = ABI_NS0;
  abi->Ns1 = ABI_NS1;
  *(UINT32 *)(p + sizeof (WMI_ABI)) = n;
  p += sizeof (WMI_ABI) + 4;
  p = WmiPutTlv (p, TAG_RESOURCE_CONFIG, sizeof (WMI_RES_CFG));
  FillResCfg ((WMI_RES_CFG *)p);
  p += sizeof (WMI_RES_CFG);
  p = WmiPutTlv (p, TAG_ARRAY_STRUCT, n * 20);
  for (i = 0; i < n; i++) {
    UINT32 *c = (UINT32 *)WmiPutTlv (p, TAG_HOST_MEM_CHUNK, 16);
    c[0] = mReq[i].ReqId;
    c[1] = (UINT32)mChunkPa[i];
    c[2] = mChunkLen[i];
    c[3] = SvcBit (SVC_EXTEND_ADDRESS) ? (UINT32)(mChunkPa[i] >> 32) : 0;
    p += 20;
  }
  len = (UINT32)(p - m);
  mInitSent = HtcWmiSend (m, len);
  Out ("  t=%u.%03u wmi: INIT (%u bytes, %u chunk(s), full_reorder %u) %a\r\n", T, len, n,
       SvcBit (SVC_RX_FULL_REORDER), mInitSent ? "sent, waiting for READY" : "SEND FAILED");
  if (mInitSent) {
    Step ("WMI INIT sent");
  }
}

/* wmi_tlv_rdy_ev: abi, mac (6 + 2 pad), status */
STATIC VOID OnReadyTlv(UINT16 Tag, CONST UINT8 *V, UINT32 Len)
{
  if (Tag != TAG_READY_EVENT || Len < sizeof (WMI_ABI) + 8 + 4) {
    return;
  }
  CopyMem (mMac, V + sizeof (WMI_ABI), 6);
  mReadyRx = TRUE;
  Out ("  t=%u.%03u *** WMI READY: MAC %02x:%02x:%02x:%02x:%02x:%02x status %u, abi %08x/%u ***\r\n", T,
       mMac[0], mMac[1], mMac[2], mMac[3], mMac[4], mMac[5], *(CONST UINT32 *)(V + sizeof (WMI_ABI) + 8),
       ((CONST WMI_ABI *)V)->Ver0, ((CONST WMI_ABI *)V)->Ver1);
  Step ("WMI READY");
}

/* Called by htc.c for every message on the WMI_CONTROL endpoint. */
VOID WmiRx(CONST UINT8 *D, UINT32 Len)
{
  UINT32 id;

  if (Len < 4) {
    return;
  }
  id = *(CONST UINT32 *)D & 0xFFFFFF;
  mEvents++;
  switch (id) {
  case WMI_EV_SERVICE_AVAILABLE:
    /* value = u32 bitmap length (bits) + ext bitmap (ath10k_wmi_tlv_svc_avail_parse) */
    if (Len >= 4 + 4 + 4 + sizeof (mSvcExt) && *(CONST UINT16 *)(D + 6) == TAG_SERVICE_AVAILABLE) {
      CopyMem (mSvcExt, D + 12, sizeof (mSvcExt));
    }
    Out ("  t=%u.%03u wmi: SERVICE_AVAILABLE, %u bytes, ext map %08x %08x %08x %08x\r\n", T, Len, mSvcExt[0],
         mSvcExt[1], mSvcExt[2], mSvcExt[3]);
    break;
  case WMI_EV_SERVICE_READY:
    Out ("  t=%u.%03u wmi: SERVICE_READY, %u bytes\r\n", T, Len);
    Step ("WMI SERVICE_READY");
    TlvWalk (D + 4, Len - 4, OnSvcRdyTlv);
    if (!mInitSent) {
      SendInit ();
    }
    break;
  case WMI_EV_SERVICE_READY_EXT:
  case WMI_EV_SERVICE_READY_EXT2:
    Out ("  t=%u.%03u wmi: SERVICE_READY_EXT%a, %u bytes (ignored, like ath10k)\r\n", T,
         id == WMI_EV_SERVICE_READY_EXT2 ? "2" : "", Len);
    break;
  case WMI_EV_READY:
    Out ("  t=%u.%03u wmi: READY event, %u bytes\r\n", T, Len);
    TlvWalk (D + 4, Len - 4, OnReadyTlv);
    if (mReadyRx) {
      ScanStart ();                                /* channel list, vdev, passive scan */
    }
    break;
  case WMI_EV_SCAN:
    ScanEvent (D + 4, Len - 4);
    break;
  case WMI_EV_MGMT_RX:
    ScanMgmtRx (D + 4, Len - 4);
    break;
  case WMI_EV_CHAN_INFO:                           /* per-channel stats during scans */
    mChanInfo++;
    ScanChanInfo (D + 4, Len - 4);
    break;
  default:
    if (mUnknown++ < 32) {
      Out ("  t=%u.%03u wmi: event %x, %u bytes\r\n", T, id, Len);
    }
    break;
  }
}

VOID WmiSummary(VOID)
{
  Out ("  wmi: %u events (%u chan info), init %a, ready %a, MAC %02x:%02x:%02x:%02x:%02x:%02x, %u mem req(s)\r\n",
       mEvents, mChanInfo, mInitSent ? "sent" : "no", mReadyRx ? "YES" : "no", mMac[0], mMac[1], mMac[2], mMac[3],
       mMac[4], mMac[5], mNumReqs);
}

/* wmi_cmd_hdr + TLVs, on the WMI endpoint (queued while HTC has no credit). */
BOOLEAN WmiSend(UINT32 CmdId, CONST VOID *Tlvs, UINT32 Len)
{
  STATIC UINT8 m[2048];

  if (Len + 4 > sizeof (m)) {
    return FALSE;
  }
  *(UINT32 *)m = CmdId & 0xFFFFFF;
  CopyMem (m + 4, Tlvs, Len);
  return HtcWmiSend (m, Len + 4);
}
