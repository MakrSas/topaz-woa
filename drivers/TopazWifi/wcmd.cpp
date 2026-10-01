/*
 * WiFiCx commands (WDI tasks/properties) and the WLAN core callbacks (WDK wificx sample
 * wifirequest.cpp / wifitransition.cpp / wifihal.cpp, reduced to stage A):
 *  - TASK_SET_RADIO_STATE, TASK_DOT11_RESET: accepted (M3 + indication + M4);
 *  - TASK_SCAN: M3 at once, the firmware runs a passive scan on the modem thread, every BSS heard
 *    goes up as WDI_INDICATION_BSS_ENTRY_LIST, then the M4 SCAN_COMPLETE;
 *  - everything else: NOT_SUPPORTED, logged, as in the sample.
 * A task is M1 (request) -> M3 (WifiRequestComplete) -> M4 (indication with the M1 TransactionId).
 */
#include "wpch.h"

#define MAX_BSS          64
#define MAX_BODY         1024                    /* beacon/probe body bytes kept per BSS */
#define SCAN_TIMEOUT_MS  20000

typedef struct {
    UCHAR   Bssid[6];
    ULONG   Channel;
    LONG    Rssi;
    BOOLEAN ProbeResp;
    ULONG   BodyLen;
    UCHAR   Body[MAX_BODY];
} BSS_SEEN;

static KSPIN_LOCK          s_Lock;
static BOOLEAN             s_LockInit;
static BSS_SEEN            s_Bss[MAX_BSS];       /* this scan's table, newest beacon per BSSID */
static ULONG               s_NumBss;
static BOOLEAN             s_ScanPending;
static WDI_MESSAGE_HEADER  s_ScanHdr;            /* M1 header of the pending TASK_SCAN */
static WDFTIMER            s_ScanTimer;
static BOOLEAN             s_FwReady;
static UCHAR               s_Radio = 1;
static ULONG               s_Unsupported;

static void LockInit(void)
{
    if (!s_LockInit) {
        KeInitializeSpinLock(&s_Lock);
        s_LockInit = TRUE;
    }
}

/* Indication = WDI header (+ TLVs) in a WDFMEMORY, handed to WiFiCx (sample WifiIhvSendIndicationToOs). */
static void SendIndication(const WDI_MESSAGE_HEADER *Orig, UINT16 MessageId, UINT32 TransactionId, NTSTATUS Status,
                           const UCHAR *Tlv, ULONG TlvLen)
{
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFMEMORY mem;
    UCHAR *buf;
    WDI_MESSAGE_HEADER *h;
    NTSTATUS s;

    if (g_Wifi == nullptr) {
        return;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = g_Wifi->Device;
    s = WdfMemoryCreate(&attrs, NonPagedPoolNx, TOPAZ_WIFI_TAG, sizeof(WDI_MESSAGE_HEADER) + TlvLen, &mem,
                        reinterpret_cast<void **>(&buf));
    if (!NT_SUCCESS(s)) {
        WLOG("indication %u: WdfMemoryCreate %08x\r\n", MessageId, s);
        return;
    }
    RtlZeroMemory(buf, sizeof(WDI_MESSAGE_HEADER) + TlvLen);
    h = reinterpret_cast<WDI_MESSAGE_HEADER *>(buf);
    h->PortId = Orig->PortId;
    h->Reserved = Orig->Reserved;
    h->Status = Status;                           /* NTSTATUS and NDIS_STATUS share success/failure codes here */
    h->TransactionId = TransactionId;             /* 0 = unsolicited */
    h->IhvSpecificId = Orig->IhvSpecificId;
    if (TlvLen != 0) {
        RtlCopyMemory(buf + sizeof(WDI_MESSAGE_HEADER), Tlv, TlvLen);
    }
    WifiDeviceReceiveIndication(g_Wifi->Device, MessageId, mem);
    WdfObjectDelete(mem);
}

static void SendM4(const WDI_MESSAGE_HEADER *M1, UINT16 CompleteId, NTSTATUS Status)
{
    SendIndication(M1, CompleteId, M1->TransactionId, Status, nullptr, 0);
}

static void CompleteM3(WIFIREQUEST Request, NTSTATUS Status)
{
    WifiRequestComplete(Request, Status, sizeof(WDI_MESSAGE_HEADER));
}

/* ---------------- scan ---------------- */

static ULONG LinkQuality(LONG Rssi)
{
    if (Rssi >= -50) {
        return 100;
    }
    if (Rssi <= -100) {
        return 0;
    }
    return (ULONG)(2 * (Rssi + 100));
}

/* One BSS -> unsolicited WDI_INDICATION_BSS_ENTRY_LIST (sample: one indication per entry). */
static void IndicateBss(const BSS_SEEN *B)
{
    WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS params;
    WDI_BSS_ENTRY_CONTAINER entry;
    UINT8 *out = nullptr;
    ULONG outLen = 0;
    NDIS_STATUS ns;

    RtlCopyMemory(entry.BSSID.Address, B->Bssid, 6);
    if (B->ProbeResp) {
        entry.Optional.ProbeResponseFrame_IsPresent = 1;
        entry.ProbeResponseFrame.ElementCount = B->BodyLen;
        entry.ProbeResponseFrame.pElements = const_cast<UINT8 *>(B->Body);
    } else {
        entry.Optional.BeaconFrame_IsPresent = 1;
        entry.BeaconFrame.ElementCount = B->BodyLen;
        entry.BeaconFrame.pElements = const_cast<UINT8 *>(B->Body);
    }
    entry.SignalInfo.RSSI = B->Rssi;
    entry.SignalInfo.LinkQuality = LinkQuality(B->Rssi);
    entry.ChannelInfo.ChannelNumber = B->Channel;
    entry.ChannelInfo.BandId = B->Channel <= 14 ? WDI_BAND_ID_2400 : WDI_BAND_ID_5000;

    params.Optional.DeviceDescriptor_IsPresent = 1;
    params.DeviceDescriptor.ElementCount = 1;
    params.DeviceDescriptor.pElements = &entry;    /* not internally allocated: no free on cleanup */

    ns = GenerateWdiIndicationBssEntryList(&params, 0, &g_Wifi->Tlv, &outLen, &out);
    params.DeviceDescriptor.ElementCount = 0;      /* our stack/table memory: detach before destructors */
    params.DeviceDescriptor.pElements = nullptr;
    entry.ProbeResponseFrame.ElementCount = entry.BeaconFrame.ElementCount = 0;
    entry.ProbeResponseFrame.pElements = entry.BeaconFrame.pElements = nullptr;
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("BSS entry TLV generation failed %08x\r\n", ns);
        return;
    }
    SendIndication(&s_ScanHdr, WDI_INDICATION_BSS_ENTRY_LIST, 0, STATUS_SUCCESS, out, outLen);
    FreeGenerated(out);
}

/* Scan over (firmware done, firmware failed or timeout): BSS list, then the M4. Modem thread or timer. */
static void FinishScan(NTSTATUS Status, const char *Why)
{
    static BSS_SEEN snapshot[MAX_BSS];             /* indications are built outside the spin lock */
    WDI_MESSAGE_HEADER hdr;
    KIRQL irql;
    ULONG n, i;

    KeAcquireSpinLock(&s_Lock, &irql);
    if (!s_ScanPending) {
        KeReleaseSpinLock(&s_Lock, irql);
        return;
    }
    s_ScanPending = FALSE;
    hdr = s_ScanHdr;
    n = s_NumBss;
    RtlCopyMemory(snapshot, s_Bss, n * sizeof(BSS_SEEN));
    KeReleaseSpinLock(&s_Lock, irql);

    if (s_ScanTimer != nullptr) {
        WdfTimerStop(s_ScanTimer, FALSE);
    }
    for (i = 0; i < n; i++) {
        IndicateBss(&snapshot[i]);
    }
    WLOG("scan done (%s): %u BSS indicated, M4 %08x\r\n", Why, n, Status);
    SendM4(&hdr, WDI_INDICATION_SCAN_COMPLETE, Status);
}

static void EvtScanTimer(WDFTIMER Timer)
{
    UNREFERENCED_PARAMETER(Timer);
    FinishScan(s_FwReady ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY, "timeout");
}

/* ---- WLAN core callbacks (modem thread, PASSIVE_LEVEL) ---- */

extern "C" VOID WlanOnReady(const UCHAR *Mac)
{
    UNREFERENCED_PARAMETER(Mac);
    s_FwReady = TRUE;
    WLOG("WLAN firmware ready for WiFiCx (scan %s)\r\n", s_ScanPending ? "pending" : "idle");
}

extern "C" VOID WlanOnBss(const UCHAR *Bssid, const UCHAR *Body, ULONG BodyLen, ULONG Channel, LONG Rssi,
                          BOOLEAN ProbeResponse)
{
    KIRQL irql;
    ULONG i;
    BSS_SEEN *b = nullptr;

    if (!s_LockInit) {
        return;
    }
    KeAcquireSpinLock(&s_Lock, &irql);
    if (s_ScanPending) {
        for (i = 0; i < s_NumBss; i++) {
            if (RtlCompareMemory(s_Bss[i].Bssid, Bssid, 6) == 6) {
                b = &s_Bss[i];
                break;
            }
        }
        if (b == nullptr && s_NumBss < MAX_BSS) {
            b = &s_Bss[s_NumBss++];
        }
        if (b != nullptr && (ProbeResponse || !b->ProbeResp)) {   /* a probe response beats a beacon */
            RtlCopyMemory(b->Bssid, Bssid, 6);
            b->Channel = Channel;
            b->Rssi = Rssi;
            b->ProbeResp = ProbeResponse;
            b->BodyLen = min(BodyLen, (ULONG)MAX_BODY);
            RtlCopyMemory(b->Body, Body, b->BodyLen);
        }
    }
    KeReleaseSpinLock(&s_Lock, irql);
}

extern "C" VOID WlanOnScanDone(LONG Status)
{
    FinishScan(Status == 0 ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL, Status == 0 ? "firmware" : "firmware failed");
}

/* ---------------- commands ---------------- */

static void ScanTimerCreate(void)
{
    WDF_TIMER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attrs;
    NTSTATUS s;

    if (s_ScanTimer != nullptr || g_Wifi == nullptr) {
        return;
    }
    WDF_TIMER_CONFIG_INIT(&config, EvtScanTimer);
    config.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = g_Wifi->Device;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;   /* WiFiCx indications at PASSIVE_LEVEL */
    s = WdfTimerCreate(&config, &attrs, &s_ScanTimer);
    if (!NT_SUCCESS(s)) {
        WLOG("WdfTimerCreate: %08x\r\n", s);
        s_ScanTimer = nullptr;
    }
}

static void OnScan(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen)
{
    WDI_SCAN_PARAMETERS params;
    NDIS_STATUS ns;
    KIRQL irql;
    BOOLEAN busy;

    ns = ParseWdiTaskScan(TlvLen, Tlv, &g_Wifi->Tlv, &params);
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("TASK_SCAN: parse failed %08x\r\n", ns);
        CompleteM3(Request, NdisToNt(ns));
        return;
    }
    WLOG("TASK_SCAN: %u SSID(s), %u band list(s), fw %s\r\n", params.SSIDList.ElementCount,
         params.BandChannelList.ElementCount, s_FwReady ? "ready" : "booting");
    CleanupParsedWdiTaskScan(&params);

    KeAcquireSpinLock(&s_Lock, &irql);
    busy = s_ScanPending;
    if (!busy) {
        s_ScanPending = TRUE;
        s_ScanHdr = *Hdr;
        s_NumBss = 0;
    }
    KeReleaseSpinLock(&s_Lock, irql);
    if (busy) {
        CompleteM3(Request, STATUS_DEVICE_BUSY);
        return;
    }
    CompleteM3(Request, STATUS_SUCCESS);
    ScanTimerCreate();
    if (s_ScanTimer != nullptr) {
        WdfTimerStart(s_ScanTimer, WDF_REL_TIMEOUT_IN_MS(SCAN_TIMEOUT_MS));
    }
    WlanScanRequest();                            /* the modem thread starts it once the fw is ready */
}

static void OnSetRadioState(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen)
{
    WDI_SET_RADIO_STATE_PARAMETERS params;
    WDI_INDICATION_RADIO_STATUS_PARAMETERS status = {};
    UINT8 *out = nullptr;
    ULONG outLen = 0;
    NDIS_STATUS ns;

    ns = ParseWdiTaskSetRadioState(TlvLen, Tlv, &g_Wifi->Tlv, &params);
    if (ns != NDIS_STATUS_SUCCESS) {
        WLOG("TASK_SET_RADIO_STATE: parse failed %08x\r\n", ns);
        CompleteM3(Request, NdisToNt(ns));
        return;
    }
    WLOG("TASK_SET_RADIO_STATE: software radio %u (was %u)\r\n", params.SoftwareRadioState, s_Radio);
    s_Radio = params.SoftwareRadioState;
    CleanupParsedWdiTaskSetRadioState(&params);
    CompleteM3(Request, STATUS_SUCCESS);

    status.RadioState.HardwareState = TRUE;
    status.RadioState.SoftwareState = s_Radio;
    if (GenerateWdiIndicationRadioStatus(&status, 0, &g_Wifi->Tlv, &outLen, &out) == NDIS_STATUS_SUCCESS) {
        SendIndication(Hdr, WDI_INDICATION_RADIO_STATUS, 0, STATUS_SUCCESS, out, outLen);
        FreeGenerated(out);
    }
    SendM4(Hdr, WDI_INDICATION_SET_RADIO_STATE_COMPLETE, STATUS_SUCCESS);
}

static void OnDot11Reset(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr)
{
    WLOG("TASK_DOT11_RESET\r\n");
    CompleteM3(Request, STATUS_SUCCESS);
    SendM4(Hdr, WDI_INDICATION_DOT11_RESET_COMPLETE, STATUS_SUCCESS);
}

/* WiFiCx -> driver: every WDI command arrives here (sample EvtWifiDeviceSendCommand). */
void EvtWifiSendCommand(WDFDEVICE Device, WIFIREQUEST Request)
{
    UINT inLen = 0, outLen = 0;
    void *buf = WifiRequestGetInOutBuffer(Request, &inLen, &outLen);
    UINT16 id = WifiRequestGetMessageId(Request);
    const WDI_MESSAGE_HEADER *hdr = static_cast<const WDI_MESSAGE_HEADER *>(buf);
    const UCHAR *tlv = static_cast<const UCHAR *>(buf) + sizeof(WDI_MESSAGE_HEADER);
    ULONG tlvLen;

    UNREFERENCED_PARAMETER(Device);
    LockInit();
    if (buf == nullptr || inLen < sizeof(WDI_MESSAGE_HEADER)) {
        CompleteM3(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    tlvLen = inLen - (ULONG)sizeof(WDI_MESSAGE_HEADER);
    switch (id) {
    case WDI_TASK_SCAN:
        OnScan(Request, hdr, tlv, tlvLen);
        break;
    case WDI_TASK_SET_RADIO_STATE:
        OnSetRadioState(Request, hdr, tlv, tlvLen);
        break;
    case WDI_TASK_DOT11_RESET:
        OnDot11Reset(Request, hdr);
        break;
    default:
        if (s_Unsupported++ < 64) {
            WLOG("WDI message %u (port %u, %u bytes): not supported\r\n", id, hdr->PortId, inLen);
        }
        CompleteM3(Request, STATUS_NOT_SUPPORTED);
        break;
    }
}
