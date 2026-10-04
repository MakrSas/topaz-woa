/* C:\TopazSpeaker.log (PASSIVE_LEVEL only; lines from higher IRQL are dropped) */
#include "tspk.h"

static HANDLE  g_LogFile;
static KMUTEX  g_LogLock;
static BOOLEAN g_LogInit;

VOID LogOpen(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\TopazSpeaker.log");
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

VOID LogClose(VOID)
{
    if (g_LogFile != NULL) {
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR line[512];
    va_list args;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER t;
    size_t len = 0;

    if (g_LogFile == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    t.QuadPart = (LONGLONG)(KeQueryInterruptTime() / 10000);
    RtlStringCbPrintfA(line, sizeof(line), "%8llu ", (ULONGLONG)t.QuadPart);
    RtlStringCbLengthA(line, sizeof(line), &len);
    va_start(args, Fmt);
    RtlStringCbVPrintfA(line + len, sizeof(line) - len, Fmt, args);
    va_end(args);
    RtlStringCbLengthA(line, sizeof(line), &len);
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, line, (ULONG)len, NULL, NULL);
    KeReleaseMutex(&g_LogLock, FALSE);
}
