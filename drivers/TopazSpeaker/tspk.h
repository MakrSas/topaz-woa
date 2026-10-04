/*
 * TopazSpeaker: Windows audio endpoint (ACX 1.0 render circuit) for the Redmi Note 12 4G speaker.
 *
 * The ADSP is booted and kept alive by TopazAudio; this driver talks to it through TopazAudio's
 * \\.\TopazAudio interface (GPR packets, MMIO in known LPASS blocks, I2C on QUP0 SE1). The audio path
 * is the one found in docs/P9_audio.md: SH_MEM_PULL_MODE reads the ACX packets (one physically
 * contiguous ring) -> CODEC_DMA_SINK RX_CODEC_DMA_RX_1 -> RX macro RX2/INT2 -> SoundWire -> WCD937x
 * AUX -> sia8159 amp. Volume = RX2 digital gain.
 */
#pragma once

#include <ntddk.h>
#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <acx.h>

#define TSPK_VERSION   "v0.7"
#define TSPK_TAG       'kpsT'

/* ---- log.c ---- */
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);

/* ---- circuit.c ---- */
NTSTATUS SpkCreateRenderCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT *Circuit);

/* volume in 1/65536 dB as ACX wants it; the hardware takes whole dB */
#define SPK_VOL_MIN    (-84L * 65536)
#define SPK_VOL_MAX    (0L)
#define SPK_VOL_STEP   (65536L)
#define SPK_VOL_DEF    (-30L * 65536)

typedef struct {
    ACXCIRCUIT  Circuit;
    LONG        VolumeLevel;           /* 1/65536 dB */
    ULONG       Mute;
    BOOLEAN     CodecUp;               /* codec bring-up done once (hw.c) */
} SPK_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPK_DEVICE_CONTEXT, SpkGetDevice)

/* ---- stream.c ---- */
NTSTATUS SpkCreateStream(_In_ WDFDEVICE Device, _In_ ACXCIRCUIT Circuit, _In_ ACXPIN Pin,
                         _In_ PACXSTREAM_INIT StreamInit, _In_ ACXDATAFORMAT StreamFormat);

/* ---- hw.c: everything that touches the ADSP / codec (stubs until the hardware step) ---- */
NTSTATUS HwStreamPrepare(_In_ WDFDEVICE Device, _In_ PHYSICAL_ADDRESS RingPa, _In_ ULONG RingBytes,
                         _In_ ULONG Channels, _In_ ULONG Rate);
NTSTATUS HwStreamRun(_In_ WDFDEVICE Device);
NTSTATUS HwStreamPause(_In_ WDFDEVICE Device);
VOID     HwStreamRelease(_In_ WDFDEVICE Device);
BOOLEAN  HwReadPosition(_Out_ ULONG *ByteIndex);   /* read index in the ring, FALSE if unknown */
VOID     HwSetVolume(_In_ WDFDEVICE Device, _In_ LONG Level, _In_ BOOLEAN Mute);
