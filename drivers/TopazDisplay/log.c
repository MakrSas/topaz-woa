/*
 * Tiny file logger: C:\TopazDisplay.log (\??\C:\ - ".." is not resolved in NT paths).
 * Readable offline from TWRP (mount.ntfs) when Windows has no input.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include "toplog.h"

static HANDLE    g_LogFile;
static KMUTEX    g_LogLock;  /* not FAST_MUTEX: ZwWriteFile needs PASSIVE_LEVEL */
static BOOLEAN   g_LogInit;
/* Lines logged at IRQL > PASSIVE (PresentDisplayOnly etc. may run at APC/DISPATCH) wait here until the
 * next passive-level log call / LogFlush() writes them out. */
static KSPIN_LOCK g_RingLock;
static CHAR       g_Ring[4096];
static ULONG      g_RingLen;

VOID LogOpen(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\TopazDisplay.log");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    if (!g_LogInit) {
        KeInitializeMutex(&g_LogLock, 0);
        KeInitializeSpinLock(&g_RingLock);
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

VOID LogClose(VOID)
{
    if (g_LogFile != NULL) {
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

static VOID LogFlushPassive(VOID)
{
    IO_STATUS_BLOCK iosb;
    CHAR tmp[sizeof(g_Ring)];
    ULONG len;
    KIRQL irql;

    KeAcquireSpinLock(&g_RingLock, &irql);
    len = g_RingLen;
    RtlCopyMemory(tmp, g_Ring, len);
    g_RingLen = 0;
    KeReleaseSpinLock(&g_RingLock, irql);
    if (len != 0 && g_LogFile != NULL) {
        ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, tmp, len, NULL, NULL);
    }
}

VOID LogFlush(VOID)
{
    if (g_LogFile != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
        LogFlushPassive();
        KeReleaseMutex(&g_LogLock, FALSE);
    }
}

static VOID LogWrite(PCSTR Text, SIZE_T Len)
{
    IO_STATUS_BLOCK iosb;
    KIRQL irql;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "TopazDisplay: %s", Text);
    if (g_LogFile == NULL) {
        return;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        if (KeGetCurrentIrql() <= DISPATCH_LEVEL) {
            KeAcquireSpinLock(&g_RingLock, &irql);
            if (g_RingLen + Len <= sizeof(g_Ring)) {
                RtlCopyMemory(g_Ring + g_RingLen, Text, Len);
                g_RingLen += (ULONG)Len;
            }
            KeReleaseSpinLock(&g_RingLock, irql);
        }
        return;
    }
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    LogFlushPassive();
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, (PVOID)Text, (ULONG)Len, NULL, NULL);
    KeReleaseMutex(&g_LogLock, FALSE);
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR buf[256];
    SIZE_T len = 0;
    va_list ap;

    va_start(ap, Fmt);
    if (NT_SUCCESS(RtlStringCbVPrintfA(buf, sizeof(buf), Fmt, ap)) &&
        NT_SUCCESS(RtlStringCbLengthA(buf, sizeof(buf), &len))) {
        LogWrite(buf, len);
    }
    va_end(ap);
}

VOID LogHex(PCSTR Prefix, const UCHAR *Buf, ULONG Len)
{
    CHAR line[256];
    ULONG i;
    size_t pos;

    RtlStringCbCopyA(line, sizeof(line), Prefix);
    RtlStringCbLengthA(line, sizeof(line), &pos);
    for (i = 0; i < Len && pos + 4 < sizeof(line); i++) {
        RtlStringCbPrintfA(line + pos, sizeof(line) - pos, " %02x", Buf[i]);
        pos += 3;
    }
    RtlStringCbCatA(line, sizeof(line), "\n");
    RtlStringCbLengthA(line, sizeof(line), &pos);
    LogWrite(line, pos);
}

/* Experiment knob: C:\topaz\td.cfg holds "<vot> <hpd>" in decimal (read at every device start). */
VOID TopazReadCfg(_Out_ LONG *Vot, _Out_ LONG *Hpd)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\td.cfg");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[40];
    LONG v[2] = {0, 0};
    ULONG i, k = 0;
    BOOLEAN neg = FALSE, in = FALSE;

    *Vot = (LONG)0x7FFFFFFF;                            /* 0x7FFFFFFF = no override */
    *Hpd = 0x7FFFFFFF;
    RtlZeroMemory(buf, sizeof(buf));
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return;
    }
    if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
        for (i = 0; i <= iosb.Information && k < 2; i++) {
            CHAR c = i < iosb.Information ? buf[i] : ' ';
            if (c >= '0' && c <= '9') {
                v[k] = v[k] * 10 + (c - '0');
                in = TRUE;
            } else if (c == '-' && !in) {
                neg = TRUE;
            } else if (in) {
                if (neg) v[k] = -v[k];
                k++; in = FALSE; neg = FALSE;
                if (k < 2) v[k] = 0;
            }
        }
        if (k >= 1) *Vot = v[0];
        if (k >= 2) *Hpd = v[1];
    }
    ZwClose(h);
}
