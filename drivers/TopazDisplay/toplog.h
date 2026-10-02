/* File logger shared with the other topaz-woa drivers (log.c): C:\TopazDisplay.log */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogFlush(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);
VOID TopazReadCfg(_Out_ LONG *Vot, _Out_ LONG *Hpd);
#ifdef __cplusplus
}
#endif
