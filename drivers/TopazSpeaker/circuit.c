/*
 * Render circuit "Speaker0": host pin (sink, KSCATEGORY_AUDIO) -> volume -> mute -> bridge pin
 * (KSNODETYPE_SPEAKER, integrated jack). Formats: 48 kHz 16-bit stereo (the DSP takes 2 channels on
 * RX_CODEC_DMA_RX_1; RX2 = left feeds the mono speaker) and 48 kHz 16-bit mono.
 * Modeled on Windows-driver-samples audio/Acx/Samples/Common/RenderCircuit.cpp, rewritten in C.
 */
#include <initguid.h>
#include "tspk.h"

/* {5C8F2B61-3D0E-4F7A-9E51-6225A0D1C4B7} */
DEFINE_GUID(TSPK_COMPONENT_GUID, 0x5c8f2b61, 0x3d0e, 0x4f7a, 0x9e, 0x51, 0x62, 0x25, 0xa0, 0xd1, 0xc4, 0xb7);
DECLARE_CONST_UNICODE_STRING(SpkCircuitName, L"Speaker0");

enum { PinHost = 0, PinBridge, PinCount };
enum { ElemVolume = 0, ElemMute, ElemCount };

typedef struct { WDFDEVICE Device; } SPK_ELEM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SPK_ELEM_CONTEXT, SpkGetElem)

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE g_Fmt48c2 = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, 2, 48000, 192000, 4, 16, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
      { 16 }, KSAUDIO_SPEAKER_STEREO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE g_Fmt48c1 = {
    { sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE), 0, 0, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    { { WAVE_FORMAT_EXTENSIBLE, 1, 48000, 96000, 2, 16, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) },
      { 16 }, KSAUDIO_SPEAKER_MONO, STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM) }
};

/* ---------------- volume / mute: one value for all channels (mono speaker) ---------------- */

static EVT_ACX_RAMPED_VOLUME_ASSIGN_LEVEL SpkVolumeAssign;
static NTSTATUS SpkVolumeAssign(ACXVOLUME Volume, ULONG Channel, LONG VolumeLevel,
                                ACX_VOLUME_CURVE_TYPE CurveType, ULONGLONG CurveDuration)
{
    WDFDEVICE dev = SpkGetElem(Volume)->Device;
    SPK_DEVICE_CONTEXT *ctx = SpkGetDevice(dev);

    UNREFERENCED_PARAMETER(Channel);
    UNREFERENCED_PARAMETER(CurveType);
    UNREFERENCED_PARAMETER(CurveDuration);
    ctx->VolumeLevel = max(SPK_VOL_MIN, min(SPK_VOL_MAX, VolumeLevel));
    HwSetVolume(dev, ctx->VolumeLevel, ctx->Mute != 0);
    return STATUS_SUCCESS;
}

static EVT_ACX_VOLUME_RETRIEVE_LEVEL SpkVolumeRetrieve;
static NTSTATUS SpkVolumeRetrieve(ACXVOLUME Volume, ULONG Channel, LONG *VolumeLevel)
{
    WDFDEVICE dev = SpkGetElem(Volume)->Device;

    UNREFERENCED_PARAMETER(Channel);
    *VolumeLevel = SpkGetDevice(dev)->VolumeLevel;
    return STATUS_SUCCESS;
}

static EVT_ACX_MUTE_ASSIGN_STATE SpkMuteAssign;
static NTSTATUS SpkMuteAssign(ACXMUTE Mute, ULONG Channel, ULONG State)
{
    WDFDEVICE dev = SpkGetElem(Mute)->Device;
    SPK_DEVICE_CONTEXT *ctx = SpkGetDevice(dev);

    UNREFERENCED_PARAMETER(Channel);
    ctx->Mute = State;
    HwSetVolume(dev, ctx->VolumeLevel, ctx->Mute != 0);
    return STATUS_SUCCESS;
}

static EVT_ACX_MUTE_RETRIEVE_STATE SpkMuteRetrieve;
static NTSTATUS SpkMuteRetrieve(ACXMUTE Mute, ULONG Channel, ULONG *State)
{
    WDFDEVICE dev = SpkGetElem(Mute)->Device;

    UNREFERENCED_PARAMETER(Channel);
    *State = SpkGetDevice(dev)->Mute;
    return STATUS_SUCCESS;
}

/* ---------------- circuit callbacks ---------------- */

static EVT_ACX_CIRCUIT_CREATE_STREAM SpkEvtCreateStream;
static NTSTATUS SpkEvtCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN Pin, PACXSTREAM_INIT StreamInit,
                                   ACXDATAFORMAT StreamFormat, const GUID *SignalProcessingMode,
                                   ACXOBJECTBAG VarArguments)
{
    UNREFERENCED_PARAMETER(SignalProcessingMode);
    UNREFERENCED_PARAMETER(VarArguments);
    return SpkCreateStream(Device, Circuit, Pin, StreamInit, StreamFormat);
}

static EVT_ACX_PIN_SET_DATAFORMAT SpkEvtPinSetFormat;
static NTSTATUS SpkEvtPinSetFormat(ACXPIN Pin, ACXDATAFORMAT DataFormat)
{
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(DataFormat);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS AddFormat(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXDATAFORMATLIST List,
                          KSDATAFORMAT_WAVEFORMATEXTENSIBLE *Wave)
{
    ACX_DATAFORMAT_CONFIG cfg;
    WDF_OBJECT_ATTRIBUTES attr;
    ACXDATAFORMAT fmt;
    NTSTATUS status;

    ACX_DATAFORMAT_CONFIG_INIT_KS(&cfg, Wave);
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = Circuit;
    status = AcxDataFormatCreate(Device, &attr, &cfg, &fmt);
    if (NT_SUCCESS(status)) {
        status = AcxDataFormatListAddDataFormat(List, fmt);
    }
    return status;
}

NTSTATUS SpkCreateRenderCircuit(WDFDEVICE Device, ACXCIRCUIT *Circuit)
{
    PACXCIRCUIT_INIT init;
    ACXCIRCUIT circuit;
    WDF_OBJECT_ATTRIBUTES attr;
    ACXELEMENT elements[ElemCount];
    ACXPIN pins[PinCount];
    ACX_VOLUME_CALLBACKS volCb;
    ACX_VOLUME_CONFIG volCfg;
    ACX_MUTE_CALLBACKS muteCb;
    ACX_MUTE_CONFIG muteCfg;
    ACX_PIN_CALLBACKS pinCb;
    ACX_PIN_CONFIG pinCfg;
    ACX_JACK_CONFIG jackCfg;
    ACXJACK jack;
    ACXDATAFORMATLIST list;
    NTSTATUS status;

    *Circuit = NULL;
    init = AcxCircuitInitAllocate(Device);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    AcxCircuitInitSetComponentId(init, &TSPK_COMPONENT_GUID);
    (VOID)AcxCircuitInitAssignName(init, &SpkCircuitName);
    AcxCircuitInitSetCircuitType(init, AcxCircuitTypeRender);
    status = AcxCircuitInitAssignAcxCreateStreamCallback(init, SpkEvtCreateStream);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        return status;
    }
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    status = AcxCircuitCreate(Device, &attr, &init, &circuit);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(init);
        LogPrint("AcxCircuitCreate: %08x\r\n", status);
        return status;
    }

    /* elements */
    ACX_VOLUME_CALLBACKS_INIT(&volCb);
    volCb.EvtAcxRampedVolumeAssignLevel = SpkVolumeAssign;
    volCb.EvtAcxVolumeRetrieveLevel = SpkVolumeRetrieve;
    ACX_VOLUME_CONFIG_INIT(&volCfg);
    volCfg.ChannelsCount = 2;
    volCfg.Minimum = SPK_VOL_MIN;
    volCfg.Maximum = SPK_VOL_MAX;
    volCfg.SteppingDelta = SPK_VOL_STEP;
    volCfg.Name = &KSAUDFNAME_VOLUME_CONTROL;
    volCfg.Callbacks = &volCb;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SPK_ELEM_CONTEXT);
    attr.ParentObject = circuit;
    status = AcxVolumeCreate(circuit, &attr, &volCfg, (ACXVOLUME *)&elements[ElemVolume]);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxVolumeCreate: %08x\r\n", status);
        return status;
    }
    SpkGetElem(elements[ElemVolume])->Device = Device;
    ACX_MUTE_CALLBACKS_INIT(&muteCb);
    muteCb.EvtAcxMuteAssignState = SpkMuteAssign;
    muteCb.EvtAcxMuteRetrieveState = SpkMuteRetrieve;
    ACX_MUTE_CONFIG_INIT(&muteCfg);
    muteCfg.ChannelsCount = 2;
    muteCfg.Name = &KSAUDFNAME_WAVE_MUTE;
    muteCfg.Callbacks = &muteCb;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, SPK_ELEM_CONTEXT);
    attr.ParentObject = circuit;
    status = AcxMuteCreate(circuit, &attr, &muteCfg, (ACXMUTE *)&elements[ElemMute]);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxMuteCreate: %08x\r\n", status);
        return status;
    }
    SpkGetElem(elements[ElemMute])->Device = Device;
    status = AcxCircuitAddElements(circuit, elements, ElemCount);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* pins */
    ACX_PIN_CALLBACKS_INIT(&pinCb);
    pinCb.EvtAcxPinSetDataFormat = SpkEvtPinSetFormat;
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSink;
    pinCfg.Communication = AcxPinCommunicationSink;
    pinCfg.Category = &KSCATEGORY_AUDIO;
    pinCfg.PinCallbacks = &pinCb;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attr, &pinCfg, &pins[PinHost]);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxPinCreate host: %08x\r\n", status);
        return status;
    }
    ACX_PIN_CONFIG_INIT(&pinCfg);
    pinCfg.Type = AcxPinTypeSource;
    pinCfg.Communication = AcxPinCommunicationNone;
    pinCfg.Category = &KSNODETYPE_SPEAKER;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = circuit;
    status = AcxPinCreate(circuit, &attr, &pinCfg, &pins[PinBridge]);
    if (!NT_SUCCESS(status)) {
        LogPrint("AcxPinCreate bridge: %08x\r\n", status);
        return status;
    }

    ACX_JACK_CONFIG_INIT(&jackCfg);
    jackCfg.Description.ChannelMapping = SPEAKER_FRONT_CENTER;
    jackCfg.Description.Color = 0;          /* RGB(0, 0, 0) */
    jackCfg.Description.ConnectionType = AcxConnTypeAtapiInternal;
    jackCfg.Description.GeoLocation = AcxGeoLocFront;
    jackCfg.Description.GenLocation = AcxGenLocPrimaryBox;
    jackCfg.Description.PortConnection = AcxPortConnIntegratedDevice;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = pins[PinBridge];
    status = AcxJackCreate(pins[PinBridge], &attr, &jackCfg, &jack);
    if (NT_SUCCESS(status)) {
        status = AcxPinAddJacks(pins[PinBridge], &jack, 1);
    }
    if (!NT_SUCCESS(status)) {
        LogPrint("jack: %08x\r\n", status);
        return status;
    }

    list = AcxPinGetRawDataFormatList(pins[PinHost]);
    if (list == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = AddFormat(Device, circuit, list, &g_Fmt48c2);
    if (NT_SUCCESS(status)) {
        status = AddFormat(Device, circuit, list, &g_Fmt48c1);
    }
    /* the same formats for the DEFAULT processing mode: a pin create without a mode attribute (and the
       endpoint builder) asks for DEFAULT, and with only a RAW list ACX fails it with INVALID_PARAMETER */
    if (NT_SUCCESS(status)) {
        ACXDATAFORMATLIST def = NULL;
        status = AcxPinRetrieveModeDataFormatList(pins[PinHost], &AUDIO_SIGNALPROCESSINGMODE_DEFAULT, &def);
        LogPrint("default mode format list: %08x\r\n", status);
        if (NT_SUCCESS(status) && def != NULL) {
            status = AddFormat(Device, circuit, def, &g_Fmt48c2);
            if (NT_SUCCESS(status)) {
                status = AddFormat(Device, circuit, def, &g_Fmt48c1);
            }
        }
    }
    if (NT_SUCCESS(status)) {
        status = AcxCircuitAddPins(circuit, pins, PinCount);
    }
    if (NT_SUCCESS(status)) {
        *Circuit = circuit;
    }
    return status;
}
