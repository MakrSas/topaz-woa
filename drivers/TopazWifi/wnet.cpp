/*
 * Station NetAdapter (WDK wificx sample adapter.cpp + netvadapterlibrary ConfigureDataCapabilities)
 * and its data path (native 802.11 frames, see below).
 */
#include "wpch.h"
#include <net/virtualaddress.h>

#define MTU_SIZE          1500
#define MAX_LINK_SPEED    433000000ULL          /* 1x1 VHT80 MCS9 */
#define RX_BUFFER_SIZE    2048

/*
 * Data path. Windows hands native 802.11 data frames (24-byte header, LLC/SNAP, payload) to the Tx
 * queue and expects the same format on Rx; the firmware runs in native-wifi decap/encap mode, so
 * frames pass through unchanged: Tx -> WlanTxQueue (HTT TX on the modem thread), HTT RX in-order
 * -> WlanOnRxFrame -> a small ring here -> the Rx queue (system-managed buffers).
 */
#define RXQ_N   64
#define FRAME_MAX 2048

typedef struct { USHORT Len; UCHAR Data[FRAME_MAX]; } RXQ_ENT;

typedef struct {
    NETPACKETQUEUE              Queue;
    NET_RING_COLLECTION const  *Rings;
    NET_EXTENSION               Va;
    volatile LONG               Notify;
} WQUEUE;

static WQUEUE      s_Tx, s_Rx;
static RXQ_ENT    *s_Rxq;
static ULONG       s_RxHead, s_RxTail, s_RxDrop, s_RxUp, s_TxUp;
static KSPIN_LOCK  s_RxLock;

static void LogFrame(const char *Dir, const UCHAR *F, ULONG Len, ULONG N)
{
    if (N <= 8 && Len >= 32) {
        WLOG("%s #%u: fc %02x%02x len %u type %02x%02x\r\n", Dir, N, F[0], F[1], Len, F[30], F[31]);
    }
}

/* core -> here (modem thread): copy, then poke the Rx queue */
extern "C" VOID WlanOnRxFrame(const UCHAR *Frame, ULONG Len)
{
    KIRQL irql;
    BOOLEAN queued = FALSE;

    if (s_Rxq == nullptr || Len < 24 || Len > FRAME_MAX) {
        return;
    }
    KeAcquireSpinLock(&s_RxLock, &irql);
    if (s_RxTail - s_RxHead < RXQ_N) {
        RXQ_ENT *e = &s_Rxq[s_RxTail % RXQ_N];
        e->Len = (USHORT)Len;
        RtlCopyMemory(e->Data, Frame, Len);
        e->Data[1] &= ~0x40;                      /* decrypted by the hardware: clear Protected */
        s_RxTail++;
        queued = TRUE;
    } else {
        s_RxDrop++;
    }
    KeReleaseSpinLock(&s_RxLock, irql);
    if (queued && s_Rx.Queue != nullptr && s_Rx.Notify) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(s_Rx.Queue);
    }
}

static void EvtRxAdvance(NETPACKETQUEUE Queue)
{
    NET_RING *pr = NetRingCollectionGetPacketRing(s_Rx.Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(s_Rx.Rings);
    KIRQL irql;

    UNREFERENCED_PARAMETER(Queue);
    while (pr->NextIndex != pr->EndIndex && fr->NextIndex != fr->EndIndex) {
        RXQ_ENT *e = nullptr;
        KeAcquireSpinLock(&s_RxLock, &irql);
        if (s_RxHead != s_RxTail) {
            e = &s_Rxq[s_RxHead % RXQ_N];
        }
        KeReleaseSpinLock(&s_RxLock, irql);
        if (e == nullptr) {
            break;
        }
        UINT32 fi = fr->NextIndex;
        NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
        UCHAR *va = static_cast<UCHAR *>(NetExtensionGetFragmentVirtualAddress(&s_Rx.Va, fi)->VirtualAddress);
        ULONG len = min((ULONG)e->Len, (ULONG)frag->Capacity);
        RtlCopyMemory(va + frag->Offset, e->Data, len);
        frag->ValidLength = len;
        NET_PACKET *pk = NetRingGetPacketAtIndex(pr, pr->NextIndex);
        RtlZeroMemory(&pk->Layout, sizeof(pk->Layout));
        pk->Layout.Layer2Type = NetPacketLayer2TypeIeee80211;
        pk->Layout.Layer2HeaderLength = 24;
        pk->FragmentIndex = fi;
        pk->FragmentCount = 1;
        pk->Ignore = 0;
        LogFrame("rx up", e->Data, len, ++s_RxUp);
        fr->NextIndex = NetRingIncrementIndex(fr, fi);
        pr->NextIndex = NetRingIncrementIndex(pr, pr->NextIndex);
        KeAcquireSpinLock(&s_RxLock, &irql);
        s_RxHead++;
        KeReleaseSpinLock(&s_RxLock, irql);
    }
    pr->BeginIndex = pr->NextIndex;
    fr->BeginIndex = fr->NextIndex;
}

static void EvtRxSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    InterlockedExchange(&s_Rx.Notify, Enabled ? 1 : 0);
    if (Enabled && s_RxHead != s_RxTail) {
        NetRxQueueNotifyMoreReceivedPacketsAvailable(s_Rx.Queue);
    }
}

/* Cancel: hand every buffer back unused */
static void EvtRxCancel(NETPACKETQUEUE Queue)
{
    NET_RING *pr = NetRingCollectionGetPacketRing(s_Rx.Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(s_Rx.Rings);
    UINT32 i;

    UNREFERENCED_PARAMETER(Queue);
    for (i = pr->NextIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i)) {
        NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
    }
    pr->BeginIndex = pr->NextIndex = pr->EndIndex;
    fr->BeginIndex = fr->NextIndex = fr->EndIndex;
}

/* Tx: gather each packet's fragments, queue it for HTT, complete at once */
static void EvtTxAdvance(NETPACKETQUEUE Queue)
{
    static UCHAR frame[FRAME_MAX];                /* one Advance at a time per queue */
    NET_RING *pr = NetRingCollectionGetPacketRing(s_Tx.Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(s_Tx.Rings);

    UNREFERENCED_PARAMETER(Queue);
    while (pr->NextIndex != pr->EndIndex) {
        NET_PACKET *pk = NetRingGetPacketAtIndex(pr, pr->NextIndex);
        if (!pk->Ignore) {
            ULONG len = 0;
            for (UINT32 k = 0; k < pk->FragmentCount; k++) {
                UINT32 fi = NetRingAdvanceIndex(fr, pk->FragmentIndex, k);
                NET_FRAGMENT *frag = NetRingGetFragmentAtIndex(fr, fi);
                UCHAR *va = static_cast<UCHAR *>(NetExtensionGetFragmentVirtualAddress(&s_Tx.Va, fi)->VirtualAddress);
                if (len + frag->ValidLength > FRAME_MAX) {
                    len = 0;
                    break;
                }
                RtlCopyMemory(frame + len, va + frag->Offset, (SIZE_T)frag->ValidLength);
                len += (ULONG)frag->ValidLength;
            }
            if (len != 0) {
                LogFrame("tx", frame, len, ++s_TxUp);
                WlanTxQueue(frame, len);
            }
        }
        if (pk->FragmentCount != 0) {
            fr->NextIndex = NetRingAdvanceIndex(fr, pk->FragmentIndex, pk->FragmentCount);
        }
        pr->NextIndex = NetRingIncrementIndex(pr, pr->NextIndex);
    }
    pr->BeginIndex = pr->NextIndex;
    fr->BeginIndex = fr->NextIndex;
}

static void EvtTxSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    InterlockedExchange(&s_Tx.Notify, Enabled ? 1 : 0);
}

static void EvtTxCancel(NETPACKETQUEUE Queue)
{
    NET_RING *pr = NetRingCollectionGetPacketRing(s_Tx.Rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(s_Tx.Rings);

    UNREFERENCED_PARAMETER(Queue);
    pr->BeginIndex = pr->NextIndex = pr->EndIndex;
    fr->BeginIndex = fr->NextIndex = fr->EndIndex;
}

static void VaQuery(NET_EXTENSION_QUERY *Q)
{
    NET_EXTENSION_QUERY_INIT(Q, NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,
                             NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1, NetExtensionTypeFragment);
}

static NTSTATUS EvtCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG config;
    NET_EXTENSION_QUERY q;
    NETPACKETQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtTxAdvance, EvtTxSetNotificationEnabled, EvtTxCancel);
    status = NetTxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &config, &queue);
    if (NT_SUCCESS(status)) {
        s_Tx.Queue = queue;
        s_Tx.Rings = NetTxQueueGetRingCollection(queue);
        VaQuery(&q);
        NetTxQueueGetExtension(queue, &q, &s_Tx.Va);
    }
    WLOG("NetTxQueueCreate: %08x\r\n", status);
    return status;
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG config;
    NET_EXTENSION_QUERY q;
    NETPACKETQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);
    if (s_Rxq == nullptr) {
        KeInitializeSpinLock(&s_RxLock);
        s_Rxq = static_cast<RXQ_ENT *>(ExAllocatePool2(POOL_FLAG_NON_PAGED, RXQ_N * sizeof(RXQ_ENT), TOPAZ_WIFI_TAG));
    }
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtRxAdvance, EvtRxSetNotificationEnabled, EvtRxCancel);
    status = NetRxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &config, &queue);
    if (NT_SUCCESS(status)) {
        s_Rx.Rings = NetRxQueueGetRingCollection(queue);
        VaQuery(&q);
        NetRxQueueGetExtension(queue, &q, &s_Rx.Va);
        s_Rx.Queue = queue;
    }
    WLOG("NetRxQueueCreate: %08x (ring %p)\r\n", status, s_Rxq);
    return status;
}

static void EvtSetReceiveFilter(NETADAPTER Adapter, NETRECEIVEFILTER Handle)
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(Handle);
}

/* Default (station) adapter, created by WiFiCx after PrepareHardware succeeded. */
NTSTATUS EvtWifiCreateAdapter(WDFDEVICE Device, NETADAPTER_INIT *AdapterInit)
{
    NET_ADAPTER_DATAPATH_CALLBACKS datapath;
    NET_ADAPTER_TX_CAPABILITIES txCaps;
    NET_ADAPTER_RX_CAPABILITIES rxCaps;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES linkCaps;
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES filterCaps;
    NET_ADAPTER_LINK_LAYER_ADDRESS address;
    NET_ADAPTER_LINK_STATE linkState;
    NETADAPTER adapter;
    NTSTATUS status;

    WLOG("CreateAdapter: type %u\r\n", (ULONG)WifiAdapterInitGetType(AdapterInit));
    if (WifiAdapterInitGetType(AdapterInit) != WIFI_ADAPTER_EXTENSIBLE_STATION) {
        return STATUS_NOT_SUPPORTED;
    }
    NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&datapath, EvtCreateTxQueue, EvtCreateRxQueue);
    NetAdapterInitSetDatapathCallbacks(AdapterInit, &datapath);

    status = NetAdapterCreate(AdapterInit, WDF_NO_OBJECT_ATTRIBUTES, &adapter);
    WLOG("NetAdapterCreate: %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WifiAdapterInitialize(adapter);
    WLOG("WifiAdapterInitialize: %08x, port %u\r\n", status, (ULONG)WifiAdapterGetPortId(adapter));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    NET_ADAPTER_TX_CAPABILITIES_INIT(&txCaps, 1);
    NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&rxCaps, RX_BUFFER_SIZE, 1);
    NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&linkCaps, MAX_LINK_SPEED, MAX_LINK_SPEED);
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&filterCaps, EvtSetReceiveFilter);
    filterCaps.SupportedPacketFilters = NetPacketFilterFlagDirected | NetPacketFilterFlagMulticast |
                                        NetPacketFilterFlagBroadcast | NetPacketFilterFlagAllMulticast;
    filterCaps.MaximumMulticastAddresses = 32;
    NetAdapterSetLinkLayerCapabilities(adapter, &linkCaps);
    NetAdapterSetLinkLayerMtuSize(adapter, MTU_SIZE);
    NetAdapterSetDataPathCapabilities(adapter, &txCaps, &rxCaps);
    NetAdapterSetReceiveFilterCapabilities(adapter, &filterCaps);

    NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&address, sizeof(g_WifiMac), g_WifiMac);
    NetAdapterSetPermanentLinkLayerAddress(adapter, &address);
    NetAdapterSetCurrentLinkLayerAddress(adapter, &address);

    NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&linkState);
    NetAdapterSetLinkState(adapter, &linkState);

    status = NetAdapterStart(adapter);
    WLOG("NetAdapterStart: %08x\r\n", status);
    if (NT_SUCCESS(status)) {
        WifiDev(Device)->StaAdapter = adapter;
    }
    return status;
}

/* No Wi-Fi Direct (NumberOfNetworkInterfaces = 1 in the INF); like the WDK sample, succeed
   without creating anything. */
NTSTATUS EvtWifiCreateWifiDirectDevice(WDFDEVICE Device, WIFIDIRECT_DEVICE_INIT *Init)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Init);
    WLOG("CreateWifiDirectDevice: ignored (no Wi-Fi Direct)\r\n");
    return STATUS_SUCCESS;
}
