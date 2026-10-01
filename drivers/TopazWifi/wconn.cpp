/*
 * WDI connect / disconnect / keys (WDK wificx sample WifiIhvConnect / PerformAssociation, done for
 * real): TASK_CONNECT -> the core associates (assoc.c) -> WDI_INDICATION_ASSOCIATION_RESULT +
 * LINK_STATE_CHANGE -> M4 CONNECT_COMPLETE. WPA2: Windows then runs the 4-way handshake over the
 * data path and hands the keys in WDI_SET_ADD_CIPHER_KEYS (-> VDEV_INSTALL_KEY).
 */
#include "wpch.h"

static BOOLEAN             s_ConnPending, s_DiscPending, s_Connected;
static WDI_MESSAGE_HEADER  s_ConnHdr, s_DiscHdr;
static WLAN_CONNECT        s_Req;                 /* what we asked the core for */
static WDI_AUTH_ALGORITHM  s_Auth;
static WDI_CIPHER_ALGORITHM s_Ucast, s_Mcast;
static ULONG               s_Channel;
static LONG                s_Rssi;

static ULONG ChanToFreq(ULONG Chan, WDI_BAND_ID Band)
{
    if (Band == WDI_BAND_ID_2400) {
        return Chan == 14 ? 2484 : 2407 + 5 * Chan;
    }
    return 5000 + 5 * Chan;
}

static void AppendIe(const WDI_BYTE_BLOB &Blob)
{
    if (Blob.ElementCount != 0 && s_Req.ExtraIeLen + Blob.ElementCount <= sizeof(s_Req.ExtraIe)) {
        RtlCopyMemory(s_Req.ExtraIe + s_Req.ExtraIeLen, Blob.pElements, Blob.ElementCount);
        s_Req.ExtraIeLen += Blob.ElementCount;
    }
}

void OnConnect(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen)
{
    WDI_TASK_CONNECT_PARAMETERS params;
    NDIS_STATUS ns;
    NTSTATUS st = STATUS_SUCCESS;

    ns = ParseWdiTaskConnect(TlvLen, Tlv, &g_Wifi->Tlv, &params);
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("TASK_CONNECT: parse failed %08x\r\n", ns);
        CompleteM3(Request, NdisToNt(ns));
        return;
    }
    const WDI_CONNECT_PARAMETERS_CONTAINER &cp = params.ConnectParameters;
    WLOG("TASK_CONNECT: %u BSS, %u auth (%u), %u ucast (%u), %u mcast (%u), ssid len %u\r\n",
         params.PreferredBSSEntryList.ElementCount, cp.AuthenticationAlgorithms.ElementCount,
         cp.AuthenticationAlgorithms.ElementCount ? (ULONG)cp.AuthenticationAlgorithms.pElements[0] : 0,
         cp.UnicastCipherAlgorithms.ElementCount,
         cp.UnicastCipherAlgorithms.ElementCount ? (ULONG)cp.UnicastCipherAlgorithms.pElements[0] : 0,
         cp.MulticastCipherAlgorithms.ElementCount,
         cp.MulticastCipherAlgorithms.ElementCount ? (ULONG)cp.MulticastCipherAlgorithms.pElements[0] : 0,
         cp.SSIDList.ElementCount ? cp.SSIDList.pElements[0].ElementCount : 0);

    s_Auth = cp.AuthenticationAlgorithms.ElementCount ? cp.AuthenticationAlgorithms.pElements[0] : WDI_AUTH_ALGO_80211_OPEN;
    s_Ucast = cp.UnicastCipherAlgorithms.ElementCount ? cp.UnicastCipherAlgorithms.pElements[0] : WDI_CIPHER_ALGO_NONE;
    s_Mcast = cp.MulticastCipherAlgorithms.ElementCount ? cp.MulticastCipherAlgorithms.pElements[0] : WDI_CIPHER_ALGO_NONE;
    if (params.PreferredBSSEntryList.ElementCount == 0 || cp.SSIDList.ElementCount == 0) {
        st = STATUS_INVALID_PARAMETER;
    } else if (s_Auth != WDI_AUTH_ALGO_80211_OPEN && s_Auth != WDI_AUTH_ALGO_RSNA_PSK && s_Auth != WDI_AUTH_ALGO_RSNA) {
        st = STATUS_NOT_SUPPORTED;                /* WPA3-SAE, OWE, shared key, WPA1: not yet */
    } else if (s_ConnPending || s_DiscPending) {
        st = STATUS_DEVICE_BUSY;
    }
    if (!NT_SUCCESS(st)) {
        WLOG("TASK_CONNECT: refused %08x (auth %u)\r\n", st, (ULONG)s_Auth);
        CleanupParsedWdiTaskConnect(&params);
        CompleteM3(Request, st);
        return;
    }

    const WDI_CONNECT_BSS_ENTRY_CONTAINER &bss = params.PreferredBSSEntryList.pElements[0];
    const WDI_BYTE_BLOB &body = bss.Optional.ProbeResponseFrame_IsPresent ? bss.ProbeResponseFrame : bss.BeaconFrame;
    RtlZeroMemory(&s_Req, sizeof(s_Req));
    RtlCopyMemory(s_Req.Bssid, bss.BSSID.Address, 6);
    s_Channel = bss.ChannelInfo.ChannelNumber;
    s_Rssi = bss.SignalInfo.RSSI;
    s_Req.Freq = ChanToFreq(s_Channel, bss.ChannelInfo.BandId);
    s_Req.SsidLen = min(cp.SSIDList.pElements[0].ElementCount, (ULONG)sizeof(s_Req.Ssid));
    RtlCopyMemory(s_Req.Ssid, cp.SSIDList.pElements[0].pElements, s_Req.SsidLen);
    s_Req.Rsn = (s_Auth == WDI_AUTH_ALGO_RSNA_PSK || s_Auth == WDI_AUTH_ALGO_RSNA);
    s_Req.Akm = (s_Auth == WDI_AUTH_ALGO_RSNA_PSK) ? 2 : 1;
    s_Req.BodyLen = min(body.ElementCount, (ULONG)sizeof(s_Req.Body));
    RtlCopyMemory(s_Req.Body, body.pElements, s_Req.BodyLen);
    if (cp.Optional.AssociationRequestVendorIE_IsPresent) {
        AppendIe(cp.AssociationRequestVendorIE);
    }
    if (bss.Optional.AssociationRequestVendorIE_IsPresent) {
        AppendIe(bss.AssociationRequestVendorIE);
    }
    WLOG("TASK_CONNECT: %02x:%02x:%02x:**:**:** ch %u (%u MHz) rssi %d, %s, body %u, extra IEs %u\r\n",
         s_Req.Bssid[0], s_Req.Bssid[1], s_Req.Bssid[2], s_Channel, s_Req.Freq, s_Rssi,
         s_Req.Rsn ? "WPA2" : "open", s_Req.BodyLen, s_Req.ExtraIeLen);
    CleanupParsedWdiTaskConnect(&params);

    s_ConnHdr = *Hdr;
    s_ConnPending = TRUE;
    CompleteM3(Request, STATUS_SUCCESS);
    WlanConnectRequest(&s_Req);
}

static void SetBlob(WDI_BYTE_BLOB &Blob, const UCHAR *P, ULONG Len)
{
    Blob.ElementCount = Len;
    Blob.pElements = const_cast<UINT8 *>(P);       /* not owned: detached again before destruction */
}

static void ClearBlob(WDI_BYTE_BLOB &Blob)
{
    Blob.ElementCount = 0;
    Blob.pElements = nullptr;
}

static void IndicateAssocResult(LONG Status, const UCHAR *ReqBody, ULONG ReqLen, const UCHAR *RespBody, ULONG RespLen)
{
    WDI_INDICATION_ASSOCIATION_RESULT_LIST list;
    WDI_ASSOCIATION_RESULT_CONTAINER r;
    WDI_PHY_TYPE phy = s_Req.Freq < 4000 ? WDI_PHY_TYPE_ERP : WDI_PHY_TYPE_OFDM;
    UINT8 *out = nullptr;
    ULONG outLen = 0;
    NDIS_STATUS ns;

    RtlCopyMemory(r.BSSID.Address, s_Req.Bssid, 6);
    r.AssociationResultParameters.AssociationStatus = Status == 0 ? WDI_ASSOC_STATUS_SUCCESS :
                                                      Status > 0 ? WDI_ASSOC_STATUS_ASSOC_FAILED_BY_PEER :
                                                      WDI_ASSOC_STATUS_FAILURE;
    r.AssociationResultParameters.StatusCode = Status > 0 ? (UINT32)Status : 0;
    r.AssociationResultParameters.AuthAlgorithm = s_Auth;
    r.AssociationResultParameters.UnicastCipherAlgorithm = s_Ucast;
    r.AssociationResultParameters.MulticastDataCipherAlgorithm = s_Mcast;
    r.AssociationResultParameters.MulticastMgmtCipherAlgorithm = WDI_CIPHER_ALGO_NONE;
    r.AssociationResultParameters.PortAuthorized = !s_Req.Rsn;     /* open: data flows at once */
    r.AssociationResultParameters.BandID = s_Req.Freq < 4000 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    if (ReqLen != 0) {
        r.Optional.AssociationRequestFrame_IsPresent = 1;
        SetBlob(r.AssociationRequestFrame, ReqBody, ReqLen);
    }
    if (RespLen != 0) {
        r.Optional.AssociationResponseFrame_IsPresent = 1;
        SetBlob(r.AssociationResponseFrame, RespBody, RespLen);
    }
    if (s_Req.BodyLen != 0) {
        r.Optional.BeaconProbeResponse_IsPresent = 1;
        SetBlob(r.BeaconProbeResponse, s_Req.Body, s_Req.BodyLen);
    }
    r.ActivePhyTypeList.ElementCount = 1;
    r.ActivePhyTypeList.pElements = &phy;
    list.AssociationResults.ElementCount = 1;
    list.AssociationResults.pElements = &r;

    ns = GenerateWdiIndicationAssociationResult(&list, 0, &g_Wifi->Tlv, &outLen, &out);
    list.AssociationResults.ElementCount = 0;
    list.AssociationResults.pElements = nullptr;
    r.ActivePhyTypeList.ElementCount = 0;
    r.ActivePhyTypeList.pElements = nullptr;
    ClearBlob(r.AssociationRequestFrame);
    ClearBlob(r.AssociationResponseFrame);
    ClearBlob(r.BeaconProbeResponse);
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("association result TLV generation failed %08x\r\n", ns);
        return;
    }
    SendIndication(&s_ConnHdr, WDI_INDICATION_ASSOCIATION_RESULT, 0, STATUS_SUCCESS, out, outLen);
    FreeGenerated(out);
}

static void IndicateLinkState(void)
{
    WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS p;
    WDI_LINK_INFO_CONTAINER link = {};
    UINT8 *out = nullptr;
    ULONG outLen = 0;

    RtlCopyMemory(p.LinkStateChangeParameters.PeerMACAddress.Address, s_Req.Bssid, 6);
    p.LinkStateChangeParameters.TxLinkSpeed = 54000;            /* kb/s, legacy association */
    p.LinkStateChangeParameters.RxLinkSpeed = 54000;
    p.LinkStateChangeParameters.LinkQuality = (UINT8)(s_Rssi >= -50 ? 100 : s_Rssi <= -100 ? 0 : 2 * (s_Rssi + 100));
    link.LinkID = 0;
    RtlCopyMemory(link.LocalLinkMACAddress.Address, g_WifiMac, 6);
    RtlCopyMemory(link.PeerLinkMACAddress.Address, s_Req.Bssid, 6);
    link.ChannelNumber = s_Channel;
    link.BandId = s_Req.Freq < 4000 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;
    link.RSSI = s_Rssi;
    link.Bandwidth = 20;
    p.LinkInfo.ElementCount = 1;
    p.LinkInfo.pElements = &link;
    if (GenerateWdiIndicationLinkStateChange(&p, 0, &g_Wifi->Tlv, &outLen, &out) == NDIS_STATUS_SUCCESS) {
        SendIndication(&s_ConnHdr, WDI_INDICATION_LINK_STATE_CHANGE, 0, STATUS_SUCCESS, out, outLen);
        FreeGenerated(out);
    }
    p.LinkInfo.ElementCount = 0;
    p.LinkInfo.pElements = nullptr;
}

/* core: association finished (modem thread) */
extern "C" VOID WlanOnAssocResult(LONG Status, ULONG Aid, const UCHAR *ReqBody, ULONG ReqLen, const UCHAR *RespBody,
                                  ULONG RespLen)
{
    WLOG("association result %d aid %u (req %u B, resp %u B), connect %s\r\n", Status, Aid, ReqLen, RespLen,
         s_ConnPending ? "pending" : "not pending");
    if (!s_ConnPending) {
        return;
    }
    s_ConnPending = FALSE;
    IndicateAssocResult(Status, ReqBody, ReqLen, RespBody, RespLen);
    if (Status == 0) {
        s_Connected = TRUE;
        IndicateLinkState();
        SendM4(&s_ConnHdr, WDI_INDICATION_CONNECT_COMPLETE, STATUS_SUCCESS);
    } else {
        SendM4(&s_ConnHdr, WDI_INDICATION_CONNECT_COMPLETE, STATUS_UNSUCCESSFUL);
    }
}

/* ---------------- disconnect ---------------- */

void OnDisconnect(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen)
{
    WDI_TASK_DISCONNECT_PARAMETERS params;
    USHORT reason = 3;                            /* deauth: leaving */

    if (ParseWdiTaskDisconnect(TlvLen, Tlv, &g_Wifi->Tlv, &params) == NDIS_STATUS_SUCCESS) {
        reason = params.DisconnectParameters.Disassociation80211Reason;
        CleanupParsedWdiTaskDisconnect(&params);
    }
    WLOG("TASK_DISCONNECT: reason %u (%s)\r\n", reason, s_Connected ? "connected" : s_ConnPending ? "connecting" : "idle");
    CompleteM3(Request, STATUS_SUCCESS);
    if (!s_Connected && !s_ConnPending) {
        SendM4(Hdr, WDI_INDICATION_DISCONNECT_COMPLETE, STATUS_SUCCESS);
        return;
    }
    s_DiscHdr = *Hdr;
    s_DiscPending = TRUE;
    WlanDisconnectRequest(reason);
}

/* core: link gone (our request, or the AP / firmware ended it) */
extern "C" VOID WlanOnDisconnected(ULONG Reason, BOOLEAN ByPeer)
{
    WLOG("disconnected: reason %u by %s (task %s)\r\n", Reason, ByPeer ? "AP" : "us",
         s_DiscPending ? "pending" : "none");
    if (s_ConnPending) {                          /* lost while associating: fail the connect task */
        s_ConnPending = FALSE;
        IndicateAssocResult(-1, nullptr, 0, nullptr, 0);
        SendM4(&s_ConnHdr, WDI_INDICATION_CONNECT_COMPLETE, STATUS_UNSUCCESSFUL);
    }
    if (s_DiscPending) {
        s_DiscPending = FALSE;
        s_Connected = FALSE;
        SendM4(&s_DiscHdr, WDI_INDICATION_DISCONNECT_COMPLETE, STATUS_SUCCESS);
        return;
    }
    if (s_Connected) {
        WDI_INDICATION_DISASSOCIATION_PARAMETERS p;
        UINT8 *out = nullptr;
        ULONG outLen = 0;

        s_Connected = FALSE;
        RtlCopyMemory(p.DisconnectIndicationParameters.MacAddress.Address, s_Req.Bssid, 6);
        p.DisconnectIndicationParameters.DisassociationWABIReason = WDI_ASSOC_STATUS_PEER_DISASSOCIATED;
        if (GenerateWdiIndicationDisassociation(&p, 0, &g_Wifi->Tlv, &outLen, &out) == NDIS_STATUS_SUCCESS) {
            SendIndication(&s_ConnHdr, WDI_INDICATION_DISASSOCIATION, 0, STATUS_SUCCESS, out, outLen);
            FreeGenerated(out);
        }
    }
}

/* ---------------- keys ---------------- */

/* WDI_SET_ADD_CIPHER_KEYS (property, M3 only): CCMP pairwise / group keys -> VDEV_INSTALL_KEY */
void OnAddKeys(WIFIREQUEST Request, const UCHAR *Tlv, ULONG TlvLen)
{
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS params;
    NDIS_STATUS ns;
    ULONG i;

    ns = ParseWdiSetAddCipherKeys(TlvLen, Tlv, &g_Wifi->Tlv, &params);
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("SET_ADD_CIPHER_KEYS: parse failed %08x\r\n", ns);
        CompleteM3(Request, NdisToNt(ns));
        return;
    }
    for (i = 0; i < params.SetCipherKey.ElementCount; i++) {
        const WDI_SET_ADD_CIPHER_KEYS_CONTAINER &k = params.SetCipherKey.pElements[i];
        BOOLEAN group = k.CipherKeyTypeInfo.KeyType == WDI_CIPHER_KEY_TYPE_GROUP_KEY;
        ULONG idx = k.Optional.CipherKeyID_IsPresent ? k.CipherKeyID.CipherKeyID : 0;

        WLOG("SET_ADD_CIPHER_KEYS: %s key id %u cipher %u, ccmp %u bytes\r\n", group ? "group" : "pairwise", idx,
             (ULONG)k.CipherKeyTypeInfo.CipherAlgorithm, k.Optional.CCMPKey_IsPresent ? k.CCMPKey.ElementCount : 0);
        if (k.CipherKeyTypeInfo.CipherAlgorithm == WDI_CIPHER_ALGO_CCMP && k.Optional.CCMPKey_IsPresent) {
            WlanInstallKey(group, idx, 4, k.CCMPKey.pElements, k.CCMPKey.ElementCount);
        }
    }
    CleanupParsedWdiSetAddCipherKeys(&params);
    CompleteM3(Request, STATUS_SUCCESS);
}
