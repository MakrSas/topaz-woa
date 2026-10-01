# P5: connect (association, keys, data path) in TopazWifi — reference + plan

State (2026-10-01): TopazWifi v0.2 shows the real networks in Windows (WiFiCx stage A). Connecting is
next. Everything below is from Linux ath10k (wmi-tlv.c/h, wmi.h, mac.c, htt*.c; sparse clone on s8build
`~/work/wifi/linux`) and the WDK wificx sample / WDI TLV model (`WiFiCxModel.xml`, `TlvGenerated_.hpp`).

## WDI side (what Windows sends / expects)
- `WDI_TASK_CONNECT` (6): `ConnectParameters` (SSIDList, AuthenticationAlgorithms, Unicast/Multicast
  CipherAlgorithms, optional AssociationRequestVendorIE, ...) + `PreferredBSSEntryList` (BSSID,
  ProbeResponseFrame/BeaconFrame body, SignalInfo, ChannelInfo). M3 at once.
- Driver associates, then sends unsolicited `WDI_INDICATION_ASSOCIATION_RESULT` (list of
  `WDI_ASSOCIATION_RESULT_CONTAINER`: BSSID, AssociationResultParameters {AssociationStatus,
  StatusCode, AuthAlgorithm, Unicast/MulticastData/MulticastMgmt cipher, WMMQoSEnabled, BandID, ...},
  AssociationRequestFrame / AssociationResponseFrame / BeaconProbeResponse = frame BODIES (no 802.11
  header), ActivePhyTypeList), then `WDI_INDICATION_LINK_STATE_CHANGE`, then the M4
  `WDI_INDICATION_CONNECT_COMPLETE` (sample: WifiIhvPerformAssociation).
- WPA2: Windows runs the 4-way handshake itself; EAPOL goes over the NetAdapter data path; keys come
  with `WDI_SET_ADD_CIPHER_KEYS` (29). `WDI_TASK_DISCONNECT` (8?) -> `WDI_INDICATION_DISCONNECT_COMPLETE`.
  AP-initiated loss -> `WDI_INDICATION_DISASSOCIATION`.

## Firmware side (ath10k station flow, WMI-TLV ids for this firmware)
| step | command (id, tag) | struct |
|---|---|---|
| power save off | STA_POWERSAVE_MODE 0x9001 (tag 0x6c) | {vdev_id, sta_ps_mode=0} |
| tune to the AP | VDEV_START_REQUEST 0x5003 (tag 0x58) + CHANNEL tlv (0x50) + empty ARRAY_STRUCT | {vdev_id, requestor_id, bcn_intval, dtim_period, flags, wmi_ssid{len, 32}, bcn_tx_rate, bcn_tx_power, num_noa_descr, disable_hw_ack} |
| -> event | VDEV_START_RESP 0x5001 (tag 0x28) | {vdev_id, req_id, resp_type, status} |
| AP peer | PEER_CREATE 0x6001 (tag 0x61) | {vdev_id, mac(8), peer_type 0} |
| auth / assoc frames | MGMT_TX_SEND 0x7008 (tag 0x1a6) + ARRAY_BYTE frame copy | {vdev_id, desc_id, chanfreq 0, u64 paddr (frame in host memory too), frame_len, buf_len (round 4)} |
| -> events | MGMT_TX_COMPLETION 0x7006 (tag 0x1a7) {desc_id, status, ...}; replies arrive as MGMT_RX 0x7001 |
| after assoc resp | PEER_ASSOC 0x6005 (tag 0x65) + ARRAY_BYTE legacy rates + ARRAY_BYTE ht rates + VHT_RATE_SET (0x66) | {mac(8), vdev_id, new_assoc, assoc_id, flags, caps, listen_intval, ht_caps, max_mpdu, mpdu_density, rate_caps, nss, vht_caps, phy_mode, ht_info[2], num_legacy_rates, num_ht_rates} |
| bss up | VDEV_UP 0x5005 (tag 0x5c) | {vdev_id, assoc_id, bssid(8)} |
| keys | VDEV_INSTALL_KEY 0x5009 (tag 0x60) + ARRAY_BYTE key | {vdev_id, mac(8), key_idx, key_flags, key_cipher (TLV: NONE 0, WEP 1, TKIP 2, AES_OCB 3, AES_CCM 4), rsc/global rsc/tsc counters, wpi counters, key_len, txmic_len, rxmic_len} |
| port open | PEER_SET_PARAM 0x6004 (tag 0x64) | {vdev_id, mac(8), param_id (AUTHORIZE 3), value} |
| teardown | VDEV_DOWN 0x5007, PEER_DELETE 0x6002, VDEV_STOP 0x5006 (-> VDEV_STOPPED 0x5002) | |
| AP kicks us | PEER_STA_KICKOUT event 0x6001 | |

Peer flags (TLV): AUTH 0x1, QOS 0x2, NEED_PTK_4_WAY 0x4, NEED_GTK_2_WAY 0x10, HT 0x1000, 40MHZ 0x2000,
VHT 0x2000000, 80MHZ 0x4000000, PMF 0x8000000. Phy modes: 11A 0, 11G 1, 11NA_HT20 4, 11NG_HT20 5.
The firmware advertises WMI_TLV_SERVICE_MGMT_TX_WMI (76), so management frames go through WMI.

## Plan
- v0.3 (association): TASK_CONNECT -> VDEV_START -> PEER_CREATE -> open-system AUTH -> ASSOC REQ
  (SSID, the AP's rates, RSN IE for WPA2-PSK with the AP's group cipher) -> ASSOC RESP -> PEER_ASSOC
  (legacy first) -> VDEV_UP -> ASSOCIATION_RESULT + LINK_STATE_CHANGE + CONNECT_COMPLETE.
  TASK_DISCONNECT / kickout -> deauth, VDEV_DOWN, PEER_DELETE, VDEV_STOP. HTT rx in-order indications
  are logged (EAPOL M1 should show up there).
- v0.4 (data path): HTT RX in-order -> native-wifi -> Ethernet -> NetAdapter Rx; NetAdapter Tx ->
  HTT TX (frag desc bank) -> firmware; EAPOL makes WPA2 possible; WDI_SET_ADD_CIPHER_KEYS ->
  VDEV_INSTALL_KEY; PEER_SET_PARAM AUTHORIZE.
- Then HT/VHT association (HT caps + WMM IEs, PEER_ASSOC HT flags/rates), power save, interrupts.
