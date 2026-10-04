/*
 * Hardware side (ADSP graph, codec, amp). v0.1: stubs - the stream runs on the wall clock, so the
 * ACX plumbing (endpoint, formats, volume, packets, position) can be checked without the DSP.
 */
#include "tspk.h"

NTSTATUS HwStreamPrepare(WDFDEVICE Device, PHYSICAL_ADDRESS RingPa, ULONG RingBytes, ULONG Channels, ULONG Rate)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(RingPa);
    UNREFERENCED_PARAMETER(RingBytes);
    UNREFERENCED_PARAMETER(Channels);
    UNREFERENCED_PARAMETER(Rate);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS HwStreamRun(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS HwStreamPause(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    return STATUS_NOT_IMPLEMENTED;
}

VOID HwStreamRelease(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
}

BOOLEAN HwReadPosition(ULONG *ByteIndex)
{
    *ByteIndex = 0;
    return FALSE;
}

VOID HwSetVolume(WDFDEVICE Device, LONG Level, BOOLEAN Mute)
{
    UNREFERENCED_PARAMETER(Device);
    LogPrint("volume %d.%02u dB%s\r\n", Level / 65536, (ULONG)((Level < 0 ? -Level : Level) % 65536) * 100 / 65536,
             Mute ? " (muted)" : "");
}
