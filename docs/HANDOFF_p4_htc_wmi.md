# Handoff: P4 WLAN data path (CE rings → HTC → HTT/WMI) in TopazModem (2026-10-01)

You are continuing the Windows 11 ARM64 port for the Redmi Note 12 4G (topaz/tapas, SM6225 "khaje").
Read `docs/P4_wlan_datapath.md`, `docs/RESEARCH_wifi.md`, and `docs/AGENT_BRIEF_drivers.md` first.
Everything up to and including the hard SMMU problem is **done and verified on the phone**; your job
is the copy-engine data path on top.

## What already works (TopazModem v0.7, driver `drivers/TopazModem`, branch `display`)
The driver boots the modem (TrustZone PAS), serves pd-mapper/tftp/rmtfs, and the WLAN firmware
reaches **FW_READY**; then it sends QMI **WLAN_CFG** + **WLAN_MODE=mission** (both OK) and opens an
**identity SMMU context bank** for the WLAN stream so the hardware can reach host memory. Verified log:
`docs/logs/TopazModem-v0.7.log`. Key facts for you:
- WCN3990, target_64bit + shadow_reg_support + rri_on_ddr. chip_id 0x4130, board bdwlan.bin.
- CE register block: physical `0xC800000 + 0x240000`, CE n at `+0x1000*n`; wrapper at `+0xC000`.
  Offsets (ath10k wcn3990_ce_regs): sr_base lo/hi 0x0/0x4, sr_size 0x8, dr_base lo/hi 0xC/0x10,
  dr_size 0x14, misc_ie 0x34, sr_wr_idx 0x3C, dr_wr_idx 0x40, cur_srri 0x44, cur_drri 0x48,
  ctrl1 0x18 (dmax[15:0], src bswap bit17, dst bswap bit18, rri-upd bit19 mask 0x80000).
  RRI base: wrapper +0x4 (lo) / +0x8 (hi) i.e. phys 0x24C004/0x24C008.
- Shadow source-ring write index (because shadow_reg_support): membase `0xC800000 + 0x32000 + 4*ce`
  for CE 0,3,4,5,7; dest-ring write index stays at CE+0x40.
- After WLAN_MODE the firmware pre-fills CE ring base regs with MSA addresses (0x519xxxxx) — ignore
  them; the host reallocates rings in host memory (now reachable through the identity CB).
- **SMMU is identity**: a DMA buffer handed to the hardware is reachable at its physical address.
  Use `PhysAlloc()` (port.c) for all rings/buffers: contiguous, <4 GiB, write-combined, phys==what
  the hardware sees. Keep everything below 0xF0000000 (the modem's 32-bit end-address wrap, see v0.2).

## The job, in order (all from Linux ath10k; sparse clone on s8build `~/work/wifi/linux`)
Work in small files/edits (a safety classifier truncates long code blocks — proven twice). Add new
files `ce.c`/`htc.c` (or extend `wlanprobe.c`) and call them after the `WLAN ON (mission)` step in
`Wlfw.c` (where `CeProbe()` runs today).

1. **CE rings** (ath10k `ce.c` ath10k_ce_init_src_ring/init_dest_ring, alloc_rri):
   - Allocate RRI (12×u32) with PhysAlloc; write phys to wrapper +0x4/+0x8; set CTRL1 bit19 on each CE.
   - For each CE in `host_ce_config_wlan` (see P4 doc / snoc.c): alloc src and/or dest ring of
     `ce_desc_64` {u64 addr, u16 nbytes, u16 flags, u32 toeplitz} (16 B), size = roundup_pow2(nentries),
     CE_DESC_RING_ALIGN. Write base (0x0/0x4 src, 0xC/0x10 dst), size (0x8/0x14), dmax, clear bswap,
     low/high watermarks. Host layout: CE0 src16, CE1 dst512, CE2 dst64, CE3 src32, CE4 src2048,
     CE5 dst512, CE7 src2/dst2, CE8 dst128, CE9/10/11 dst512. src_sz_max 2048 (CE4 256, CE5 512).
   - Post RX buffers (2048 B each, PhysAlloc) on every dest ring: write desc.addr, bump dest write idx.
2. **HTC** (ath10k `htc.c`): the control protocol over CE0(tx)/CE1(rx).
   - Header `ath10k_htc_hdr` (8 B): eid, flags, le16 len, trailer/seq, pad. Messages `ath10k_htc_msg`.
   - Boot: firmware sends **HTC READY** (msg id 1, `ath10k_htc_ready`: credit_count, credit_size,
     max_endpoints) on CE1. Then for each service connect: send **CONNECT_SERVICE** (id 2,
     `ath10k_htc_conn_svc`: service_id, flags) on CE0, read **CONNECT_SERVICE_RESP** (id 3: service_id,
     status, eid, max_msg_size) on CE1. Connect WMI_CONTROL (0x8001→service id via ATH10K svc ids),
     then send **SETUP_COMPLETE** (id 4/5). Service ids in `htc.h` (SVC(group,idx)); eid→CE pipe map
     is from the WLAN_CFG svc table already sent (WMI_CONTROL out3/in2, HTT_DATA out4/in1, etc.).
   - First milestone: **log "HTC READY" with credit_count/credit_size**. That proves the whole data
     path (SMMU CB + rings + CE send/recv) works end-to-end. Commit at that point.
3. **WMI** (ath10k `wmi-tlv.c`, this fw is TLV): after HTC setup the firmware sends a WMI **service
   ready** / **ready** event on the WMI_CONTROL endpoint. Send WMI **init**. Milestone: log the WMI
   ready event (MAC addr, abi version).
4. **HTT** (`htt.c`,`htt_tx.c`,`htt_rx.c`): setup for the data path (RX ring in host memory, TX).
5. **WiFiCx/NetAdapter miniport**: only after 1–4 produce scan results over WMI. This is a second
   driver (or a second device) exposing NetAdapterCx; map WMI scan/connect + HTT RX/TX to Windows.
   This is the longest part — plan several iterations.

## Build / deploy / test loop (you cannot touch the phone directly)
- CI builds on push to `display`: `.github/workflows/build.yml` already includes TopazModem. After
  push, `gh run watch <id>`, then `gh run download <id> -n TopazModem-arm64`.
- The **user** runs it on the phone. Deploy by copying the artifact + firmware to the exFAT flash at
  `/Volumes/Образы/topaz-woa/TopazModem/` with `tools/deploy/install-modem.cmd` as `install.cmd`
  (firmware tree is staged on s8build `~/work/wifi/winfw/fw`, already on the flash). The user runs
  `install.cmd`, then `copy-log.cmd`, and brings back `C:\TopazModem.log`. Ask the user to do this and
  wait for the log; do not claim anything works until the log shows it.
- Bump `TOPAZ_MODEM_VERSION` in `driver.c` each build; save each returned log to `docs/logs/`.

## Gotchas (paid for already)
- Read CE registers only AFTER WLAN_MODE=mission; earlier hangs the bus → silent reboot (v0.4).
- `KeQueryInterruptTimePrecise` needs a non-NULL out param (v0.1 bugcheck).
- Keep TZ/hardware buffers < 0xF0000000 (v0.2 "xpu lock failed").
- Log is buffered/lazy during the GLINK loop; call `LogSetLazy(FALSE)` (or it flushes on exit) before
  a risky hardware poke so the last line survives a reboot.
- One agent drives the phone at a time; coordinate with the user. Never flash partitions / touch GPT.
- `MmioRead32/Write32` = READ/WRITE_REGISTER_ULONG; use `MapPhys(pa,size,FALSE)` for CE/SMMU MMIO
  and `MapPhys(...,TRUE)` or `PhysAlloc` for memory the hardware reads.
