/*
 * P4 first radio use: a PASSIVE scan over WMI (ath10k mac.c hw_scan + update_channel_list).
 * After WMI READY: SCAN_CHAN_LIST (every channel flagged passive: no transmit), VDEV_CREATE
 * (vdev 0, station, locally administered MAC), START_SCAN with ath10k's defaults. Beacons and
 * probe responses arrive as WMI MGMT_RX events; each new BSS is logged once. BSSIDs are logged
 * with the last three bytes masked (they are geolocatable).
 */
#include "Modem.h"
#ifdef TOPAZ_WIFICX
#include "wlanif.h"                                /* TopazWifi: scans on Windows' request */
#endif

#define Out ModemOut
#define Step WlfwSetStep
#define T   (UINT32)(ModemMs () / 1000), (UINT32)(ModemMs () % 1000)

#define CMD_START_SCAN          0x3001
#define CMD_SCAN_CHAN_LIST      0x3003
#define CMD_VDEV_CREATE         0x5001

#define TAG_ARRAY_UINT32        0x10
#define TAG_ARRAY_BYTE          0x11
#define TAG_ARRAY_STRUCT        0x12
#define TAG_ARRAY_FIXED_STRUCT  0x13
#define TAG_SCAN_EVENT          0x24
#define TAG_MGMT_RX_HDR         0x2C
#define TAG_START_SCAN_CMD      0x4D
#define TAG_SCAN_CHAN_LIST_CMD  0x4F
#define TAG_CHANNEL             0x50
#define TAG_VDEV_CREATE_CMD     0x56

#define CHAN_FLAG_PASSIVE       (1u << 7)
#define CHAN_FLAG_ALLOW_HT      (1u << 11)
#define CHAN_FLAG_ALLOW_VHT     (1u << 12)
#define MODE_11A                0
#define MODE_11G                1

#define SCAN_FLAG_PASSIVE       0x01
#define SCAN_CHAN_STAT_EVENT    0x10
#define SCAN_FILTER_PROBE_REQ   0x20               /* inverted logic on WMI-TLV (ath10k xors it) */

#define EV_STARTED              0x001
#define EV_COMPLETED            0x002
#define EV_BSS_CHANNEL          0x004
#define EV_FOREIGN_CHANNEL      0x008
#define EV_DEQUEUED             0x010
#define EV_PREEMPTED            0x020
#define EV_START_FAILED         0x040
#define EV_RESTARTED            0x080
#define EV_FOREIGN_EXIT         0x100

#define VDEV_ID                 0
#define VDEV_TYPE_STA           2
#define MAX_SCANS               3
#define RESCAN_MS               10000
#define MAX_BSS                 64
#define NOISE_FLOOR             (-95)              /* ATH10K_DEFAULT_NOISE_FLOOR */

/* 2.4 GHz 1-13, 5 GHz UNII-1/2/2e/3 (20 MHz centres) */
STATIC CONST UINT16 mFreqs[] = {
  2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
  5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320,
  5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
  5745, 5765, 5785, 5805, 5825,
};
#define NFREQ ARRAY_SIZE (mFreqs)

/* locally administered unicast ("TOPAZ"); the real one lives in Android's persist (wlan_mac.bin) */
STATIC CONST UINT8 mOurMac[6] = { 0x02, 0x54, 0x4F, 0x50, 0x41, 0x5A };

typedef struct { UINT8 Bssid[6]; CHAR8 Ssid[33]; UINT8 Chan; INT8 Rssi; UINT16 Seen; } BSS;

STATIC BSS     mBss[MAX_BSS];
STATIC UINT32  mNumBss, mScans, mForeign, mFrames, mOtherFrames, mChanInfo;
STATIC UINTN   mNextScanMs;
STATIC BOOLEAN mStarted, mScanning;
STATIC volatile BOOLEAN mScanReq;                  /* TOPAZ_WIFICX: a scan asked for by WiFiCx */

STATIC UINT8 *PutU32(UINT8 *P, UINT32 V)
{
  *(UINT32 *)P = V;
  return P + 4;
}

/* ath10k_wmi_tlv_op_gen_scan_chan_list + ath10k_wmi_put_wmi_channel (legacy modes, as ath10k) */
STATIC BOOLEAN SendChanList(VOID)
{
  STATIC UINT8 m[1536];
  UINT8 *p = m, *c;
  UINT32 i, mode, flags;

  p = PutU32 (WmiPutTlv (p, TAG_SCAN_CHAN_LIST_CMD, 4), NFREQ);
  p = WmiPutTlv (p, TAG_ARRAY_STRUCT, NFREQ * (4 + 24));
  for (i = 0; i < NFREQ; i++) {
    mode  = mFreqs[i] < 4000 ? MODE_11G : MODE_11A;
    flags = mode | CHAN_FLAG_PASSIVE | CHAN_FLAG_ALLOW_HT | (mFreqs[i] < 4000 ? 0 : CHAN_FLAG_ALLOW_VHT);
    c = WmiPutTlv (p, TAG_CHANNEL, 24);
    c = PutU32 (c, mFreqs[i]);                     /* mhz */
    c = PutU32 (c, mFreqs[i]);                     /* band_center_freq1 */
    c = PutU32 (c, 0);                             /* band_center_freq2 */
    c = PutU32 (c, flags);                         /* mode in [5:0] + WMI_CHAN_FLAG_* */
    c = PutU32 (c, (40u << 8) | (40u << 16));      /* min 0, max/reg power 20 dBm (0.5 dBm units) */
    p = PutU32 (c, 40u << 8);                      /* antenna 0, max_tx_power 20 dBm */
  }
  return WmiSend (CMD_SCAN_CHAN_LIST, m, (UINT32)(p - m));
}

/* ath10k_wmi_tlv_op_gen_vdev_create */
STATIC BOOLEAN SendVdevCreate(VOID)
{
  UINT8 m[4 + 20], *p;

  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, TAG_VDEV_CREATE_CMD, 20);
  p = PutU32 (p, VDEV_ID);
  p = PutU32 (p, VDEV_TYPE_STA);
  p = PutU32 (p, 0);                               /* subtype none */
  CopyMem (p, mOurMac, 6);                         /* wmi_mac_addr: 6 bytes + 2 pad */
  return WmiSend (CMD_VDEV_CREATE, m, sizeof (m));
}

/* ath10k_wmi_start_scan_init defaults + ath10k_wmi_tlv_op_gen_start_scan, passive, no IEs */
STATIC BOOLEAN SendStartScan(VOID)
{
  STATIC UINT8 m[512];
  UINT8 *p = m, *c;
  UINT32 i;

  ZeroMem (m, sizeof (m));
  c = WmiPutTlv (p, TAG_START_SCAN_CMD, 100);
  c = PutU32 (c, 0xA000 | (mScans + 1));           /* scan_id (WMI_HOST_SCAN_REQ_ID_PREFIX) */
  c = PutU32 (c, 0xA000 | 1);                      /* scan_req_id */
  c = PutU32 (c, VDEV_ID);
  c = PutU32 (c, 1);                               /* WMI_SCAN_PRIORITY_LOW */
  c = PutU32 (c, EV_STARTED | EV_COMPLETED | EV_BSS_CHANNEL | EV_FOREIGN_CHANNEL | EV_FOREIGN_EXIT | EV_DEQUEUED);
  c = PutU32 (c, 50);                              /* dwell_time_active */
  c = PutU32 (c, 150);                             /* dwell_time_passive */
  c = PutU32 (c, 50);                              /* min_rest_time */
  c = PutU32 (c, 500);                             /* max_rest_time */
  c = PutU32 (c, 0);                               /* repeat_probe_time */
  c = PutU32 (c, 0);                               /* probe_spacing_time */
  c = PutU32 (c, 0);                               /* idle_time */
  c = PutU32 (c, 20000);                           /* max_scan_time */
  c = PutU32 (c, 5);                               /* probe_delay */
  c = PutU32 (c, (SCAN_FLAG_PASSIVE | SCAN_CHAN_STAT_EVENT) ^ SCAN_FILTER_PROBE_REQ);
  c = PutU32 (c, 0);                               /* burst_duration_ms */
  c = PutU32 (c, NFREQ);                           /* num_channels */
  c = PutU32 (c, 1);                               /* num_bssids */
  c = PutU32 (c, 0);                               /* num_ssids */
  c = PutU32 (c, 0);                               /* ie_len */
  c = PutU32 (c, 3);                               /* num_probes */
  p = c + 16;                                      /* mac_addr + mac_mask: zero */
  c = WmiPutTlv (p, TAG_ARRAY_UINT32, NFREQ * 4);
  for (i = 0; i < NFREQ; i++) {
    c = PutU32 (c, mFreqs[i]);
  }
  p = WmiPutTlv (c, TAG_ARRAY_FIXED_STRUCT, 0);    /* ssids: none (passive) */
  c = WmiPutTlv (p, TAG_ARRAY_FIXED_STRUCT, 8);    /* bssids: broadcast */
  RtlFillMemory (c, 6, 0xFF);
  p = WmiPutTlv (c + 8, TAG_ARRAY_BYTE, 0);        /* extra IEs: none */
  return WmiSend (CMD_START_SCAN, m, (UINT32)(p - m));
}

/* After WMI READY: what ath10k does on interface up + hw_scan, minus everything a scan doesn't need. */
VOID ScanStart(VOID)
{
  BOOLEAN a, b, c;

  if (mStarted) {
    return;
  }
  mStarted = TRUE;
  PmicProbe ("WMI READY");
  ScanRequestStats ("baseline");
  a = SendChanList ();
  b = SendVdevCreate ();
#ifdef TOPAZ_WIFICX
  c = FALSE;                                       /* Windows decides when to scan */
  WlanOnReady (mOurMac);
#else
  c = SendStartScan ();
#endif
  mScanning = c;
  Out ("  t=%u.%03u scan: chan list (%u ch) %a, vdev %u create %a, passive scan 1 %a\r\n", T, (UINT32)NFREQ,
       a ? "ok" : "FAILED", VDEV_ID, b ? "ok" : "FAILED", c ? "requested" : "FAILED");
  Step ("WMI scan requested");
}

STATIC CONST CHAR8 *EvName(UINT32 E)
{
  switch (E) {
  case EV_STARTED:       return "STARTED";
  case EV_COMPLETED:     return "COMPLETED";
  case EV_BSS_CHANNEL:   return "BSS_CHANNEL";
  case EV_DEQUEUED:      return "DEQUEUED";
  case EV_PREEMPTED:     return "PREEMPTED";
  case EV_START_FAILED:  return "START_FAILED";
  case EV_RESTARTED:     return "RESTARTED";
  default:               return "?";
  }
}

/* wmi_scan_event: type, reason, channel_freq, scan_req_id, scan_id, vdev_id */
VOID ScanEvent(CONST UINT8 *Tlvs, UINT32 Len)
{
  CONST UINT32 *e;

  if (Len < 4 + 24 || *(CONST UINT16 *)(Tlvs + 2) != TAG_SCAN_EVENT) {
    Out ("  scan: short/odd scan event (%u bytes)\r\n", Len);
    return;
  }
  e = (CONST UINT32 *)(Tlvs + 4);
  if (e[0] == EV_FOREIGN_CHANNEL) {
    if (mForeign++ < 3) {
      Out ("  t=%u.%03u scan: on %u MHz\r\n", T, e[2]);
    }
    return;
  }
  if (e[0] == EV_FOREIGN_EXIT || e[0] == EV_BSS_CHANNEL) {
    return;
  }
  Out ("  t=%u.%03u scan: %a reason %u freq %u scan_id %x vdev %u\r\n", T, EvName (e[0]), e[1], e[2], e[4], e[5]);
  if (e[0] == EV_COMPLETED || e[0] == EV_START_FAILED || e[0] == EV_DEQUEUED) {
    mScanning = FALSE;
    mScans++;
    Out ("  t=%u.%03u *** scan %u done: %u channels visited, %u BSS known, %u beacons/probe resp, %u chan info ***\r\n",
         T, mScans, mForeign, mNumBss, mFrames, mChanInfo);
    mForeign = 0;
#ifdef TOPAZ_WIFICX
    mNextScanMs = 0;
    WlanOnScanDone (e[0] == EV_COMPLETED ? 0 : (INT32)e[0]);
#else
    mNextScanMs = mScans < MAX_SCANS ? ModemMs () + RESCAN_MS : 0;
#endif
    if (mScans == 1 || mScans == MAX_SCANS) {
      PmicProbe ("after scan");
      ScanRequestStats ("after scan");
    }
    Step ("WMI scan done");
  }
}

/* One beacon / probe response (802.11 mgmt frame) -> BSS table. */
STATIC VOID OnFrame(CONST UINT8 *F, UINT32 Len, UINT32 Chan, INT32 Rssi)
{
  UINT32 o, i, n;
  CHAR8 ssid[33];
  BSS *b = NULL;

  if (Len < 36 || (F[0] & 0x0C) != 0 || ((F[0] >> 4) != 8 && (F[0] >> 4) != 5)) {
    mOtherFrames++;
    return;
  }
  mFrames++;
  ssid[0] = 0;
  for (o = 36; o + 2 <= Len && o + 2 + F[o + 1] <= Len; o += 2 + F[o + 1]) {
    if (F[o] == 0) {                               /* SSID */
      n = MIN (F[o + 1], 32);
      for (i = 0; i < n; i++) {
        UINT8 ch = F[o + 2 + i];
        ssid[i] = (ch >= 0x20 && ch < 0x7F) ? (CHAR8)ch : (ch >= 0x80 ? (CHAR8)ch : '?');   /* keep UTF-8 */
      }
      ssid[n] = 0;
      if (n == 0 || F[o + 2] == 0) {
        AsciiStrCpyS (ssid, sizeof (ssid), "<hidden>");
      }
    } else if (F[o] == 3 && F[o + 1] == 1) {        /* DS parameter set: current channel */
      Chan = F[o + 2];
    }
  }
#ifdef TOPAZ_WIFICX
  WlanOnBss (F + 16, F + 24, Len - 24, Chan, Rssi, (F[0] >> 4) == 5);
#endif
  for (i = 0; i < mNumBss; i++) {
    if (CompareMem (mBss[i].Bssid, F + 16, 6) == 0) {
      b = &mBss[i];
      break;
    }
  }
  if (b != NULL) {
    b->Seen++;
    if (Rssi > b->Rssi) {
      b->Rssi = (INT8)Rssi;
    }
    return;
  }
  if (mNumBss >= MAX_BSS) {
    return;
  }
  b = &mBss[mNumBss++];
  CopyMem (b->Bssid, F + 16, 6);
  AsciiStrCpyS (b->Ssid, sizeof (b->Ssid), ssid);
  b->Chan = (UINT8)Chan;
  b->Rssi = (INT8)Rssi;
  b->Seen = 1;
  Out ("  t=%u.%03u bss %2u: %02x:%02x:%02x:**:**:** ch %3u %4d dBm '%a'\r\n", T, mNumBss, b->Bssid[0],
       b->Bssid[1], b->Bssid[2], b->Chan, b->Rssi, b->Ssid);
  if (mNumBss == 1) {
    Step ("first BSS seen");
  }
}

/* WMI MGMT_RX: TLV mgmt_rx_hdr {channel, snr, rate, phy_mode, buf_len, status, rssi[4]} + byte array */
VOID ScanMgmtRx(CONST UINT8 *Tlvs, UINT32 Len)
{
  CONST UINT32 *hdr = NULL;
  CONST UINT8 *frame = NULL;
  UINT32 o = 0, flen = 0;

  while (o + 4 <= Len) {
    UINT16 l = *(CONST UINT16 *)&Tlvs[o], tag = *(CONST UINT16 *)&Tlvs[o + 2];
    if (o + 4 + l > Len) {
      break;
    }
    if (tag == TAG_MGMT_RX_HDR && l >= 24 && hdr == NULL) {
      hdr = (CONST UINT32 *)&Tlvs[o + 4];
    } else if (tag == TAG_ARRAY_BYTE && frame == NULL) {
      frame = &Tlvs[o + 4];
      flen = l;
    }
    o += 4 + l;
  }
  if (hdr == NULL || frame == NULL) {
    mOtherFrames++;
    return;
  }
  OnFrame (frame, MIN (hdr[4], flen), hdr[0], (INT32)hdr[1] + NOISE_FLOOR);
}

/* From the GLINK loop: re-scan a few times so the table fills up. */
#ifdef TOPAZ_WIFICX
VOID WlanScanRequest(VOID)
{
  mScanReq = TRUE;
}
#endif

VOID ScanPoll(VOID)
{
#ifdef TOPAZ_WIFICX
  if (mStarted && !mScanning && mScanReq) {
    mScanReq = FALSE;
    mScanning = SendStartScan ();
    Out ("  t=%u.%03u scan: Windows scan request -> passive scan %u %a\r\n", T, mScans + 1,
         mScanning ? "requested" : "FAILED");
    if (!mScanning) {
      WlanOnScanDone (-1);
    }
  }
#endif
  if (mStarted && !mScanning && mNextScanMs != 0 && ModemMs () >= mNextScanMs) {
    mNextScanMs = 0;
    mScanning = SendStartScan ();
    Out ("  t=%u.%03u scan: passive scan %u %a\r\n", T, mScans + 1, mScanning ? "requested" : "FAILED");
  }
}

VOID ScanSummary(VOID)
{
  Out ("  scan: %u scan(s) done, %u BSS, %u beacons/probe resp, %u other frames, %u chan info\r\n", mScans,
       mNumBss, mFrames, mOtherFrames, mChanInfo);
}

/* wmi_tlv_chan_info_event: is the radio receiving at all? (rx_frame_count, noise floor) */
VOID ScanChanInfo(CONST UINT8 *Tlvs, UINT32 Len)
{
  CONST UINT32 *c;

  mChanInfo++;
  if (mChanInfo <= 4) {
    Out ("  t=%u.%03u chinfo #%u raw (%u bytes):\r\n", T, mChanInfo, Len);
    ScanDump ("chinfo", Tlvs, Len, 24);
  }
  if (Len < 4 + 52 || mScans != 0 || *(CONST UINT16 *)(Tlvs + 2) != 0x26) {   /* TAG_STRUCT_CHAN_INFO_EVENT */
    return;
  }
  c = (CONST UINT32 *)(Tlvs + 4);
  if ((c[2] & 1) == 0) {                           /* WMI_CHAN_INFO_FLAG_COMPLETE: end of the channel */
    return;
  }
  Out ("  chinfo %u MHz: err %u nf %d rx_clear %u cycle %u rx_frames %u tx_frames %u mac_clk %u\r\n", c[1], c[0],
       (INT32)c[3], c[4], c[5], c[8], c[11], c[12]);
}

/* Raw dump as u32 words, 8 per line. */
VOID ScanDump(CONST CHAR8 *Tag, CONST UINT8 *P, UINT32 Len, UINT32 MaxWords)
{
  CONST UINT32 *w = (CONST UINT32 *)P;
  UINT32 n = MIN (Len / 4, MaxWords), i;

  for (i = 0; i < n; i += 8) {
    Out ("    %a +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\r\n", Tag, i * 4, w[i],
         i + 1 < n ? w[i + 1] : 0, i + 2 < n ? w[i + 2] : 0, i + 3 < n ? w[i + 3] : 0, i + 4 < n ? w[i + 4] : 0,
         i + 5 < n ? w[i + 5] : 0, i + 6 < n ? w[i + 6] : 0, i + 7 < n ? w[i + 7] : 0);
  }
}

/* WMI REQUEST_STATS (pdev): base {chan_nf, tx_frame, rx_frame, rx_clear, cycle, phy_err, tx_pwr} + tx + rx */
VOID ScanRequestStats(CONST CHAR8 *Why)
{
  UINT8 m[4 + 20], *p;
  BOOLEAN ok;

  ZeroMem (m, sizeof (m));
  p = WmiPutTlv (m, 0x8F, 20);                     /* TAG_STRUCT_REQUEST_STATS_CMD */
  p = PutU32 (p, 1u << 2);                         /* WMI_TLV_STAT_PDEV */
  ok = WmiSend (0x16001, m, sizeof (m));           /* WMI_TLV_REQUEST_STATS_CMDID */
  Out ("  t=%u.%03u stats: pdev stats requested (%a) %a\r\n", T, Why, ok ? "ok" : "FAILED");
}

/* WMI UPDATE_STATS: TLV stats_event {stats_id, num_pdev, ...} + byte array with the stats */
VOID ScanStats(CONST UINT8 *Tlvs, UINT32 Len)
{
  UINT32 o = 0;

  while (o + 4 <= Len) {
    UINT16 l = *(CONST UINT16 *)&Tlvs[o], tag = *(CONST UINT16 *)&Tlvs[o + 2];
    CONST UINT32 *v = (CONST UINT32 *)&Tlvs[o + 4];
    if (o + 4 + l > Len) {
      break;
    }
    if (tag == 0x46 && l >= 8) {                   /* TAG_STRUCT_STATS_EVENT */
      Out ("  t=%u.%03u stats: id %x, %u pdev, %u vdev, %u peer\r\n", T, v[0], v[1], v[2], v[3]);
    } else if (tag == 0x11 && l >= 28) {           /* byte array: pdev stats first */
      Out ("  stats: chan_nf %d, tx_frame %u, rx_frame %u, rx_clear %u, cycle %u, phy_err %u, tx_pwr %u\r\n",
           (INT32)v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
      ScanDump ("pdev", (CONST UINT8 *)v, l, 64);
    }
    o += 4 + l;
  }
}
