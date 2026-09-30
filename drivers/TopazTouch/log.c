/*
 * Tiny file logger: \SystemRoot\..\TopazTouch.log (C:\TopazTouch.log).
 * Readable offline from TWRP (mount.ntfs) when Windows has no input.
 */
#include "driver.h"

static HANDLE    g_LogFile;
static FAST_MUTEX g_LogLock;
static BOOLEAN   g_LogInit;

VOID LogOpen(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\SystemRoot\\..\\TopazTouch.log");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    if (!g_LogInit) {
        ExInitializeFastMutex(&g_LogLock);
        g_LogInit = TRUE;
    }
    if (g_LogFile != NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
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

static VOID LogWrite(PCSTR Text, SIZE_T Len)
{
    IO_STATUS_BLOCK iosb;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "TopazTouch: %s", Text);
    if (g_LogFile == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    ExAcquireFastMutex(&g_LogLock);
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, (PVOID)Text, (ULONG)Len, NULL, NULL);
    ZwFlushBuffersFile(g_LogFile, &iosb);
    ExReleaseFastMutex(&g_LogLock);
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
