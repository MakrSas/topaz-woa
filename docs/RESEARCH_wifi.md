# Wi-Fi on topaz under Windows — research log

Status: **WLAN firmware reports FW_READY**, all from UEFI (P1+P2+P3). Started 2026-10-01.
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

- [x] P0 Firmware — all of it is in the `modem_a` (NON-HLOS, FAT16, 4 KiB sectors) backup,
      extracted to `~/work/wifi/fw/image/` (`7z x modem_a.img`):
      - `modem.mdt` + `modem.b00..b29` (Hexagon ELF, 31 phdrs, loads at 0x4AB00000.., one segment at 0x51400000)
      - `wlanmdsp.mbn` 3.8 MB — **WLAN.HL.3.2.4**-01083.3-QCAHLSWMTPLZ (ath10k WCN3990 "HL3.x" family);
        the modem fetches it over QRTR TFTP (Linux: `tqftpserv`)
      - board data `bdwlan.bin` + `bdwlan.1xx/2xx/bxx` variants (picked by board id via WLFW QMI)
      - `modemr.jsn` / `modemuw.jsn` = pd-mapper service lists (root_pd, qmi instance 180: servreg, pdr, gps)
      - EFS backups for rmtfs: `modemst1`, `modemst2`, `fsg` (in `~/work/topaz/backup`)
- [x] P1 Spike in UEFI (RAM boot), `uefi/TopazOtgDxe/ModemPas.c`, menu "Modem test":
      **2026-10-01: TZ authenticated and started MPSS** (init_image 0/0, mem_setup 0/0,
      auth_and_reset 0/0). Facts: raw SMC64 SiP calls (0x420002xx) from UEFI are accepted,
      all PIL calls available; firmware read from modem_a (FAT, after ConnectController on all
      BlockIo); split firmware (hash segment = modem.b01, 8056 B); relocatable (mem_setup
      needed); long calls return QCOM_SCM_INTERRUPTED (1) and must be resumed with x0=1 and the
      returned x6 (init 2 resumes, auth 22). No RPM proxy votes were made.
      **2026-10-01 later: MPSS reaches READY.** SMEM v12 (global partition), ptable at
      0x461FF000, apps<->modem partition @0x460DD000. The modem creates its SMP2P item 435
      (modem->apps) 0.25 s after auth_and_reset but adds no entries until apps creates item 428
      (apps->modem): allocated under TCSR hwlock 3 (0x343000, block 0x340000 is not in the UEFI
      map -> gDS AddMemorySpace + UC), magic `$SMP` v1 features 1, kick = APCS 0x0F111008 bit 14.
      Then within the same 0.25 s: entries `smp2p`=0, `slave-kernel`=0x6 (READY + handover),
      stable for 20 s, no FATAL, wdog SPI 0x133 not pending. Bits (DTB): 0 fatal, 1 ready,
      2 handover, 3 stop-ack, 7 shutdown-ack. SMEM 421 reads "SFR Init: wdog or kernel error
      suspected." — that is the modem's placeholder written at init, not a crash.
      Still no RPM proxy votes and no rmtfs/pd-mapper — the modem does not need them to get READY.
- [x] P2 (UEFI spike, `Glink.c` + `ModemSvc.c`) **2026-10-01: WLFW service 0x45 up** (inst 1, node 0, port 0x48)
      ~1.7 s after the services are announced. What it took:
      - GLINK over SMEM: items 478 (4 ring indices: tx tail, tx head, rx tail, rx head),
        479 = our TX FIFO 16 KiB, 480 = modem TX FIFO (allocated by the modem); doorbell APCS
        0x0F111008 bit 12. VERSION 1 features 1 (intent reuse). The modem opens "IPCRTR" itself;
        answer OPEN_ACK + our OPEN, advertise intents. Modem intents 128..8320 B; packets that don't
        fit need RX_INTENT_REQ (it grants them).
      - QRTR v1, modem = node 0, we are node 1. HELLO both ways; the modem then announces only
        servreg-notif 0x42 (inst 0xB4 = 180) and ssctl 0x2B until the apps services exist.
      - We announce (NEW_SERVER, instance field = version | inst << 8):
        pd-mapper 0x40 v0x101, tftp 4096 v1, rmtfs 14 v1.
      - pd-mapper: modem asks GET_DOMAIN_LIST for "kernel/elf_loader", "wlan/fw", ... -> answer
        msm/modem/{root_pd,wlan_pd}, instance 180 (modemr.jsn / modemuw.jsn).
      - rmtfs: opens /boot/modem_fs1/fs2/fsc/fsg, ALLOC_BUFF -> 3 MiB buffer, handed to the modem with
        SCM MP_ASSIGN (HLOS -> HLOS+MSS_MSA+NAV RW). After the EFS reads the modem brings up ~40 services.
      - tftp: `/readonly/vendor/firmware_mnt/image/wlanmdsp.mbn` (served from modem_a \image\):
        stat (rsize 0 + tsize, then ERROR 9 "End of Transfer"), then the whole file in one session
        (rsize = size, blksize 7680, wsize 10). Later mcfg.tmp (absent), mbn_hw.dig, mbn_sw.dig,
        /readwrite/lctoem.tmp (absent, plus a dropped write).
      - **Gotcha 1:** QRTR flow control. The modem sets confirm_rx on ~every 5th packet and blocks
        after 10 unconfirmed packets to one port; our RESUME_TX must be the full 20-byte
        qrtr_ctrl_pkt (12 bytes was silently ignored -> every transfer stalled after 10 ACKs =
        100 blocks, then "User-PD grace timer expired for wlan_process").
      - **Gotcha 2:** console output on the 1080x2400 GOP is slow (scrolling); spamming lines
        made the PD miss its grace timer. Use the timer counter for timestamps.
- [ ] P2b Same in a Windows kernel driver `drivers/TopazModem` — plan and porting notes in `docs/HANDOFF_wifi_windows.md`.
- [x] P3 WLFW QMI handshake (`Wlfw.c`, Linux ath10k/qmi.c order) **2026-10-01: FW_READY received.**
      IND_REGISTER (fw_ready + msa_ready, client id 0x4b4e454c) -> HOST_CAP (daemon_support 0) ->
      MSA_INFO (0x51900000, 1 MiB) + SCM assign of the returned regions to MSS_MSA/WLAN(/WLAN_CE) ->
      MSA_READY -> CAP -> on MSA_READY_IND: BDF download (bdwlan.bNN / .NNN by board id, 6144-byte
      segments, data_len u16) -> CAL_REPORT (empty) -> FW_READY_IND. Modem stays up (no FATAL).
      Chip/board/fw version lines scrolled away: print them in the summary next time.
- [ ] P4 ath10k SNOC data path + WiFiCx miniport.

Meanwhile internet works over the USB hub (RNDIS tethering or USB-Ethernet, inbox drivers).

## 5. Graphics note
GPU acceleration (Adreno 610) would need a full WDDM KMD + D3D UMD; Qualcomm ships these only
for its Windows platforms (RPMh, own firmware). Not realistic; stay on BasicDisplay
(UEFI framebuffer). Possible small win later: panel brightness (AMOLED, DSI DCS command).
