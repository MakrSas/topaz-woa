/*
 * C:\TopazModem.log. ModemOut() takes EDK2 formats (%a, %s = CHAR16*, %lu, %r) so the code
 * ported from UEFI keeps its format strings. In lazy mode (GLINK loop) lines are buffered and
 * written at most every 500 ms from the idle path: slow logging made the WLAN PD miss its
 * ~38 s grace timer in UEFI.
 */
#include "Modem.h"

static HANDLE    g_LogFile;
static KMUTEX    g_LogLock;  /* not FAST_MUTEX: ZwWriteFile needs PASSIVE_LEVEL */
static BOOLEAN   g_LogInit;
static CHAR      g_Buf[0x10000];
static SIZE_T    g_BufLen;
static BOOLEAN   g_Lazy;
static UINT64    g_LastFlush;

VOID LogOpen(VOID)
{
#ifdef TOPAZ_WIFICX
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\TopazWifi.log");
#else
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\TopazModem.log");
#endif
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    if (!g_LogInit) {
        KeInitializeMutex(&g_LogLock, 0);
        g_LogInit = TRUE;
    }
    if (g_LogFile != NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0))) {
        g_LogFile = NULL;
    }
}

static VOID LogWrite(PCSTR Text, SIZE_T Len)
{
    IO_STATUS_BLOCK iosb;

    if (g_LogFile == NULL || Len == 0 || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, (PVOID)Text, (ULONG)Len, NULL, NULL);
    KeReleaseMutex(&g_LogLock, FALSE);
}

static VOID FlushNow(VOID)
{
    LogWrite(g_Buf, g_BufLen);
    g_BufLen = 0;
    g_LastFlush = KeQueryInterruptTime();
}

VOID LogFlush(VOID)
{
    if (g_BufLen != 0 && (!g_Lazy || KeQueryInterruptTime() - g_LastFlush >= 5000000)) {
        FlushNow();
    }
}

VOID LogSetLazy(BOOLEAN Lazy)
{
    g_Lazy = Lazy;
    if (!Lazy) {
        FlushNow();
    }
}

VOID LogClose(VOID)
{
    FlushNow();
    if (g_LogFile != NULL) {
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR buf[256];
    SIZE_T len = 0;
    va_list ap;

    va_start(ap, Fmt);
    if (NT_SUCCESS(RtlStringCbVPrintfA(buf, sizeof(buf), Fmt, ap)) &&
        NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &len))) {
        LogWrite(buf, len);       /* other threads: never touch the modem thread's buffer */
    }
    va_end(ap);
}

/* EDK2 PrintLib format -> MSVC: %a -> %s, %s -> %ws, %l? -> %ll?, %r -> %llx (EFI_STATUS) */
static VOID EdkFormat(PCSTR In, PSTR Out, SIZE_T Max)
{
    SIZE_T o = 0;

    while (*In != 0 && o + 5 < Max) {
        BOOLEAN isLong = FALSE;
        if (*In != '%') {
            Out[o++] = *In++;
            continue;
        }
        Out[o++] = *In++;
        while (*In != 0 && strchr("-+ #0123456789.*", *In) != NULL && o + 5 < Max) {
            Out[o++] = *In++;
        }
        while (*In == 'l' || *In == 'L') {
            isLong = TRUE;
            In++;
        }
        switch (*In) {
        case 'a': Out[o++] = 's'; break;
        case 's': Out[o++] = 'w'; Out[o++] = 's'; break;
        case 'r': Out[o++] = 'l'; Out[o++] = 'l'; Out[o++] = 'x'; break;
        case 'u': case 'd': case 'x': case 'X':
            if (isLong) {
                Out[o++] = 'l';
                Out[o++] = 'l';
            }
            Out[o++] = *In;
            break;
        case 0:
            continue;
        default:  Out[o++] = *In; break;
        }
        In++;
    }
    Out[o] = 0;
}

UINTN AsciiSPrint(CHAR8 *Buf, UINTN Size, CONST CHAR8 *Fmt, ...)
{
    CHAR fmt[256];
    size_t n = 0;
    va_list ap;

    EdkFormat(Fmt, fmt, sizeof(fmt));
    va_start(ap, Fmt);
    RtlStringCbVPrintfA(Buf, Size, fmt, ap);
    va_end(ap);
    RtlStringCbLengthA(Buf, Size, &n);
    return n;
}

VOID ModemOut(CONST CHAR8 *Fmt, ...)
{
    CHAR fmt[256], line[320];
    size_t len = 0;
    va_list ap;

    EdkFormat(Fmt, fmt, sizeof(fmt));
    va_start(ap, Fmt);
    RtlStringCbVPrintfA(line, sizeof(line), fmt, ap);   /* truncation is fine */
    va_end(ap);
    RtlStringCbLengthA(line, sizeof(line), &len);
    if (g_BufLen + len > sizeof(g_Buf)) {
        FlushNow();
    }
    RtlCopyMemory(g_Buf + g_BufLen, line, len);
    g_BufLen += len;
    if (!g_Lazy) {
        FlushNow();
    }
}
