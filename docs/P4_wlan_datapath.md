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
