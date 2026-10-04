/*
 * TopazWifi WiFiCx front end: common includes and shared state. Include order follows the WDK
 * wificx sample (precomp.h). The C WLAN core is reached only through wlanif.h.
 */
#pragma once

#include <ntddk.h>
#include <ntintsafe.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <netiodef.h>
#include <wificx.h>
#include "dot11wificxintf.h"
#include "dot11wificxtypes.hpp"
#include "TlvGeneratorParser.hpp"
#include "wlanif.h"

#define TOPAZ_WIFI_VERSION  "v0.24"
#define TOPAZ_WIFI_TAG      'iWzT'

#define WLOG(...)           LogPrint (__VA_ARGS__)

/* Device context: WDF wants the triage-info pointer first for NetAdapterCx devices (sample). */
typedef struct _TOPAZ_WIFI_DEVICE
{
    void       *WdfTriageInfoPtr;
    WDFDEVICE   Device;
    NETADAPTER  StaAdapter;
    TLV_CONTEXT Tlv;
    PKTHREAD    ModemThread;
} TOPAZ_WIFI_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TOPAZ_WIFI_DEVICE, WifiDev);

/* the one device (root-enumerated, single instance): core callbacks come without a handle */
extern TOPAZ_WIFI_DEVICE *g_Wifi;

/* our station MAC until the persist one is read (locally administered "TOPAZ") */
extern const UCHAR g_WifiMac[6];

/* wcaps.cpp */
NTSTATUS WifiSetCapabilities(_In_ WDFDEVICE Device);

/* wcmd.cpp */
EVT_WIFI_DEVICE_SEND_COMMAND EvtWifiSendCommand;
void SendIndication(const WDI_MESSAGE_HEADER *Orig, UINT16 MessageId, UINT32 TransactionId, NTSTATUS Status,
                    const UCHAR *Tlv, ULONG TlvLen);
void SendM4(const WDI_MESSAGE_HEADER *M1, UINT16 CompleteId, NTSTATUS Status);
void CompleteM3(WIFIREQUEST Request, NTSTATUS Status);

/* wconn.cpp */
void OnConnect(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen);
void OnDisconnect(WIFIREQUEST Request, const WDI_MESSAGE_HEADER *Hdr, const UCHAR *Tlv, ULONG TlvLen);
void OnAddKeys(WIFIREQUEST Request, const UCHAR *Tlv, ULONG TlvLen);

/* wnet.cpp */
EVT_WIFI_DEVICE_CREATE_ADAPTER EvtWifiCreateAdapter;
EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE EvtWifiCreateWifiDirectDevice;

/* NDIS_STATUS from the TLV library -> NTSTATUS (umkmfusion.h ConvertNDISSTATUSToNTSTATUS) */
inline NTSTATUS NdisToNt(NDIS_STATUS S)
{
    if (S == NDIS_STATUS_SUCCESS) {
        return STATUS_SUCCESS;
    }
    if (S == NDIS_STATUS_BUFFER_TOO_SHORT) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    return NT_SUCCESS(S) ? STATUS_UNSUCCESSFUL : (NTSTATUS)S;
}
