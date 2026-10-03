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
