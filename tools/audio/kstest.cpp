// kstest: talk KS directly to the TopazSpeaker filter (\\?\root#media#0000#{audio}\speaker0):
// pin count / data flow / communication / data ranges, data intersection and pin creation with
// 48 kHz 16-bit stereo. Prints every step (ACX endpoint bring-up check).
// usage: kstest [filter path]
#include <windows.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <stdio.h>

static HANDLE g_F;

static BOOL Prop(const GUID &set, ULONG id, ULONG pin, void *out, ULONG outLen, ULONG *got, BOOL isPin)
{
    KSP_PIN kp = {};
    kp.Property.Set = set;
    kp.Property.Id = id;
    kp.Property.Flags = KSPROPERTY_TYPE_GET;
    kp.PinId = pin;
    DWORD n = 0;
    BOOL ok = DeviceIoControl(g_F, IOCTL_KS_PROPERTY, &kp, isPin ? sizeof(kp) : sizeof(KSPROPERTY), out, outLen, &n, nullptr);
    if (got) *got = n;
    if (!ok) printf("  prop %lu pin %lu: error %lu (needed %lu)\n", id, pin, GetLastError(), n);
    return ok;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "\\\\?\\root#media#0000#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\speaker0";
    g_F = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    printf("open %s: %p (error %lu)\n", path, g_F, GetLastError());
    if (g_F == INVALID_HANDLE_VALUE) return 1;
    ULONG pins = 0, got = 0;
    Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, 0, &pins, sizeof(pins), &got, FALSE);
    printf("pin types: %lu\n", pins);
    for (ULONG p = 0; p < pins && p < 8; p++) {
        KSPIN_DATAFLOW flow = (KSPIN_DATAFLOW)0;
        KSPIN_COMMUNICATION com = (KSPIN_COMMUNICATION)0;
        GUID cat = {};
        Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, p, &flow, sizeof(flow), &got, TRUE);
        Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION, p, &com, sizeof(com), &got, TRUE);
        Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_CATEGORY, p, &cat, sizeof(cat), &got, TRUE);
        printf("pin %lu: flow %d com %d category %08lx-...\n", p, flow, com, cat.Data1);
        KSPIN_CINSTANCES ci = {};
        if (Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_CINSTANCES, p, &ci, sizeof(ci), &got, TRUE))
            printf("  instances possible %lu current %lu\n", ci.PossibleCount, ci.CurrentCount);
        if (Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_GLOBALCINSTANCES, p, &ci, sizeof(ci), &got, TRUE))
            printf("  global instances possible %lu current %lu\n", ci.PossibleCount, ci.CurrentCount);
        BYTE ib[512] = {};
        if (Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_INTERFACES, p, ib, sizeof(ib), &got, TRUE)) {
            KSMULTIPLE_ITEM *mi = (KSMULTIPLE_ITEM *)ib;
            KSIDENTIFIER *id = (KSIDENTIFIER *)(mi + 1);
            for (ULONG i = 0; i < mi->Count && i < 8; i++) printf("  interface %08lx-.. id %lu\n", id[i].Set.Data1, id[i].Id);
        }
        if (Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_MEDIUMS, p, ib, sizeof(ib), &got, TRUE)) {
            KSMULTIPLE_ITEM *mi = (KSMULTIPLE_ITEM *)ib;
            KSIDENTIFIER *id = (KSIDENTIFIER *)(mi + 1);
            for (ULONG i = 0; i < mi->Count && i < 8; i++) printf("  medium %08lx-.. id %lu\n", id[i].Set.Data1, id[i].Id);
        }
        BYTE buf[4096] = {};
        if (Prop(KSPROPSETID_Pin, KSPROPERTY_PIN_DATARANGES, p, buf, sizeof(buf), &got, TRUE)) {
            KSMULTIPLE_ITEM *mi = (KSMULTIPLE_ITEM *)buf;
            printf("  data ranges: %lu items, %lu bytes\n", mi->Count, mi->Size);
            BYTE *r = (BYTE *)(mi + 1);
            for (ULONG i = 0; i < mi->Count && i < 8; i++) {
                KSDATARANGE *dr = (KSDATARANGE *)r;
                if (dr->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                    KSDATARANGE_AUDIO *a = (KSDATARANGE_AUDIO *)dr;
                    printf("   range %lu: size %lu ch %lu bits %lu..%lu rate %lu..%lu spec %08lx\n", i, dr->FormatSize,
                           a->MaximumChannels, a->MinimumBitsPerSample, a->MaximumBitsPerSample,
                           a->MinimumSampleFrequency, a->MaximumSampleFrequency, dr->Specifier.Data1);
                } else {
                    printf("   range %lu: size %lu\n", i, dr->FormatSize);
                }
                r += (dr->FormatSize + 7) & ~7;
            }
        }
    }
    // try creating pin 0: interface STREAMING / LOOPED, format EXTENSIBLE / plain WAVEFORMATEX
    for (int variant = 0; variant < 4; variant++) {
        struct {
            KSPIN_CONNECT c;
            KSDATAFORMAT_WAVEFORMATEXTENSIBLE f;
        } req = {};
        BOOL looped = (variant & 1) != 0, ext = (variant & 2) == 0;
        req.c.Interface.Set = KSINTERFACESETID_Standard;
        req.c.Interface.Id = looped ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;
        req.c.Medium.Set = KSMEDIUMSETID_Standard;
        req.c.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        req.c.PinId = 0;
        req.c.Priority.PriorityClass = KSPRIORITY_NORMAL;
        req.c.Priority.PrioritySubClass = 1;
        req.f.DataFormat.FormatSize = ext ? sizeof(req.f) : sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX);
        req.f.DataFormat.SampleSize = 4;
        req.f.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        req.f.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        req.f.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        req.f.WaveFormatExt.Format.wFormatTag = ext ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
        req.f.WaveFormatExt.Format.nChannels = 2;
        req.f.WaveFormatExt.Format.nSamplesPerSec = 48000;
        req.f.WaveFormatExt.Format.nAvgBytesPerSec = 192000;
        req.f.WaveFormatExt.Format.nBlockAlign = 4;
        req.f.WaveFormatExt.Format.wBitsPerSample = 16;
        req.f.WaveFormatExt.Format.cbSize = ext ? 22 : 0;
        req.f.WaveFormatExt.Samples.wValidBitsPerSample = 16;
        req.f.WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
        req.f.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        if (variant == 0) {
            KSP_PIN kp = {};
            kp.Property.Set = KSPROPSETID_Pin;
            kp.Property.Id = KSPROPERTY_PIN_PROPOSEDATAFORMAT;
            kp.Property.Flags = KSPROPERTY_TYPE_SET;
            kp.PinId = 0;
            DWORD n = 0;
            BOOL ok = DeviceIoControl(g_F, IOCTL_KS_PROPERTY, &kp, sizeof(kp), &req.f, sizeof(req.f), &n, nullptr);
            printf("PROPOSEDATAFORMAT 48k/16/2 ext: %d (error %lu)\n", ok, ok ? 0 : GetLastError());
        }
        HANDLE pin = nullptr;
        DWORD r = KsCreatePin(g_F, &req.c, GENERIC_WRITE, &pin);
        printf("KsCreatePin(%s, %s): %lu\n", looped ? "LOOPED" : "STREAMING", ext ? "EXTENSIBLE" : "PCM", r);
        if (r == 0) {
            KSSTATE st = KSSTATE_ACQUIRE;
            KSPROPERTY sp = { KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET };
            DWORD n = 0;
            BOOL ok = DeviceIoControl(pin, IOCTL_KS_PROPERTY, &sp, sizeof(sp), &st, sizeof(st), &n, nullptr);
            printf("  state ACQUIRE: %d (error %lu)\n", ok, ok ? 0 : GetLastError());
            CloseHandle(pin);
        }
    }
    CloseHandle(g_F);
    return 0;
}
