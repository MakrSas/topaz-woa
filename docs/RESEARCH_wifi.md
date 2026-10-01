# Wi-Fi on topaz under Windows — research log

Status: **research**, nothing runs yet. Started 2026-10-01.
Source of facts: the phone's DTB (`~/work/topaz/backup/fdt.dts`), Linux upstream/MiCode drivers.

## 1. What the hardware is

Wi-Fi is **WCN3990** in "integrated" mode (`qcom,icnss`): the radio is the WCN3990 RF chip,
but the WLAN **firmware runs on the modem DSP (MPSS)** and the host talks to it through copy
engines in a SoC block. No PCIe/SDIO — nothing a stock Windows driver can enumerate.

| Piece | DTB facts |
|---|---|
| WLAN host block (icnss) | `qcom,icnss@C800000`: membase 0xC800000 len 0x800000 (copy engines), `smmu_iova_ipa` 0xB0000000 len 0x10000 |
| WLAN interrupts | 12 copy-engine IRQs SPI 0x166..0x171 (GSIV 0x186..0x191), level |
| WLAN SMMU | `iommus = <apps_smmu 0x1a0 0x1>`, IOVA pool 0xA0000000 + 256 MiB, faults "stall-disable HUPCF" |
| WLAN MSA memory | `wlan_msa_region@51900000`, 1 MiB, no-map (firmware's private RAM) |
| WLAN supplies | vdd-3.3-ch0 (2.8-3.3 V), vdd-1.3-rfa, vdd-1.8-xo, vdd-cx-mx (640 mV) — RPM regulators |
| WLAN crash/early-crash | smp2p `wlan_1_in` (force-fatal, early-crash-ind) |
| Modem (runs WLAN fw) | `remoteproc-mss@6080000` `qcom,khaje-modem-pas` (TrustZone PAS loader), carveout `modem_region@4ab00000` 0x6900000 |
| Modem IRQs | wdog SPI 0x133; fatal/ready/handover/stop-ack/shutdown-ack via smp2p (phandle 0xdb) |
| Modem IPC | GLINK over SMEM edge "mpss" (remote-pid 1), channels **IPCRTR** (QRTR), DS, fastrpc |
| SMEM | `smem_region@46000000` 2 MiB |
| RPM | **GLINK-RPM** (`qcom,glink-rpm`) — rails/clocks are voted through RPM, not RPMh |
| Bluetooth | same WCN3990, UART + `qcom,wcn3990` node (bt-sw-ctrl GPIO 87) |

## 2. What Linux needs to bring it up (= what Windows would need)

1. **RPM votes** (GLINK-RPM over SMEM): CX/MX corners, XO, WLAN rails (3.3/1.3/1.8).
2. **MPSS boot** via TrustZone PAS: load `modem.mdt` + `modem.bNN` segments into 0x4AB00000,
   SCM calls `pas_init_image` / `pas_mem_setup` / `pas_auth_and_reset`
   (Linux `qcom_q6v5_pas.c`, `qcom_scm.c`), wait for smp2p "ready".
3. **Host services the modem expects** (Linux userspace daemons):
   `rmtfs` (modem EFS from `modemst1/2`, `fsg`), `pd-mapper` (protection-domain map),
   `tqftpserv` (firmware files over QRTR). Without them the modem may stall/crash.
4. **SMEM + GLINK (SMEM transport) + QRTR (IPC router) + QMI** client.
5. **WLFW QMI service** on the modem (Linux `ath10k/qmi.c`): send board data (`bdwlan.*`),
   calibration, set mode "mission".
6. **WLAN data path**: ath10k **SNOC** (`ath10k/snoc.c`): copy engines at 0xC800000, HTT/WMI
   messages, DMA buffers through the **apps SMMU** stream 0x1A0.
7. **Windows network driver** on top: WiFiCx/NetAdapter (or WDI) miniport mapping
   scan/connect/data to ath10k WMI/HTT (802.11 MLME is mostly offloaded to the firmware).

Linux references: `drivers/remoteproc/qcom_q6v5_pas.c`, `drivers/firmware/qcom_scm.c`,
`drivers/soc/qcom/smem.c`, `drivers/rpmsg/qcom_glink_smem.c`, `net/qrtr/`,
`drivers/net/wireless/ath/ath10k/{snoc,qmi,ce,htt,wmi-tlv}.c`, userspace `rmtfs`,
`pd-mapper`, `tqftpserv` (github.com/linux-msm).

## 3. Hard problems / unknowns

- **SMMU**: Windows has no Qualcomm SMMU driver here; DMA from the WLAN block goes through
  stream 0x1A0. Need to know whether UEFI/TZ leaves it bypassed or faulting.
- **RPM over GLINK** from Windows: a small GLINK-RPM client is needed for rail/clock votes,
  unless UEFI already leaves everything on.
- **SMC from a Windows kernel driver**: MSVC has no inline asm on ARM64; an `.asm` stub with
  `SMC #0` (armasm64) should work since there is no hypervisor between Windows and TZ.
- **Modem host services** (rmtfs/pd-mapper/tqftpserv) must be reimplemented as Windows drivers/services.
- Effort estimate: several weeks for "modem up + WLFW ready", more for a usable WiFiCx driver.

## 4. Plan

- [ ] P0 Collect firmware from Android: `/vendor/firmware_mnt/image/modem.*`, `wlanmdsp.mbn`,
      `bdwlan*`, `/vendor/etc/wifi/*`, plus the `modem`, `modemst1/2`, `fsg` partitions.
- [ ] P1 Spike in UEFI (cheapest place to experiment, RAM boot): Qualcomm `ScmDxe` protocol
      (`EFIScm.h`) for PAS calls, load MPSS, watch smp2p "ready" / IPCRTR HELLO in SMEM.
- [ ] P2 Same in a Windows kernel driver (SMEM, GLINK-SMEM, QRTR, rmtfs/pd-mapper equivalents).
- [ ] P3 WLFW QMI handshake (board data, mode on) — first proof the radio is alive.
- [ ] P4 ath10k SNOC data path + WiFiCx miniport.

Meanwhile internet works over the USB hub (RNDIS tethering or USB-Ethernet, inbox drivers).

## 5. Graphics note
GPU acceleration (Adreno 610) would need a full WDDM KMD + D3D UMD; Qualcomm ships these only
for its Windows platforms (RPMh, own firmware). Not realistic; stay on BasicDisplay
(UEFI framebuffer). Possible small win later: panel brightness (AMOLED, DSI DCS command).
