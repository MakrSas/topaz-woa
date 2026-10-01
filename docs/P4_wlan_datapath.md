# P4: WLAN data path on topaz (WCN3990 via SNOC) — plan

Source: upstream Linux `drivers/net/wireless/ath/ath10k/{snoc,ce,qmi,hw,htc}.c` (sparse clone on
s8build: `~/work/wifi/linux`). Everything below happens after WLFW `FW_READY` (done in TopazModem v0.3).

## Order (ath10k_snoc_hif_power_up + core start)

1. **QMI WLAN_CFG (0x23)** — tells the firmware our copy-engine layout:
   - TLV 0x11 `tgt_cfg`: u8 count + 12 x {pipe_num, pipe_dir, nentries, nbytes_max, flags} (u32 each)
     = `target_ce_config_wlan` (CE0 out 32x2048, CE1 in 32x2048, CE2 in 64x2048, CE3 out 32x2048,
     CE4 out 256x256 DIS_INTR, CE5 out 1024x64 DIS_INTR, CE6 inout 32x16384, CE7 dir 4 0x0,
     CE8 in 32x2048 flags 0, CE9/10/11 in 32x2048). PIPEDIR: 1 in, 2 out, 3 inout. DIS_INTR = bit 3.
   - TLV 0x12 `svc_cfg`: u8 count + N x {service_id, pipe_dir, pipe_num} (u32 each)
     = `target_service_to_ce_map_wlan`. Service id = group << 8 | idx: WMI 0x100..0x104 (out 3, in 2),
     RSVD_CTRL 0x001 (out 0, in 2), HTT_DATA 0x300 (out 4, in 1), HTT_DATA2 0x301 (in 9),
     HTT_DATA3 0x302 (in 10), HTT_LOG 0x600 (in 11), TEST_RAW 0xFE00 (out 0/5, in 2). Terminator {0,0,0}
     is in the table too (Linux sends it).
   - TLV 0x13 `shadow_reg`: u8 count + 12 x {u16 ce_id, u16 reg_offset}: SRC_WR_IDX 0x3C for CE 0,3,4,5,7;
     DST_WR_IDX 0x40 for CE 1,2,7,8,9,10,11.
   - no host_version (TLV 0x10 absent).
2. **QMI WLAN_MODE (0x22)**: TLV 0x01 mode (s32) = 0 MISSION, TLV 0x10 hw_debug u8 = 0.
3. **RRI**: 12 x u32 DMA buffer; CE wrapper 0x24C004 = addr lo, 0x24C008 = addr hi; in every CE set
   CTRL1 (0x18) bit "upd" (see `wcn3990_ctrl1_upd`) so the hardware writes read indices to memory.
4. **CE rings** (`ath10k_ce_init_src_ring/dest_ring`), CE n regs at 0xC800000 + 0x240000 + 0x1000*n:
   descriptors are `ce_desc_64` {u64 addr, u16 nbytes, u16 flags, u32 toeplitz} (16 B), ring sizes
   rounded to a power of 2, base 0x0/0x4 (src) 0xC/0x10 (dst), size 0x8/0x14, CTRL1 0x18 dmax[15:0],
   src/dst byte-swap bits 17/18 = 0, watermarks low 0 / high nentries, indices 0x3C/0x40/0x44/0x48.
   Host side (Linux host_ce_config_wlan): CE0 src 16, CE1 dst 512, CE2 dst 64, CE3 src 32, CE4 src 2048,
   CE5 dst 512, CE7 2/2, CE8 dst 128, CE9/10/11 dst 512.
5. **Post RX buffers** on CE1/CE2/CE5/CE8/CE9/CE10/CE11 (2048 B each), write dst write index.
6. **HTC**: the firmware sends HTC READY on CE1/CE2 (svc RSVD_CTRL in 2) -> connect WMI_CONTROL,
   HTT_DATA -> WMI READY event -> WMI init -> scan.

## Hard questions

- **DMA addresses**: every ring/buffer address goes through the apps SMMU, stream 0x1A0.
  TopazModem v0.4 logs what is there. Linux maps an IOVA pool 0xA0000000+256 MiB in a stage-1
  context bank ("fastmap"). Options: (a) stream already bypass -> use physical addresses; (b) a context
  bank exists with Linux-like tables -> unlikely under Windows; (c) fault -> we program our own
  SMR/S2CR + context bank with identity page tables for our DMA buffers (needs SMMU global register
  write access from EL1; under the Qualcomm hypervisor this may be trapped).
- **Interrupts**: CE IRQs SPI 0x166..0x171; we keep polling (read indices come via RRI memory).
- **Shadow registers** (WCN3990 hw_params: target_64bit, shadow_reg_support, rri_on_ddr): source-ring
  write indices go to shadow regs at membase + 0x32000 + 4*ce (CE 0, 3, 4, 5, 7) instead of CE+0x3C;
  dest-ring write indices still go to CE+0x40. RRI enable = CTRL1 bit 19 (mask 0x80000).

## Results (TopazModem v0.5, 2026-10-01, log docs/logs/TopazModem-v0.5.log)

Everything after FW_READY works EXCEPT the SMMU bypass:
- **WLAN_CFG ok, WLAN_MODE mission ok** at t=4.64 s; modem stays READY 90+ s, no FATAL.
- **CE registers readable after WLAN_MODE** (not before — reading them right after FW_READY hung the
  bus in v0.4). The firmware itself fills the CE ring base addresses, and they point into the MSA
  carveout (0x519xxxxx): ce0 dr 0x51948300, ce1 sr 0x51945c00, ce2 sr 0x51945380, ce6 sr/dr
  0x5193xxxx, ce9/10/11 sr 0x519xxxxx. ce wrapper +0xC = 0xdeadc0de (marker).
- **SMMU bypass REJECTED.** Writing the SMR works (SMR49 = 0x800101a0: valid, sid 0x1a0 mask 1
  survives), but S2CR type comes back **2 (FAULT)** though we wrote 1 (BYPASS). Qualcomm SMMUs
  force unconfigured/bypass streams to fault; UEFI's own streams all use **type 0 (translate) +
  a context bank** (SMR0..4 -> cb 0..3). So bypass is not an option here.

### Next: give WLAN its own translating context bank (identity map)
Replicate what UEFI does for its streams: S2CR type 0 -> a free context bank, stage-1, with page
tables that identity-map (IOVA = PA) the host DMA buffers we hand the firmware. Global writes from
EL1 are honoured (SMR took), so the per-CB regs (CBAR in GR1, SCTLR/TCR/TTBR0/MAIR in the CB page)
should take too. Steps: pick a free cb (UEFI used 0..3; take e.g. 8), CBAR = stage1 + our VMID,
TTBR0 = our L1 table PA, TCR/MAIR for a 32/36-bit AArch64 stage-1 map, SCTLR.M=1, then S2CR[49]
type 0 cbndx=8. Identity-map the CE ring region + our buffers. Verify by reading S2CR back as type 0.
Only then allocate host rings/buffers and start HTC.

## SMMU solved (TopazModem v0.7, 2026-10-01, log docs/logs/TopazModem-v0.7.log)

Global bypass is forbidden, but a **context bank with SCTLR.M=0 (translation off) = identity
pass-through** is allowed — that is exactly how UEFI maps its own streams (cb0..3: CBAR 0x1f000,
CBA2R 1 (VA64), SCTLR 0xe0, TCR/TTBR0/MAIR 0). We cloned that into a free bank (cb4), pointed
S2CR[5] type 0 at it for sid 0x1A0/mask1, and it took:
`SMR5 800101a0  S2CR 00000004 type 0 cb 4  <== WLAN`, SCTLR 0xe0. So host memory is now reachable
by the WLAN hardware without building page tables. FSR of cb4 stays 0x400 (same as UEFI's, benign).

All three P4 prerequisites are done: SMMU open, WLAN_CFG/WLAN_MODE accepted, CE engines readable.
Next: HTC/HTT/WMI — host allocates CE rings + RX buffers in (now reachable) host memory, waits for
HTC READY on CE1/CE2, connects WMI_CONTROL, WMI READY, then WiFiCx miniport.

## Prior art (searched 2026-10-01)
No open-source Windows driver exists for the integrated WCN3990 (SNOC). The only Windows driver is
Qualcomm's proprietary binary shipped for Surface Pro X / sc8180x & sc7180 (closed, ACPI/address-
bound to those platforms — not reusable on SM6225). Linux `ath10k` (snoc) is the only open reference,
and linux-surface documents the same bring-up chain (qrtr/pd-mapper/tqftpserv/rmtfs) we reimplemented.

## HTC up, WMI SERVICE_READY (TopazModem v0.8, 2026-10-01, log docs/logs/TopazModem-v0.8.log)

First end-to-end data path: host rings in Windows memory (PhysAlloc 0xEFC5F000 + 0xA1000), both
directions work through the identity SMMU context bank.
- After `CeStart` (RRI at the wrapper, CTRL1 bit19, host-owned rings, 32 RX buffers per pipe) the
  firmware's already-queued message on CE2 arrives at t=4.753 s, 120 ms after WLAN_MODE:
  **HTC READY: credit_count 2, credit_size 2184, max_endpoints 22**, extended, HTC version 1 (2.1).
- CONNECT HTT_DATA (0x300, no credit flow) -> status 0, **eid 1**, max_msg 2000;
  CONNECT WMI_CONTROL (0x100, 1 credit) -> status 0, **eid 2**, max_msg 2040; SETUP_COMPLETE_EX sent.
- The firmware then sends four WMI events on eid 2 (CE2): `3` SERVICE_AVAILABLE (48 B),
  `1` **SERVICE_READY** (316 B), `0x4009` (440 B) and `0x4022` (60 B). By the qcacld PDEV event
  numbering those two are most likely SERVICE_READY_EXT and SERVICE_READY_EXT2 (ath10k ignores both).
- TX completion works through the RRI memory: CE0 `rri 00030003` after 3 sends (2 connects +
  setup complete). CE2 rx 7. Nothing on CE1/CE5/CE9-11 yet (HTT is idle until WMI INIT).
- Modem stays READY 120+ s, no FATAL, no bus hang. Interrupts are not needed: pure polling at ~1 kHz.

Next (milestone 3): parse SERVICE_READY (TLVs: hal reg caps, mem_reqs) and send WMI_TLV INIT
(cmd 0x1: init_cmd + resource_config + host_mem_chunks from PhysAlloc), then wait for WMI READY
(event 2: MAC address, abi version). WMI sends go on CE3 with eid 2 and HTC credit flow (1 credit,
credits come back in HTC trailers).

## WMI READY (TopazModem v0.9, 2026-10-01, log docs/logs/TopazModem-v0.9.log)

- SERVICE_READY parsed: fw 0x32497fff, **ABI 1.0 "QCA_ML" (abi_ver1 1074, ath10k sends 53 — accepted)**,
  phy_cap 3, 1 RF chain, HT 0x3813, VHT 0x73901132, max 68 scan channels, **num_mem_reqs 0** (no host
  memory needed), regdomain 0x406c, 2G 2312-2732 / 5G 4912-6100 MHz.
- INIT (220 bytes: abi + ath10k wcn3990 resource config, 0 chunks) on CE3/eid 2 -> **WMI READY at
  t=4.779 s (16 ms later), status 0**, MAC 00:00:00:00:00:00 (normal for WCN3990: the host owns the MAC —
  Android reads persist/wlan_mac.bin; ath10k uses DT or a random one).
- Other events: 0x3a001 (148 B, before READY, likely regulatory channel list, newer than ath10k),
  0x401f (1148 B), 0x1d00a WLAN_FREQ_AVOID (LTE coex). After INIT the firmware takes over CE5 and CE8
  (both reprogrammed to MSA rings) — same as with Linux, our host rings there just stay idle.
- Bug found: the TLV base service map is 4 services per u32 word (ath10k WMI_SERVICE_IS_ENABLED uses
  BIT(id % sizeof(u32))), the ext map from SERVICE_AVAILABLE is {u32 bits, 4 x u32}. Fixed in v0.10.

Next (v0.10): passive scan over WMI — SCAN_CHAN_LIST (all passive), VDEV_CREATE (vdev 0 STA, local MAC
02:54:4f:50:41:5a), START_SCAN; beacons arrive as WMI MGMT_RX (0x7001). HTC credit reports are now logged
and WMI commands queue while the endpoint has no credit.

## Passive scan runs but hears nothing (TopazModem v0.10, log docs/logs/TopazModem-v0.10.log)

- WMI credit flow works: the 3 commands queued behind INIT's credit go out as the firmware returns
  credits in HTC trailers (credit report ep 2 +1 after each command; ep 0 also gets credits).
- SCAN_CHAN_LIST (37 ch, all passive) + VDEV_CREATE (vdev 0 STA) + START_SCAN accepted: **scan STARTED,
  visits all 37 channels at the 150 ms passive dwell, COMPLETED reason 0**; three scans, same result.
- But **0 MGMT_RX events: not a single beacon** in ~16 s of listening.
- Prime suspect: the WCN3950 RF rails are off. Stock DT (`~/work/topaz/backup/fdt.dts`, icnss@c800000):
  vdd-cx-mx = PM6125 **L8**, vdd-1.8-xo = **L16**, vdd-1.3-rfa = **L17**, vdd-3.3-ch0 = **L23**
  (BT also L9). They are RPM-SMD regulators (ldoa 8/16/17/23); downstream icnss votes them on when the
  WLFW service arrives, before the QMI handshake. Nobody does that here. SPMI addresses (mainline
  pm6125_regulators): Ln at 0x4000 + 0x100*(n-1); enable = reg 0x46 bit 7.
- v0.11 checks it read-only: SPMI observer reads of those rails (before the modem boots and after scan 1)
  + WMI CHAN_INFO per channel (noise floor, rx_clear, **rx_frame_count**) to tell "radio deaf" from
  "frames received but not forwarded".

## Rails: off at boot, on after the first scan (TopazModem v0.11, log docs/logs/TopazModem-v0.11.log)

SPMI observer reads (PM6125 LDOs are on SID 1; L8 apid 82, L9 83, L16 90, L17 91, L23 97):
- before the modem boots: L8 en 80 (ready), L9 en 80, **L16 / L17 / L23 en 00 (off)**;
- after scan 1: **L16, L17, L23 all en 80, status 87 (ready)**, L23 vset changed (e8/0c -> 80/0c).
So somebody (the modem / WLAN firmware via RPM) switches the RF rails on; apps does not have to.
Still 0 beacons. Only one CHAN_INFO with the COMPLETE flag arrived and it was all zeros (layout or
flag guess wrong). v0.12: rails at each step (FW_READY, WLAN_MODE, WMI READY, after scans), raw CHAN_INFO
dumps, raw dumps of the unknown events (0x3a001, 0x401f), and WMI pdev stats (chan_nf, rx_frame,
rx_clear, cycle, phy_err + raw tx/rx counters) before and after the scans.

## The radio hears, nothing reaches the firmware (TopazModem v0.12, log docs/logs/TopazModem-v0.12.log)

- Rails: before boot L16/L17/L23 off; at FW_READY L23 on, rest off; **at WLAN_MODE all on** — the
  firmware switches its own RF rails on through RPM when it enters mission mode.
- WMI pdev stats (base = chan_nf, tx_frame, rx_frame, rx_clear, cycle, phy_err, tx_pwr):
  baseline rx_frame 0; after scan 1 **rx_frame 17.6 M** / cycle 680 M; after scan 3 **175 M / 2293 M**
  (~7.6 % of air time receiving), rx_clear grows, chan_nf -109. CHAN_INFO (tag 0x26, 88 B, one per
  channel with cmd_flags 0; the COMPLETE one at the end is empty): e.g. 2412 MHz nf -120,
  rx_frame_count 2.8 M of 17.7 M cycles. **The PHY receives frames.**
- But every firmware RX software counter in the pdev stats (status_rcvd, mpdus, ...) stays **0**:
  received PPDUs never reach the firmware's rx path. ath10k configures the host rx ring (HTT
  RX_RING_CFG) right after WMI READY, before any scan — we skipped HTT setup.
- Unknown events decoded: 0x3a001 = tag 0x261 regulatory info (alpha2 "na", regdomain 0x6c);
  0x401f = tag 0x318 + 1128-byte array of 0x01; 0x1d00a = WLAN_FREQ_AVOID.

v0.13: HTT setup like ath10k_htt_setup (64-bit): VERSION_REQ -> FRAG_DESC_BANK_CFG (1056 x 72 B) ->
RX_RING_CFG (256 x 2 KiB host buffers, wcn3990 rx_desc_v2 offsets in words: attention 1, frag_info 2,
mpdu_start 4, msdu_start 7, msdu_end 12, mpdu_end 26, ppdu_start 27, ppdu_end 37, hdr_status 74,
payload 90; computed by compiling ath10k rx_desc.h) -> AGGR_CFG v2 (64/3), all on CE4; then the scan.
RX_IN_ORD_PADDR_IND buffers are recycled and their 802.11 headers logged.

## FIRST SCAN RESULTS in Windows (TopazModem v0.13, 2026-10-01, log docs/logs/TopazModem-v0.13.log)

HTT setup was the missing piece:
- VERSION_REQ -> **HTT VERSION_CONF 3.96** within 1 ms; FRAG_DESC_BANK_CFG, RX_RING_CFG (256 x 2 KiB,
  255 posted) and AGGR_CFG all accepted on CE4 (eid 1).
- Passive scan (same WMI commands as v0.10) now delivers WMI MGMT_RX beacons: **scan 1: 13 BSS /
  18 beacons, after 3 scans 18 BSS / 54 beacons**, 2.4 GHz ch 1-11 and 5 GHz ch 48/64, RSSI -61..-95 dBm
  (SSIDs redacted in the committed log). pdev stats: phy_err now non-zero, rx path alive.
- No HTT RX_IN_ORD_PADDR_IND yet: management frames still come over WMI; the host rx ring will carry
  data frames once associated.
- Modem stays READY, no FATAL, credits/queue fine.

This completes P4 steps 1-4 of docs/HANDOFF_p4_htc_wmi.md (CE rings, HTC, WMI init, HTT setup) and the
precondition for step 5 ("scan results over WMI").

### Next
- **MAC address**: WMI READY reports 00:00:00:00:00:00; the host must choose it (stock: persist
  `wlan_mac.bin`; else a stable locally administered one). Now vdev 0 uses 02:54:4f:50:41:5a.
- **Association path** (still inside TopazModem, test-only): vdev start + peer create, auth/assoc
  frames (WMI mgmt tx), WMI install key, HTT TX (frag desc bank) + HTT RX in-order data, EAPOL.
- **Step 5, WiFiCx/NetAdapter miniport**: expose scan / connect / data to Windows; the OS supplicant
  does WPA2 (EAPOL over the data path), the driver installs keys. Plan several iterations.
- Replace 1 ms polling with the CE interrupts (SPI 0x166..0x171) before this becomes a real driver.
