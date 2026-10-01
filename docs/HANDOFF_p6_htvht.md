# Handoff: P6 — HT/VHT (802.11n/ac) for TopazWifi (2026-10-01)

## State
- **Wi-Fi works in Windows** with TopazWifi v0.4 (branch `display`, commit fd2b4b0 and before):
  scan list in Settings, WPA2-PSK connect from Settings, DHCP, internet. Yandex speed test on the
  phone: **23.7 Mb/s down, 13.2 up, 35 ms** = the ceiling of the current *legacy* (11a/g, 54 Mb/s)
  association. Goal now: HT20 on 2.4 GHz, HT/VHT on 5 GHz (home AP `WiFi 2.4` is also on 5 GHz ch 64).
- Background: `docs/P4_wlan_datapath.md` (CE/HTC/WMI/HTT bring-up), `docs/P5_connect.md` (WMI/WDI
  ids, association + data path reference). ath10k sources: s8build `~/work/wifi/linux/drivers/net/
  wireless/ath/ath10k`. WDK WiFiCx/WDI headers are not in the repo; to read them again, push a
  one-off workflow that uploads `Include\10.0.26100.0\km\wificx`, `km\wlan\2.0`, `shared\netcx`.

## Code map
- `drivers/TopazWifi/` (C++, WiFiCx 1.0 + NetAdapterCx 2.2 + KMDF 1.33, built by CI):
  `wdrv.cpp` driver/device, modem thread (boots the modem on every start unless
  `C:\topaz\modem.off`), boot guard `C:\topaz\wifi.boot` (20 s); `wcaps.cpp` capabilities (PHY list
  already advertises HT/VHT); `wcmd.cpp` WDI dispatcher, scan; `wconn.cpp` TASK_CONNECT/DISCONNECT,
  association result (ActivePhyTypeList is ERP/OFDM now), link state (54000 kb/s now), keys;
  `wnet.cpp` NetAdapter + data path; `wmem.cpp` TLV allocators.
- WLAN core in `drivers/TopazModem/` compiled with `TOPAZ_WIFICX` (`wlanif.h` = C/C++ interface):
  `assoc.c` association state machine + WMI commands (VDEV_START, PEER_CREATE, MGMT_TX_SEND,
  PEER_ASSOC, VDEV_UP, INSTALL_KEY...), `htt.c` HTT setup + data TX/RX, `scan.c`, `wmi.c`, `htc.c`,
  `ce.c`, `Glink.c` (the 1 ms polling loop calls ScanPoll/HttPoll/AssocPoll/HttTxPoll).

## Build / test loop
- Push to `display` -> CI `Build drivers` -> artifact `TopazWifi-arm64`
  (`gh run download <id> -n TopazWifi-arm64`). Flash drive (exFAT/NTFS "Образы", may need
  `diskutil mount disk4s1`): `/Volumes/Образы/topaz-woa/TopazWifi/` gets the .sys/.inf/.cat/.cer +
  `tools/deploy/install-wifi.cmd` as `install.cmd` and `copy-log-wifi.cmd` as `copy-log.cmd`.
- User: `install.cmd`, wait 30 s, reboot, connect, `copy-log.cmd` -> `TopazWifi.log` on the flash.
  Log = `C:\TopazWifi.log` (append; find the last `==== TopazWifi vX ====`). Photos come as HEIC:
  `sips -s format jpeg`. Redact SSIDs before committing logs (they are logged by scan.c).
- Bump `TOPAZ_WIFI_VERSION` in `drivers/TopazWifi/wpch.h` each build. Never mention a UEFI
  "Modem test" item to the user (their UEFI has none).

## HT/VHT plan (ath10k mac.c: peer_assoc_h_ht 2325, h_vht 2575, h_qos 2683, phymode 2723)
1. **Assoc request** (`assoc.c` BuildAssocReq): add the HT Capabilities IE (45) when the AP's beacon
   has HT (IE 45): 1x1, HT20 (+SGI20; 40 MHz only if we also do HT40), A-MPDU params, MCS 0-7 rx
   mask; the WMM information element (vendor 221: 00 50 F2 02 00 01 qos-info 00) when the AP has WMM
   (IE 221 with 00 50 F2 02) — HT requires QoS. 5 GHz + AP VHT (IE 191): VHT Capabilities IE
   (1x1, MCS 0-9, 80 MHz if used). Keep the capability bits sane (short slot etc.).
2. **PEER_ASSOC** (`SendPeerAssoc`): flags |= QOS 0x2, HT 0x1000 (+40MHZ 0x2000), VHT 0x2000000
   (+80MHZ 0x4000000); ht_caps = negotiated HT cap info; max_mpdu / mpdu_density from the AP's
   A-MPDU params; rate_caps WMI_RC_HT_FLAG 0x08 (| SGI 0x04, CW40 0x02); HT rates array = MCS 0-7
   (intersection with the AP's rx MCS mask); VHT rate set {rx_max_rate, rx_mcs_set, tx_max_rate,
   tx_mcs_set}; nss 1; phy_mode MODE_11NG_HT20 5 / 11NA_HT20 4 / 11NA_HT40 6 / 11AC_VHT20 8 /
   VHT40 9 / VHT80 10 / 11AC_VHT20_2G 11.
3. **VDEV_START channel** (`PutChannel`): mode = the same phy mode, band_center_freq1 = the 40/80 MHz
   center for wider channels, CHAN_FLAG_ALLOW_HT/VHT.
4. **QoS TX**: with a QoS peer the firmware wants a TID: TX_FRM flags1 ext_tid 0 (best effort)
   instead of 16 (non-QoS) for unicast data; broadcast/multicast keep 16. Native-wifi frames from
   Windows carry no QoS field, the firmware adds it.
5. **Windows side** (`wconn.cpp`): ActivePhyTypeList HT/VHT, LINK_STATE_CHANGE speeds
   (72200 / 150000 / 433300 kb/s), WMMQoSEnabled TRUE in the association result.
6. Then (P7): CE interrupts (SPI 0x166..0x171) instead of 1 ms polling, real MAC from persist,
   WDI_GET_STATISTICS (34) for signal/rate in Windows, roaming / AP loss, WPA3.
