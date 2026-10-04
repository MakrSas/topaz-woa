/*
 * RT packet stream. ACX hands us 2 packets (plain non-paged pool, as in the ACX sample); the DSP
 * (SH_MEM_PULL_MODE) reads its own physically contiguous, non-cached ring of 2 * PacketSize bytes.
 * v0.7: the packets are copied into the ring (SetRenderPacket + every timer tick for the packet the
 * DSP is not reading) - v0.6 let the DSP read the engine's mapping directly and it crackled
 * (CPU cache vs. DSP DMA).
 * Position: a 5 ms timer reads the DSP read index (hw.c) - or, without the DSP, advances by wall
 * clock - accumulates a linear byte count and completes packets with AcxRtStreamNotifyPacketComplete.
 */
#include "tspk.h"

#define SPK_PACKETS   2
#define SPK_TIMER_MS  5

typedef struct {
    ACXSTREAM      Stream;
    WDFDEVICE      Device;
    WDFTIMER       Timer;
    KSPIN_LOCK     Lock;
    ULONG          Channels, Rate, BlockAlign;
    ULONG          PacketSize, Chunk;
    PUCHAR         Va;               /* DSP ring (contiguous, write-combined) */
    PHYSICAL_ADDRESS Pa;
    SIZE_T         AllocBytes;
    PUCHAR         Pkt[SPK_PACKETS]; /* packet allocations (engine side) */
    PUCHAR         PktData[SPK_PACKETS];
    PMDL           Mdl[SPK_PACKETS];
    BOOLEAN        Running, Prepared, HwPos;
    ULONG          LastIndex;        /* last DSP read index (bytes in the ring) */
    ULONGLONG      Linear;           /* bytes consumed since run */
    ULONGLONG      LastQpc, QpcFreq; /* wall-clock fallback */
    ULONG          PacketsDone;      /* completed packets (monotonic) */
} SPK_STREAM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPK_STREAM_CONTEXT, SpkGetStream)

static VOID UpdatePosition(SPK_STREAM_CONTEXT *s, BOOLEAN Notify)
{
    ULONG idx, ring = 2 * s->PacketSize;
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);
    ULONG done;

    if (!s->Running || ring == 0) {
        return;
    }
    if (HwReadPosition(&idx) && idx < ring) {
        s->HwPos = TRUE;
        s->Linear += (idx + ring - s->LastIndex) % ring;
        s->LastIndex = idx;
    } else if (!s->HwPos && s->QpcFreq != 0) {
        ULONGLONG frames = ((ULONGLONG)(qpc.QuadPart - s->LastQpc) * s->Rate) / s->QpcFreq;
        if (frames != 0) {
            s->Linear += frames * s->BlockAlign;
            s->LastQpc += (frames * s->QpcFreq) / s->Rate;
        }
    }
    done = (ULONG)(s->Linear / s->PacketSize);
    while (Notify && s->PacketsDone < done) {
        (VOID)AcxRtStreamNotifyPacketComplete(s->Stream, s->PacketsDone, (ULONGLONG)qpc.QuadPart);
        s->PacketsDone++;
    }
}

static VOID CopyPacket(SPK_STREAM_CONTEXT *s, ULONG k)
{
    if (s->Va != NULL && s->PktData[k] != NULL) {
        RtlCopyMemory(s->Va + (SIZE_T)k * s->PacketSize, s->PktData[k], s->PacketSize);
    }
}

static EVT_WDF_TIMER SpkTimer;
static VOID SpkTimer(WDFTIMER Timer)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(WdfTimerGetParentObject(Timer));
    KIRQL irql;

    KeAcquireSpinLock(&s->Lock, &irql);
    UpdatePosition(s, TRUE);
    if (s->PacketSize != 0) {
        CopyPacket(s, 1 - (s->LastIndex / s->PacketSize) % 2);   /* the half the DSP is not reading */
    }
    KeReleaseSpinLock(&s->Lock, irql);
}

/* ---------------- RT callbacks ---------------- */

static EVT_ACX_STREAM_ALLOCATE_RTPACKETS SpkAllocPackets;
static NTSTATUS SpkAllocPackets(ACXSTREAM Stream, ULONG PacketCount, ULONG PacketSize, PACX_RTPACKET *Packets)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    PHYSICAL_ADDRESS lo, hi, bound;
    PACX_RTPACKET p;
    ULONG i;

    if (PacketCount != SPK_PACKETS || PacketSize == 0 || PacketSize > 0x100000) {
        LogPrint("alloc: %u packets x %u bytes not supported\r\n", PacketCount, PacketSize);
        return STATUS_NOT_SUPPORTED;
    }
    s->Chunk = (PacketSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    s->AllocBytes = ((SIZE_T)2 * PacketSize + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1);
    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;                  /* below 4 GiB for the DSP (32-bit lsw + SID msw) */
    bound.QuadPart = 0;
    s->Va = MmAllocateContiguousMemorySpecifyCache(s->AllocBytes, lo, hi, bound, MmWriteCombined);
    if (s->Va == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(s->Va, s->AllocBytes);
    s->Pa = MmGetPhysicalAddress(s->Va);
    p = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(ACX_RTPACKET) * SPK_PACKETS, TSPK_TAG);
    if (p == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    for (i = 0; i < SPK_PACKETS; i++) {
        s->Pkt[i] = ExAllocatePool2(POOL_FLAG_NON_PAGED, s->Chunk, TSPK_TAG);
        if (s->Pkt[i] == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        s->Mdl[i] = IoAllocateMdl(s->Pkt[i], s->Chunk, FALSE, FALSE, NULL);
        if (s->Mdl[i] == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(s->Mdl[i]);
        ACX_RTPACKET_INIT(&p[i]);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&p[i].RtPacketBuffer, s->Mdl[i], s->Chunk);
        p[i].RtPacketSize = PacketSize;
        p[i].RtPacketOffset = (i == 0) ? s->Chunk - PacketSize : 0;
        s->PktData[i] = s->Pkt[i] + p[i].RtPacketOffset;
    }
    s->PacketSize = PacketSize;
    *Packets = p;
    LogPrint("alloc: 2 x %u bytes, dsp ring pa %llx\r\n", PacketSize, (ULONGLONG)s->Pa.QuadPart);
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_FREE_RTPACKETS SpkFreePackets;
static VOID SpkFreePackets(ACXSTREAM Stream, PACX_RTPACKET Packets, ULONG PacketCount)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    ULONG i;

    UNREFERENCED_PARAMETER(PacketCount);
    for (i = 0; i < SPK_PACKETS; i++) {
        if (s->Mdl[i] != NULL) {
            IoFreeMdl(s->Mdl[i]);
            s->Mdl[i] = NULL;
        }
        if (s->Pkt[i] != NULL) {
            ExFreePoolWithTag(s->Pkt[i], TSPK_TAG);
            s->Pkt[i] = NULL;
            s->PktData[i] = NULL;
        }
    }
    if (s->Va != NULL) {
        MmFreeContiguousMemorySpecifyCache(s->Va, s->AllocBytes, MmWriteCombined);
        s->Va = NULL;
    }
    ExFreePoolWithTag(Packets, TSPK_TAG);
    LogPrint("free packets\r\n");
}

static EVT_ACX_STREAM_GET_HW_LATENCY SpkGetLatency;
static NTSTATUS SpkGetLatency(ACXSTREAM Stream, ULONG *FifoSize, ULONG *Delay)
{
    UNREFERENCED_PARAMETER(Stream);
    *FifoSize = 128;
    *Delay = 0;
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_GET_CURRENT_PACKET SpkGetCurrentPacket;
static NTSTATUS SpkGetCurrentPacket(ACXSTREAM Stream, PULONG CurrentPacket)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    KIRQL irql;

    KeAcquireSpinLock(&s->Lock, &irql);
    UpdatePosition(s, FALSE);
    *CurrentPacket = s->PacketSize ? (ULONG)(s->Linear / s->PacketSize) : 0;
    KeReleaseSpinLock(&s->Lock, irql);
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_GET_PRESENTATION_POSITION SpkGetPresentation;
static NTSTATUS SpkGetPresentation(ACXSTREAM Stream, PULONGLONG PositionInBlocks, PULONGLONG QPCPosition)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    KIRQL irql;

    KeAcquireSpinLock(&s->Lock, &irql);
    UpdatePosition(s, FALSE);
    *PositionInBlocks = s->BlockAlign ? s->Linear / s->BlockAlign : 0;
    KeReleaseSpinLock(&s->Lock, irql);
    *QPCPosition = (ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_SET_RENDER_PACKET SpkSetRenderPacket;
static NTSTATUS SpkSetRenderPacket(ACXSTREAM Stream, ULONG Packet, ULONG Flags, ULONG EosPacketLength)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    KIRQL irql;

    KeAcquireSpinLock(&s->Lock, &irql);
    CopyPacket(s, Packet % SPK_PACKETS);
    KeReleaseSpinLock(&s->Lock, irql);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(EosPacketLength);
    return STATUS_SUCCESS;
}

/* ---------------- state callbacks ---------------- */

static EVT_ACX_STREAM_PREPARE_HARDWARE SpkPrepare;
static NTSTATUS SpkPrepare(ACXSTREAM Stream)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    PHYSICAL_ADDRESS ring;
    NTSTATUS status;

    if (s->Va == NULL) {
        LogPrint("prepare without packets\r\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    ring.QuadPart = s->Pa.QuadPart;
    status = HwStreamPrepare(s->Device, ring, 2 * s->PacketSize, s->Channels, s->Rate);
    LogPrint("prepare: %u ch %u Hz, hw %08x\r\n", s->Channels, s->Rate, status);
    s->Prepared = TRUE;
    return STATUS_SUCCESS;                     /* without the DSP the stream still runs on the clock */
}

static EVT_ACX_STREAM_RELEASE_HARDWARE SpkRelease;
static NTSTATUS SpkRelease(ACXSTREAM Stream)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);

    if (s->Prepared) {
        HwStreamRelease(s->Device);
        s->Prepared = FALSE;
    }
    LogPrint("release\r\n");
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_RUN SpkRun;
static NTSTATUS SpkRun(ACXSTREAM Stream)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    LARGE_INTEGER freq;
    KIRQL irql;
    NTSTATUS hw;

    CopyPacket(s, 0);
    CopyPacket(s, 1);
    hw = HwStreamRun(s->Device);
    KeAcquireSpinLock(&s->Lock, &irql);
    s->LastQpc = (ULONGLONG)KeQueryPerformanceCounter(&freq).QuadPart;
    s->QpcFreq = (ULONGLONG)freq.QuadPart;
    s->HwPos = FALSE;
    s->LastIndex = 0;
    s->Running = TRUE;
    KeReleaseSpinLock(&s->Lock, irql);
    WdfTimerStart(s->Timer, WDF_REL_TIMEOUT_IN_MS(SPK_TIMER_MS));
    LogPrint("run (hw %08x)\r\n", hw);
    return STATUS_SUCCESS;
}

static EVT_ACX_STREAM_PAUSE SpkPause;
static NTSTATUS SpkPause(ACXSTREAM Stream)
{
    SPK_STREAM_CONTEXT *s = SpkGetStream(Stream);
    KIRQL irql;

    WdfTimerStop(s->Timer, TRUE);
    KeAcquireSpinLock(&s->Lock, &irql);
    UpdatePosition(s, FALSE);
    s->Running = FALSE;
    KeReleaseSpinLock(&s->Lock, irql);
    (VOID)HwStreamPause(s->Device);
    LogPrint("pause at %llu bytes, %u packets\r\n", s->Linear, s->PacketsDone);
    return STATUS_SUCCESS;
}

/* ---------------- creation ---------------- */

NTSTATUS SpkCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin, PACXSTREAM_INIT StreamInit,
                         ACXDATAFORMAT StreamFormat)
{
    ACX_STREAM_CALLBACKS cb;
    ACX_RT_STREAM_CALLBACKS rt;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_TIMER_CONFIG tcfg;
    ACXSTREAM stream;
    SPK_STREAM_CONTEXT *s;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Pin);
    LogPrint("create stream\r\n");
    ACX_STREAM_CALLBACKS_INIT(&cb);
    cb.EvtAcxStreamPrepareHardware = SpkPrepare;
    cb.EvtAcxStreamReleaseHardware = SpkRelease;
    cb.EvtAcxStreamRun = SpkRun;
    cb.EvtAcxStreamPause = SpkPause;
    status = AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &cb);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxStreamInitAssignAcxStreamCallbacks: %08x\r\n", status);
        return status;
    }
    ACX_RT_STREAM_CALLBACKS_INIT(&rt);
    rt.EvtAcxStreamGetHwLatency = SpkGetLatency;
    rt.EvtAcxStreamAllocateRtPackets = SpkAllocPackets;
    rt.EvtAcxStreamFreeRtPackets = SpkFreePackets;
    rt.EvtAcxStreamGetCurrentPacket = SpkGetCurrentPacket;
    rt.EvtAcxStreamGetPresentationPosition = SpkGetPresentation;
    rt.EvtAcxStreamSetRenderPacket = SpkSetRenderPacket;
    status = AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rt);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxStreamInitAssignAcxRtStreamCallbacks: %08x\r\n", status);
        return status;
    }
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SPK_STREAM_CONTEXT);
    status = AcxRtStreamCreate(Device, Circuit, &attr, &StreamInit, &stream);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxRtStreamCreate: %08x\r\n", status);
        return status;
    }
    s = SpkGetStream(stream);
    RtlZeroMemory(s, sizeof(*s));
    s->Stream = stream;
    s->Device = Device;
    KeInitializeSpinLock(&s->Lock);
    s->Channels = AcxDataFormatGetChannelsCount(StreamFormat);
    s->Rate = AcxDataFormatGetSampleRate(StreamFormat);
    s->BlockAlign = AcxDataFormatGetBlockAlign(StreamFormat);

    WDF_TIMER_CONFIG_INIT_PERIODIC(&tcfg, SpkTimer, SPK_TIMER_MS);
    tcfg.TolerableDelay = 0;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = stream;
    status = WdfTimerCreate(&tcfg, &attr, &s->Timer);
    LogPrint("stream created: %u ch %u Hz block %u, timer %08x\r\n", s->Channels, s->Rate, s->BlockAlign, status);
    return status;
}
