// d3dtest - step B check (docs/P8_gpu.md): D3D11 on the TopazGpuW adapter through our UMD
// (topazgpu_d3d10.dll = Mesa d3d10umd + freedreno). Picks the adapter by its DXGI description,
// creates a device (feature level 10.x), clears a 64x64 render target to a known color, copies it
// to a staging texture and reads pixels back. Exit 0 = the GPU (via freedreno) produced the color.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

int main(void)
{
    IDXGIFactory1 *factory = nullptr;
    IDXGIAdapter1 *adapter = nullptr, *pick = nullptr;
    DXGI_ADAPTER_DESC1 desc;

    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
        printf("CreateDXGIFactory1 failed\n");
        return 1;
    }
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        adapter->GetDesc1(&desc);
        wprintf(L"adapter %u: %s (vendor %04x device %04x)\n", i, desc.Description, desc.VendorId, desc.DeviceId);
        if (!pick && wcsstr(desc.Description, L"Adreno")) {
            pick = adapter;
        } else {
            adapter->Release();
        }
    }
    if (!pick) {
        printf("Adreno adapter not found\n");
        return 2;
    }

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &dev, &got, &ctx);
    printf("D3D11CreateDevice: %08lx, feature level %x\n", (unsigned long)hr, (unsigned)got);
    if (FAILED(hr)) {
        return 3;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr, *staging = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    hr = dev->CreateTexture2D(&td, nullptr, &rt);
    printf("CreateTexture2D(RT): %08lx\n", (unsigned long)hr);
    if (FAILED(hr) || FAILED(dev->CreateRenderTargetView(rt, nullptr, &rtv))) {
        return 4;
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
        printf("CreateTexture2D(staging) failed\n");
        return 5;
    }

    const float color[4] = { 1.0f, 0.5f, 0.25f, 1.0f };    // expect R=ff G=80 B=40 A=ff
    ctx->ClearRenderTargetView(rtv, color);
    ctx->CopyResource(staging, rt);
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    printf("Map: %08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        return 6;
    }
    unsigned p0 = ((unsigned *)m.pData)[0];
    unsigned p1 = ((unsigned *)((char *)m.pData + 63 * m.RowPitch))[63];
    ctx->Unmap(staging, 0);
    bool ok = (p0 & 0xFFFFFF) == 0x4080FF && p1 == p0;
    printf("pixel[0,0] = %08x, pixel[63,63] = %08x (expect ff4080ff) -> %s\n", p0, p1,
           ok ? "the Adreno rendered it through freedreno" : "MISMATCH");
    return ok ? 0 : 7;
}
