/*
 * Station NetAdapter (WDK wificx sample adapter.cpp + netvadapterlibrary ConfigureDataCapabilities).
 * Stage A has no data path yet: the Tx queue returns every packet at once (dropped), the Rx queue
 * never indicates anything and hands its buffers back on cancel. Media stays disconnected.
 */
#include "wpch.h"

#define MTU_SIZE          1500
#define MAX_LINK_SPEED    433000000ULL          /* 1x1 VHT80 MCS9 */
#define RX_BUFFER_SIZE    2048

/* ---- Tx: drop everything ---- */

static void ReturnAll(NETPACKETQUEUE Queue, BOOLEAN Rx)
{
    NET_RING_COLLECTION const *rings = Rx ? NetRxQueueGetRingCollection(Queue) : NetTxQueueGetRingCollection(Queue);
    NET_RING *pr = NetRingCollectionGetPacketRing(rings);
    NET_RING *fr = NetRingCollectionGetFragmentRing(rings);
    UINT32 i;

    if (Rx) {
        for (i = pr->BeginIndex; i != pr->EndIndex; i = NetRingIncrementIndex(pr, i)) {
            NetRingGetPacketAtIndex(pr, i)->Ignore = 1;
        }
    }
    pr->BeginIndex = pr->NextIndex = pr->EndIndex;
    fr->BeginIndex = fr->NextIndex = fr->EndIndex;
}

static void EvtTxAdvance(NETPACKETQUEUE Queue)
{
    ReturnAll(Queue, FALSE);
}

static void EvtTxSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);
}

static void EvtTxCancel(NETPACKETQUEUE Queue)
{
    ReturnAll(Queue, FALSE);
}

/* ---- Rx: nothing yet ---- */

static void EvtRxAdvance(NETPACKETQUEUE Queue)
{
    UNREFERENCED_PARAMETER(Queue);
}

static void EvtRxSetNotificationEnabled(NETPACKETQUEUE Queue, BOOLEAN Enabled)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Enabled);
}

static void EvtRxCancel(NETPACKETQUEUE Queue)
{
    ReturnAll(Queue, TRUE);
}

static NTSTATUS EvtCreateTxQueue(NETADAPTER Adapter, NETTXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG config;
    NETPACKETQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtTxAdvance, EvtTxSetNotificationEnabled, EvtTxCancel);
    status = NetTxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &config, &queue);
    WLOG("NetTxQueueCreate: %08x\r\n", status);
    return status;
}

static NTSTATUS EvtCreateRxQueue(NETADAPTER Adapter, NETRXQUEUE_INIT *Init)
{
    NET_PACKET_QUEUE_CONFIG config;
    NETPACKETQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Adapter);
    NET_PACKET_QUEUE_CONFIG_INIT(&config, EvtRxAdvance, EvtRxSetNotificationEnabled, EvtRxCancel);
    status = NetRxQueueCreate(Init, WDF_NO_OBJECT_ATTRIBUTES, &config, &queue);
    WLOG("NetRxQueueCreate: %08x\r\n", status);
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
