/*
 * Interface between the C WLAN core (TopazModem sources: modem boot, GLINK/QRTR, WLFW, CE/HTC/
 * WMI/HTT, scan) and the C++ WiFiCx front end of drivers/TopazWifi. Only plain kernel types here:
 * the C++ side never includes Modem.h/compat.h (EDK2 names would clash with WDF/NetAdapter).
 * Built with TOPAZ_WIFICX defined; the standalone TopazModem driver does not use it.
 */
#pragma once
#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- core -> front end (implemented in C++, called on the modem thread at PASSIVE_LEVEL) ---- */

/* WMI READY + HTT configured: the firmware accepts scans from now on. */
VOID WlanOnReady(_In_reads_(6) const UCHAR *Mac);

/* One beacon or probe response heard during a scan. Body = 802.11 frame body (timestamp,
   beacon interval, capability, IEs); Channel = IEEE channel number; Rssi in dBm. */
VOID WlanOnBss(_In_reads_(6) const UCHAR *Bssid, _In_reads_bytes_(BodyLen) const UCHAR *Body, ULONG BodyLen,
               ULONG Channel, LONG Rssi, BOOLEAN ProbeResponse);

/* Scan finished (Status 0 = completed, else the firmware's reason / failure). */
VOID WlanOnScanDone(LONG Status);

/* Association finished. Status 0 = associated (Aid valid), > 0 = 802.11 status code from the AP,
   < 0 = no answer / firmware failure. Bodies = 802.11 frame bodies (no header). */
VOID WlanOnAssocResult(LONG Status, ULONG Aid, _In_reads_bytes_(ReqLen) const UCHAR *ReqBody, ULONG ReqLen,
                       _In_reads_bytes_(RespLen) const UCHAR *RespBody, ULONG RespLen);

/* Link gone: after WlanDisconnectRequest, or the AP deauthenticated/kicked us (Reason = 802.11 code). */
VOID WlanOnDisconnected(ULONG Reason, BOOLEAN ByPeer);

/* ---- front end -> core (any thread) ---- */

typedef struct _WLAN_CONNECT {
    UCHAR   Bssid[6];
    ULONG   Freq;                                /* MHz */
    UCHAR   Ssid[32];
    ULONG   SsidLen;
    BOOLEAN Rsn;                                 /* WPA2: put our RSN IE (CCMP) in the assoc request */
    UCHAR   Akm;                                 /* RSN AKM suite type: 2 = PSK, 1 = 802.1X */
    ULONG   BodyLen;                             /* the AP's beacon / probe response body */
    UCHAR   Body[1024];
    ULONG   ExtraIeLen;                          /* IEs from Windows for the assoc request */
    UCHAR   ExtraIe[256];
} WLAN_CONNECT;

/* Start an association (the modem thread runs it); the answer is WlanOnAssocResult. */
VOID WlanConnectRequest(_In_ const WLAN_CONNECT *Req);

/* Leave the BSS (deauth, vdev down, peer delete); the answer is WlanOnDisconnected. */
VOID WlanDisconnectRequest(USHORT Reason);

/* TRUE while an association is being set up (scans must wait). */
BOOLEAN WlanIsConnecting(VOID);

/* Key from Windows (WDI_SET_ADD_CIPHER_KEYS), installed on the modem thread (VDEV_INSTALL_KEY);
   Cipher in WMI-TLV values: 2 TKIP, 4 CCMP. A pairwise key also opens the port (AUTHORIZE). */
VOID WlanInstallKey(BOOLEAN Group, ULONG KeyIdx, ULONG Cipher, _In_reads_bytes_(KeyLen) const UCHAR *Key, ULONG KeyLen);

/* Ask for one passive scan over all channels; runs on the modem thread when the firmware is
   ready and no scan is in progress. Results come back through WlanOnBss / WlanOnScanDone. */
VOID WlanScanRequest(VOID);

/* ---- modem bring-up, shared with TopazModem's driver.c ---- */
LONG_PTR ModemPasTest(VOID);                     /* boots the modem and runs the service loop */
extern volatile BOOLEAN gModemStop;
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogSetLazy(BOOLEAN Lazy);
VOID LogPrint(PCSTR Fmt, ...);

#ifdef __cplusplus
}
#endif
