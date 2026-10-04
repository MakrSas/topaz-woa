# Handoff: SIM / mobile data (P9), 2026-10-04

Full chronological log: `docs/P9_sim.md`. This file is the short version for the next session.

## Where it stands

- **SIM works:** t2 USIM (physical/logical slot 2) READY, modem REGISTERED on LTE 250-20, PS
  attached, no modem crash, Wi-Fi keeps working.
- **AP IPA bring-up works** (all in `drivers/TopazModem`, built into TopazWifi):
  - TopazRpm (separate driver) opens the RPM GLINK link and votes the IPA clock (100 MHz);
  - `ModemPas.c` `IpaFwLoad`: identity SMMU banks for IPA streams 0x140/0x141/0x142
    (`wlanprobe.c` `SmmuIpaMap`), ipa_fws (GSI firmware) via PAS 15 into 0x55B00000,
    SRAM canaries, GSI interrupt setup (INTSET = 1, masks 0), modem GSI channels 0..3 allocated
    with GENERIC_CMD (IPA v4.2 quirk);
  - `ModemSvc.c`: AP IPA QMI service 0x31:101 + INIT_DRIVER to the modem (0x31:201),
    INIT_COMPLETE indication. Modem then sends IPA QMI 0x27 and 0x23 (acked generically).
- **Data call (`wwan.c`):** DPM OPEN_PORT (hardware data port EMBEDDED/1, rx endpoint 9,
  tx endpoint 1) -> 0, WDA SET_DATA_FORMAT raw-IP without aggregation -> 0,
  WDS BIND_MUX_DATA_PORT (EMBEDDED/1, mux 1) -> 0, **WDS START_NETWORK (APN internet.tele2.ru,
  IPv4) is sent and never answered** (> 2 min, modem otherwise alive).

## Next step (not started - `drivers/TopazModem/ipa.c` does not exist yet)

Working theory: START_NETWORK waits for the AP side of the IPA data path announced through DPM.
Set up, polled, in EE 0 (GSI v4.0 register map, see `docs/P9_sim.md` and Linux
`drivers/net/ipa` as the reference for register semantics only - write own code, the Linux IPA
driver is GPL-2.0):

1. Event ring per channel (GPI type, 16-byte elements, ring aligned to its size), EV ALLOCATE,
   doorbell.
2. GSI channel 0 = AP_MODEM_TX (endpoint 1, toward IPA, 512 TREs) and channel 3 = AP_MODEM_RX
   (endpoint 9, from IPA, 256 TREs): program CNTXT_0..3, QOS, GPI scratch, CH ALLOCATE, CH START;
   completion = state field change.
3. RX buffers posted on channel 3 (data/ipa_data-v4.2.c: buffer 8192).
4. Then retry START_NETWORK; if it answers, GET_CURRENT_SETTINGS logs the address.
5. Later: IPA endpoint config (ENDP_INIT_*), hardware config, table init, QMAP framing, then
   a Windows adapter (MBBCx "Cellular", plain NetAdapter as fallback).

Linux reference files are copied on the Mac in this session's scratchpad (`ipa/`) and on s8build
in `~/work/topaz/linux/drivers/net/ipa`.

## Phone state / how to run

- Running: TopazWifi v0.25 (`C:\Windows\System32\drivers\TopazWifi.sys`, service ImagePath points
  there), TopazRpm v0.6 (same scheme). Older builds kept as `TopazWifi.sys.vNN`.
- Flags: `C:\topaz\rpm.on` (TopazRpm link + IPA clock), `C:\topaz\fw\ipa.on` (IPA path),
  `C:\topaz\fw\sim.on` (USIM provisioning), `C:\topaz\fw\data.on` (DPM / WDA / WDS data call).
  `C:\topaz\fw\gsi.off` skips the modem GSI channel allocation (do not: the modem then crashes
  once the SIM is active).
- Deploy without killing Wi-Fi (SSH runs over it): copy the CI-built .sys next to the old one
  (rename the old one first, a loaded driver file can be renamed but not overwritten), add that CI
  run's `topaz-woa-test.cer` to Root + TrustedPublisher (new cert every run), reboot.
- **Never restart TopazRpm on a running system** (a second handshake crashed the RPM = SoC reset);
  update it only by file swap + reboot.
- After any hang/reset: `C:\topaz\gpuw.guard` (makes the GPU boot hang on the logo) and
  `C:\topaz\wifi.boot` (TopazWifi refuses to start) must be deleted; the user deletes files by
  hand, just name them.
- Logs: `C:\TopazWifi.log` (search `==== TopazWifi vNN`, `wwan:`, `ipa:`, `ipa-qmi:`),
  `C:\TopazRpm.log`.
