# P9: SIM / mobile data (no calls)

Plan (docs/ROADMAP.md §2): stage 1 QMI clients (log only) → stage 2 data path (WDS data call +
IPA v4.2 / GSI) → stage 3 Windows network adapter (raw-IP NetAdapter, later MBBCx).

## Stage 1: QMI DMS / UIM / NAS clients (TopazWifi v0.5, 2026-10-03)

- `drivers/TopazModem/wwan.c`, built into TopazWifi (which owns the modem: PAS boot, GLINK/QRTR,
  rmtfs/tftp/pd-mapper). Our QMI client ports 0x4010 (DMS), 0x4011 (NAS), 0x4012 (UIM).
- On QRTR NEW_SERVER: DMS GET_IDS (IMEI) + GET_REVISION; then every 10 s DMS GET_OPERATING_MODE
  (sends SET_OPERATING_MODE online once if the modem is not online), UIM GET_CARD_STATUS,
  NAS GET_SERVING_SYSTEM + GET_SIGNAL_INFO. Logged to C:\TopazWifi.log on change; the 30 s
  summary has a `wwan:` line. Message ids / TLVs from libqmi qmi-service-{dms,uim,nas}.json.
- Install without dropping Wi-Fi (SSH runs over it): the new TopazWifi.sys is copied to
  `C:\Windows\System32\drivers\` and the service ImagePath points there (was the DriverStore
  copy `topazwifi.inf_arm64_e38bc04a85bb22a4`); loads on the next boot. Each CI run signs with a
  new test cert: add its topaz-woa-test.cer to Root + TrustedPublisher first (else sig UnknownError).
  Rollback: set `HKLM\SYSTEM\CurrentControlSet\Services\TopazWifi\ImagePath` back to
  `\SystemRoot\System32\DriverStore\FileRepository\topazwifi.inf_arm64_e38bc04a85bb22a4\TopazWifi.sys`.
- v0.5 result: IMEI 868773065134622, MPSS.HA.1.1.c1-00084 (Nov 2023); modem comes up in mode 5
  (shutting-down), SET_OPERATING_MODE online answers error 52 but the mode is online next poll;
  NAS camps on 250-01 UMTS (emergency only).
- v0.6 result: card is in **physical/logical slot 2** (slot 1 empty = "no ATR"), USIM app
  "detected", ICCID 89701204145300976051 (T2 Russia); index_gw_primary = 0xFFFF = no provisioned
  subscription (Android's RIL does that) -> v0.7 sends UIM CHANGE_PROVISIONING_SESSION
  (primary GW, activate, slot + AID of the USIM).
- v0.7 result: provisioning ok -> index_gw_primary 0x0001, USIM **READY** (PIN disabled, 3/10
  retries), NAS "not registered"; then at t=31.9 s **modem FATAL**: `dog_hb.c:367: Task
  starvation: modem_cfg`. Before it the modem wrote /readwrite/mcfg.tmp and lctoem.tmp over TFTP
  (we acked and dropped them, the read-back said "not found") and, at t=10.3 s after the SIM
  appeared, read mbn_hw.dig / mbn_sw.dig and four mcfg_sw.mbn (SIM-specific carrier config).
  Modem down = Wi-Fi down (same modem).
- v0.8: /readwrite/* files kept in RAM (WRQ stores, RRQ serves, like tqftpserv on disk), rmtfs
  writes logged with time, up to 40 unhandled QRTR packets logged.
- v0.8 result: /readwrite in RAM works (mcfg.tmp / lctoem.tmp written and read back; the second
  "WRQ ... w 1" per file sends no data and closes the client = probably a truncate), but the
  crash is identical: USIM READY ~20 s, NAS goes "not registered, radio none" (modem_cfg
  switching the carrier config), 47 services instead of 45, FATAL modem_cfg starvation at 31.9 s.
  No TFTP / rmtfs / intent traffic between 10.7 s and the crash; GLINK intents fine. The
  "unhandled QRTR" log budget had been eaten by DEL_CLIENT packets.
- v0.9: provisioning only with C:\topaz\fw\sim.on (Wi-Fi stays usable by default); logs late
  NEW_SERVERs, all QRTR control packets except DEL_CLIENT and up to 200 unhandled DATA packets.
- v0.9 run (sim.on): modem found **t2 (250-20)** this time; after provisioning two late services
  (svc 0x21, 0x12) appear; no unanswered QRTR / TFTP / rmtfs traffic before the identical
  modem_cfg starvation. Remaining suspect: SMP2P. The modem's "ipa" entry raises an IPA power
  query and "polls the valid bit until it is set" (Linux ipa_smp2p.c) - nobody answers it.
- v0.10: outbound SMP2P entry "ipa" (bit0 valid, bit1 power) created before the modem boots;
  the GLINK loop watches the modem's "ipa" entry (bit0 query, bit1 GSI setup ready), logs it and
  answers queries with "valid, power off".
- v0.10 result: the modem never creates an "ipa" SMP2P entry (its item 435 has only "smp2p" and
  "slave-kernel"), so no power query; crash identical. During the crash run Wi-Fi had already
  dropped ("WLFW: disconnected" at 30 s).
- **Android reference (s8build ~/work/topaz/gsi_dmesg_full.txt, GSI on this phone):** the AP
  loads the IPA GSI firmware `ipa_fws.mdt/.b01-.b04` (PAS) and finishes IPA init at 10.7 s
  ("IPA driver is now in ready state", rmnet_ipa3), and only then boots the modem (19.4 s);
  ipa-wan handles MPSS SSR notifications (BEFORE_POWERUP ...). Linux mainline ipa_qmi.c: the AP
  hosts its own IPA QMI service (svc 0x31, inst 0x101) and is a client of the modem's (svc 0x31
  inst 0x201 - present in our service list); INIT_DRIVER / DRIVER_INIT_COMPLETE handshake.
  Working theory: once the SIM is provisioned the modem's data services start, need the AP-side
  IPA (firmware + QMI handshake) and spin -> modem_cfg starvation. So stage 2 (IPA bring-up) is
  needed even for a stable registered SIM, not only for data.

## Stage 2: IPA

Plan: (1) probe - AP-side IPA QMI service, no hardware access; (2) ipa_fws firmware from the
vendor partition (read-only); (3) RPM vote for the IPA clock (DT: clocks = <rpmcc-khaje 0x44>,
an RPM SMD clock - touching IPA registers without it hangs the bus); (4) IPA + GSI init after
Linux drivers/net/ipa (v4.2), then the data path.
DT: `qcom,ipa@0x5800000` reg ipa 0x5800000+0x34000, gsi 0x5804000+0x28000, irqs SPI 0x101 / 0x103,
pas-ids 0x0f, firmware ipa_fws, memory region ipa_fw_region 0x55b00000 (64 KiB) + ipa_gsi_region
0x55b10000 (20 KiB), ee 0, SMMU streams 0x140 (ap), 0x141 (wlan), 0x142 (uc).

- v0.11 (probe): with C:\topaz\fw\ipa.on the driver announces the AP IPA QMI service 0x31:101
  (ipa_qmi.c IPA_HOST_SERVICE) and answers every modem request with success, logging msg id + TLVs.
- ipa_fws firmware: vendor_a (EROFS) read-only from super (PhysicalDrive0 p9, LP metadata:
  vendor_a = 691810304 B at super+6343884800), extracted on s8build `~/work/vendor_a/firmware/`:
  ipa_fws.mdt (7020) + .b00..b04 + .elf (29120); copied to C:\topaz\fw\image\.
- v0.11 result (sim.on + ipa.on): the modem connects to our AP IPA service at t=0.32 s and sends
  INDICATION_REGISTER (0x20) TLV 0x10 = 1 (wants the "AP driver init complete" indication);
  answered ok, no INIT_DRIVER / INIT_COMPLETE from us (nothing to report), crash unchanged.
  Confirms the modem depends on the AP IPA driver. Next: RPM client (IPA clock vote; also the
  pending GPU voltage vote), ipa_fws PAS load (id 15), IPA v4.2 + GSI init, then INIT_DRIVER.

### RPM client (drivers/TopazRpm)

Needed for the IPA clock (DT: IPA `clocks = <rpmcc-khaje 0x44>`), later also the GPU rails.
Stock DT: `rpm-glink` (qcom,glink-rpm) over `memory@045f0000` (rpm-msg-ram, 0x7000 B), RPM irq
SPI 0xC2, doorbell APCS IPC bit 0, channel `rpm_requests` (qcom,rpm-smd). Linux
qcom_glink_rpm.c: TOC (256 B) at the end of the msg RAM, magic "grt0", FIFOs "ap2r"/"r2ap" as
{tail, head, data}; GLINK native, intentless.
- TopazRpm v0.1: root KMDF driver, read only: TOC, FIFO indices and pending bytes -> C:\TopazRpm.log.
  Question it answers: did UEFI leave the RPM GLINK link / rpm_requests channel open?
- TopazRpm v0.1 result: TOC grt0 with 10 FIFOs (ap2r/r2ap + mp2r/r2mp, ad2r/r2ad, cD2r/r2cD,
  tz2r/r2tz); ap2r at 0x200, r2ap at 0x900, both 0x6f8 bytes, tail = head = 0: unused, clean link.
- TopazRpm v0.2 (C:\topaz\rpm.on): VERSION, OPEN rpm_requests, IPA clock 100 MHz active-set vote
  (resource "ipa" 0x617069 id 0, key "KHz"), logs the msg# ack / err string.
- TopazRpm v0.2 result: VERSION 1 / VERSION_ACK 1 (features 0), RPM opens "rpm_requests" (rcid 3)
  and "glink_ssr" (rcid 4), our OPEN acked, **IPA clock 100 MHz acked (msg# 1)**. Same channel can
  carry the GPU rail votes later.
- TopazRpm v0.3 (C:\topaz\ipa.probe): first read of IPA/GSI registers after the vote.
- **TopazRpm v0.3 install = whole SoC down**: devcon update restarted the driver, the second
  VERSION on the already-live link got a VERSION_ACK only, then the system died (RPM crash) before
  the IPA probe; after the reboot the store still had v0.2 (update never completed). Rule: TopazRpm
  is updated only by file swap + reboot. v0.4 refuses to handshake when the FIFO indices show
  earlier traffic.
- **TopazRpm v0.5 result (fresh boot, one-shot ipa.probe): IPA is alive after the RPM vote.**
  IPA COMP_CFG 0x5840000+0x3c = 000f0078, SHARED_MEM_SIZE +0x54 = 00000415 (0x415 x 8 B = 8360 B
  SRAM, base 0), FLAVOR_0 +0x210 = 08090811 (8 consumer + 9 producer pipes, producers from 8),
  GSI STATUS (EE0) 0x5823000 = 0 (not enabled: no GSI firmware yet), GSI HW_PARAM_2 0x5823040 =
  e4046b59. Register layout confirmed (ipa-reg 0x5840000, gsi 0x5804000).
- Next: ipa_fws PAS load (id 15, memory region 0x55b00000) before the modem boots -> GSI enabled;
  then IPA/GSI init after Linux drivers/net/ipa (v4.2 data), the QMI INIT_DRIVER to the modem,
  INIT_COMPLETE indication. Boot order: TopazRpm vote -> IPA init -> modem.
- TopazRpm v0.6: after the acked IPA vote writes `IpaClockKhz` to the volatile key
  `HKLM\SYSTEM\CurrentControlSet\Services\TopazRpm\State` (gone after reboot; a named event
  would be created signaled by whichever driver comes first).
- TopazWifi v0.12 (C:\topaz\fw\ipa.on): before the modem, waits up to 15 s for that value, then
  loads ipa_fws like Linux ipa_firmware_load(): init_image(15), mem_setup(15, 0x55B00000, size),
  relocated segments into ipa_fw_region (inside our "PIL Reserved"), auth_and_reset(15), logs
  GSI_STATUS (bit 0 = enabled).
