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

/* ---- front end -> core (any thread) ---- */

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
