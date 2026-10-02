/*
 * Tiny file logger: C:\TopazGpuW.log (\??\C:\ - ".." is not resolved in NT paths).
 * Readable offline from TWRP (mount.ntfs) when Windows has no input.
 *
 * v0.26: every message goes into a ring of fixed slots (any IRQL, also ISR / SynchronizeExecution
 * callbacks: a sequence number from InterlockedIncrement picks the slot) and a passive system
 * thread writes the slots to the file in sequence order. Before v0.26 messages above PASSIVE_LEVEL
 * were dropped silently (SubmitCommand runs at DISPATCH_LEVEL and never showed up in the log).
 * The thread is stopped and awaited in LogClose (a thread outliving the image = bugcheck 0xCE).
 */
#include "tgpu.h"

#define LOG_SLOTS     512
#define LOG_SLOT_LEN  240

typedef struct _LOG_SLOT {
    volatile LONG Seq;                  /* sequence number of the message in Text (0 = empty) */
    USHORT        Len;
    CHAR          Text[LOG_SLOT_LEN];
} LOG_SLOT;

static HANDLE    g_LogFile;
static BOOLEAN   g_LogInit;
static LOG_SLOT  g_Slots[LOG_SLOTS];
static volatile LONG g_LogSeq;          /* last sequence number handed out */
static LONG      g_LogDone;             /* last sequence number written (flusher only) */
static volatile LONG g_LogDropped;
static KEVENT    g_LogKick, g_LogStop;
static PKTHREAD  g_LogThread;

static VOID LogFlush(VOID)
{
    IO_STATUS_BLOCK iosb;
    LONG seq;

    for (;;) {
        seq = g_LogDone + 1;
        if (seq - g_LogSeq > 0) {
            break;                                   /* nothing handed out beyond g_LogDone */
        }
        LOG_SLOT *s = &g_Slots[(ULONG)seq % LOG_SLOTS];
        if (s->Seq != seq) {
            if (seq - s->Seq > 0) {
                break;                               /* writer still copying: next round */
            }
            g_LogDone = seq;                         /* overwritten by a later message: lost */
            continue;
        }
        if (g_LogFile != NULL) {
            ZwWriteFile(g_LogFile, NULL, NULL, NULL, &iosb, s->Text, s->Len, NULL, NULL);
        }
        g_LogDone = seq;
    }
}

static KSTART_ROUTINE LogThread;
static VOID LogThread(PVOID Ctx)
{
    PVOID objs[2] = { &g_LogKick, &g_LogStop };
    LARGE_INTEGER t;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Ctx);
    t.QuadPart = -10000LL * 100;                      /* 100 ms */
    for (;;) {
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &t, NULL);
        LogFlush();
        if (st == STATUS_WAIT_1) {
            break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID LogOpen(VOID)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\TopazGpuW.log");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE th;

    if (!g_LogInit) {
        KeInitializeEvent(&g_LogKick, SynchronizationEvent, FALSE);
        KeInitializeEvent(&g_LogStop, NotificationEvent, FALSE);
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
        return;
    }
    KeClearEvent(&g_LogStop);
    if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, LogThread, NULL))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode,
                                                  (PVOID *)&g_LogThread, NULL))) {
            g_LogThread = NULL;
        }
        ZwClose(th);
    }
}

VOID LogClose(VOID)
{
    if (g_LogThread != NULL) {
        KeSetEvent(&g_LogStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(g_LogThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_LogThread);
        g_LogThread = NULL;
    }
    if (g_LogFile != NULL) {
        LogFlush();
        ZwClose(g_LogFile);
        g_LogFile = NULL;
    }
}

static VOID LogWrite(PCSTR Text, SIZE_T Len)
{
    LONG seq;
    LOG_SLOT *s;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "TopazGpuW: %s", Text);
    if (!g_LogInit) {
        return;
    }
    seq = InterlockedIncrement(&g_LogSeq);
    s = &g_Slots[(ULONG)seq % LOG_SLOTS];
    if (Len > LOG_SLOT_LEN) {
        Len = LOG_SLOT_LEN;
    }
    s->Seq = 0;                                      /* claim: the flusher waits for the final seq */
    RtlCopyMemory(s->Text, Text, Len);
    s->Len = (USHORT)Len;
    MemoryBarrier();
    InterlockedExchange(&s->Seq, seq);
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        KeSetEvent(&g_LogKick, IO_NO_INCREMENT, FALSE);
    }
}

VOID LogPrint(PCSTR Fmt, ...)
{
    CHAR buf[LOG_SLOT_LEN];
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
    CHAR line[LOG_SLOT_LEN];
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
