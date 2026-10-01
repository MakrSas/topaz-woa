/*
 * Wi-Fi capabilities reported to WiFiCx in PrepareHardware (WDK wificx sample
 * WifiIhvSetDeviceCapabilities, trimmed to WiFiCx 1.0 and to what the WCN3950 does: 1x1,
 * 2.4 + 5 GHz, 802.11a/b/g/n/ac, open and WPA2-PSK (CCMP). Channels = the ones our scan visits.
 */
#include "wpch.h"

static const WDI_CHANNEL_MAPPING_ENTRY s_Chan24[] = {
    { 1, 2412 }, { 2, 2417 }, { 3, 2422 }, { 4, 2427 }, { 5, 2432 }, { 6, 2437 }, { 7, 2442 },
    { 8, 2447 }, { 9, 2452 }, { 10, 2457 }, { 11, 2462 }, { 12, 2467 }, { 13, 2472 },
};

static const WDI_CHANNEL_MAPPING_ENTRY s_Chan5[] = {
    { 36, 5180 }, { 40, 5200 }, { 44, 5220 }, { 48, 5240 }, { 52, 5260 }, { 56, 5280 }, { 60, 5300 },
    { 64, 5320 }, { 100, 5500 }, { 104, 5520 }, { 108, 5540 }, { 112, 5560 }, { 116, 5580 },
    { 120, 5600 }, { 124, 5620 }, { 128, 5640 }, { 132, 5660 }, { 136, 5680 }, { 140, 5700 },
    { 149, 5745 }, { 153, 5765 }, { 157, 5785 }, { 161, 5805 }, { 165, 5825 },
};

static NTSTATUS SetDeviceCaps(WDFDEVICE Device)
{
    WIFI_DEVICE_CAPABILITIES caps;

    WIFI_DEVICE_CAPABILITIES_INIT(&caps);
    caps.HardwareRadioState = TRUE;
    caps.SoftwareRadioState = TRUE;
    RtlCopyMemory(caps.FirmwareVersion, "WLAN.HL.3.2.4", sizeof("WLAN.HL.3.2.4"));
    caps.ActionFramesSupported = FALSE;
    caps.NumRxStreams = 1;
    caps.NumTxStreams = 1;
    caps.BluetoothCoexistenceSupport = WDI_BLUETOOTH_COEXISTENCE_UNKNOWN;
    return WifiDeviceSetDeviceCapabilities(Device, &caps);
}

static NTSTATUS SetStationCaps(WDFDEVICE Device)
{
    static const DOT11_AUTH_CIPHER_PAIR unicast[] = {
        { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
        { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP },
        { DOT11_AUTH_ALGO_RSNA, DOT11_CIPHER_ALGO_CCMP },
    };
    static const DOT11_AUTH_CIPHER_PAIR mcastData[] = {
        { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
        { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_CCMP },
        { DOT11_AUTH_ALGO_RSNA_PSK, DOT11_CIPHER_ALGO_TKIP },
        { DOT11_AUTH_ALGO_RSNA, DOT11_CIPHER_ALGO_CCMP },
    };
    static const DOT11_AUTH_CIPHER_PAIR mcastMgmt[] = {
        { DOT11_AUTH_ALGO_80211_OPEN, DOT11_CIPHER_ALGO_NONE },
    };
    WIFI_STATION_CAPABILITIES caps;

    WIFI_STATION_CAPABILITIES_INIT(&caps);
    caps.ScanSSIDListSize = 4;
    caps.DesiredSSIDListSize = 1;
    caps.PrivacyExemptionListSize = 1;
    caps.KeyMappingTableSize = 32;
    caps.DefaultKeyTableSize = 4;
    caps.WEPKeyValueMaxLength = 0x20;
    caps.MaxNumPerSTA = 4;
    caps.NumSupportedUnicastAlgorithms = ARRAYSIZE(unicast);
    caps.UnicastAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(unicast);
    caps.NumSupportedMulticastDataAlgorithms = ARRAYSIZE(mcastData);
    caps.MulticastDataAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(mcastData);
    caps.NumSupportedMulticastMgmtAlgorithms = ARRAYSIZE(mcastMgmt);
    caps.MulticastMgmtAlgorithmsList = const_cast<PDOT11_AUTH_CIPHER_PAIR>(mcastMgmt);
    return WifiDeviceSetStationCapabilities(Device, &caps);
}

static NTSTATUS SetBandCaps(WDFDEVICE Device)
{
    static const WDI_PHY_TYPE phy24[] = { WDI_PHY_TYPE_HRDSSS, WDI_PHY_TYPE_ERP, WDI_PHY_TYPE_HT };
    static const WDI_PHY_TYPE phy5[] = { WDI_PHY_TYPE_OFDM, WDI_PHY_TYPE_HT, WDI_PHY_TYPE_VHT };
    static UINT32 width24[] = { 20 };
    static UINT32 width5[] = { 20, 40, 80 };
    WIFI_BAND_INFO bands[2] = {};
    WIFI_BAND_CAPABILITIES caps = {};

    bands[0].BandID = WDI_BAND_ID_2400;
    bands[0].BandState = TRUE;
    bands[0].NumValidPhyTypes = ARRAYSIZE(phy24);
    bands[0].ValidPhyTypeList = const_cast<WDI_PHY_TYPE *>(phy24);
    bands[0].NumValidChannelTypes = ARRAYSIZE(s_Chan24);
    bands[0].ValidChannelTypes = const_cast<WDI_CHANNEL_MAPPING_ENTRY *>(s_Chan24);
    bands[0].NumChannelWidths = ARRAYSIZE(width24);
    bands[0].ChannelWidthList = width24;

    bands[1].BandID = WDI_BAND_ID_5000;
    bands[1].BandState = TRUE;
    bands[1].NumValidPhyTypes = ARRAYSIZE(phy5);
    bands[1].ValidPhyTypeList = const_cast<WDI_PHY_TYPE *>(phy5);
    bands[1].NumValidChannelTypes = ARRAYSIZE(s_Chan5);
    bands[1].ValidChannelTypes = const_cast<WDI_CHANNEL_MAPPING_ENTRY *>(s_Chan5);
    bands[1].NumChannelWidths = ARRAYSIZE(width5);
    bands[1].ChannelWidthList = width5;

    caps.Size = sizeof(caps);
    caps.NumBands = ARRAYSIZE(bands);
    caps.BandInfoList = bands;
    return WifiDeviceSetBandCapabilities(Device, &caps);
}

/* Data rates in 500 kb/s units: legacy b/g/a rates, HT/VHT with their 1x1 top rates. */
static NTSTATUS SetPhyCaps(WDFDEVICE Device)
{
    static const UINT16 legacyB[] = { 2, 4, 11, 22 };
    static const UINT16 legacyAG[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
    static const UINT16 ht[] = { 13, 26, 39, 52, 78, 104, 117, 130, 300 };     /* MCS0-7 20 MHz .. 150 Mb/s */
    static const UINT16 vht[] = { 13, 130, 400, 866 };                          /* .. 433 Mb/s 80 MHz MCS9 */
    static const struct { WDI_PHY_TYPE Type; const UINT16 *Rates; UINT32 N; } list[] = {
        { WDI_PHY_TYPE_HRDSSS, legacyB, ARRAYSIZE(legacyB) },
        { WDI_PHY_TYPE_ERP, legacyAG, ARRAYSIZE(legacyAG) },
        { WDI_PHY_TYPE_OFDM, legacyAG, ARRAYSIZE(legacyAG) },
        { WDI_PHY_TYPE_HT, ht, ARRAYSIZE(ht) },
        { WDI_PHY_TYPE_VHT, vht, ARRAYSIZE(vht) },
    };
    static WIFI_PHY_INFO info[ARRAYSIZE(list)];                 /* big (126 rates each): not on the stack */
    WIFI_PHY_CAPABILITIES caps = {};
    UINT32 i, r;

    RtlZeroMemory(info, sizeof(info));
    for (i = 0; i < ARRAYSIZE(list); i++) {
        info[i].PhyType = list[i].Type;
        info[i].NumberDataRateEntries = list[i].N;
        for (r = 0; r < list[i].N; r++) {
            info[i].DataRateList[r].DataRateFlag = WDI_DATA_RATE_RX_RATE | WDI_DATA_RATE_TX_RATE;
            info[i].DataRateList[r].DataRateValue = list[i].Rates[r];
        }
    }
    caps.Size = sizeof(caps);
    caps.NumPhyTypes = ARRAYSIZE(list);
    caps.PhyInfoList = info;
    return WifiDeviceSetPhyCapabilities(Device, &caps);
}

NTSTATUS WifiSetCapabilities(WDFDEVICE Device)
{
    NTSTATUS s1 = SetDeviceCaps(Device), s2 = SetStationCaps(Device), s3 = SetBandCaps(Device),
             s4 = SetPhyCaps(Device);

    WLOG("caps: device %08x station %08x band %08x phy %08x\r\n", s1, s2, s3, s4);
    if (!NT_SUCCESS(s1)) {
        return s1;
    }
    if (!NT_SUCCESS(s2)) {
        return s2;
    }
    return NT_SUCCESS(s3) ? s4 : s3;
}
