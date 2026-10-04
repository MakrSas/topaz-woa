// wasapitest: open the "Topaz Speaker" render endpoint in shared mode, play a 2 s 440 Hz tone and set
// the endpoint volume, printing every HRESULT (TopazSpeaker / ACX bring-up check).
// usage: wasapitest [seconds] [volume 0..1]
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <math.h>

#define CHK(x) do { HRESULT _h = (x); printf("%-60s %08lx\n", #x, (unsigned long)_h); if (FAILED(_h)) return 1; } while (0)

int wmain(int argc, wchar_t **argv)
{
    double secs = argc > 1 ? _wtof(argv[1]) : 2.0, vol = argc > 2 ? _wtof(argv[2]) : -1.0;
    IMMDeviceEnumerator *en = nullptr;
    IMMDeviceCollection *col = nullptr;
    IMMDevice *dev = nullptr;
    UINT n = 0;

    CHK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CHK(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en));
    CHK(en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &col));
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        IMMDevice *d = nullptr;
        IPropertyStore *ps = nullptr;
        PROPVARIANT v;
        DWORD state = 0;
        col->Item(i, &d);
        d->GetState(&state);
        d->OpenPropertyStore(STGM_READ, &ps);
        PropVariantInit(&v);
        ps->GetValue(PKEY_Device_FriendlyName, &v);
        wprintf(L"endpoint %u state %lu: %s\n", i, state, v.pwszVal ? v.pwszVal : L"?");
        if (v.pwszVal && wcsstr(v.pwszVal, L"Topaz") && dev == nullptr) {
            dev = d;
            d->AddRef();
        }
        PropVariantClear(&v);
        ps->Release();
        d->Release();
    }
    if (dev == nullptr) {
        printf("no Topaz endpoint\n");
        return 1;
    }
    if (vol >= 0) {
        IAudioEndpointVolume *ev = nullptr;
        float lv = 0;
        UINT ch = 0;
        CHK(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&ev));
        CHK(ev->GetChannelCount(&ch));
        CHK(ev->SetMasterVolumeLevelScalar((float)vol, nullptr));
        CHK(ev->GetMasterVolumeLevelScalar(&lv));
        printf("channels %u, volume now %.2f\n", ch, lv);
        {
            DWORD mask = 0;
            ev->QueryHardwareSupport(&mask);
            printf("hardware support mask %lx (1 = volume, 2 = mute)\n", mask);
        }
        ev->Release();
    }
    IAudioClient *ac = nullptr;
    IAudioRenderClient *rc = nullptr;
    WAVEFORMATEX *mix = nullptr;
    UINT32 frames = 0, pad = 0;
    CHK(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac));
    CHK(ac->GetMixFormat(&mix));
    printf("mix format: tag %x, %u ch, %lu Hz, %u bits\n", mix->wFormatTag, mix->nChannels, mix->nSamplesPerSec, mix->wBitsPerSample);
    CHK(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr));
    CHK(ac->GetBufferSize(&frames));
    CHK(ac->GetService(__uuidof(IAudioRenderClient), (void **)&rc));
    CHK(ac->Start());
    double ph = 0;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)(secs * 1000)) {
        ac->GetCurrentPadding(&pad);
        UINT32 avail = frames - pad;
        if (avail) {
            BYTE *buf = nullptr;
            if (SUCCEEDED(rc->GetBuffer(avail, &buf))) {
                float *f = (float *)buf;                       /* shared-mode mix format is float */
                for (UINT32 i = 0; i < avail; i++) {
                    float s = (float)(0.2 * sin(ph));
                    ph += 2 * 3.14159265358979 * 440.0 / mix->nSamplesPerSec;
                    for (UINT c = 0; c < mix->nChannels; c++) {
                        *f++ = s;
                    }
                }
                rc->ReleaseBuffer(avail, 0);
            }
        }
        Sleep(10);
    }
    CHK(ac->Stop());
    printf("played %.1f s\n", secs);
    return 0;
}
