// d3dtest - step B check (docs/P8_gpu.md): D3D11 on the TopazGpuW adapter through our UMD
// (topazgpu_d3d10.dll = Mesa d3d10umd + freedreno). Picks the adapter by its DXGI description,
// creates a device (feature level 10.x), clears a 64x64 render target to a known color, copies it
// to a staging texture and reads pixels back. Exit 0 = the GPU (via freedreno) produced the color.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <psapi.h>
#include <d3dcompiler.h>

// Crash report: exception code/PC and the call stack as module+offset (symbolize the UMD frames
// with llvm-symbolizer and the CI PDB).
static void print_addr(void *a)
{
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)a, &m)) {
        GetModuleFileNameA(m, name, sizeof(name));
    }
    const char *base = strrchr(name, '\\');
    printf("  %s+0x%llx\n", base ? base + 1 : name, (unsigned long long)((char *)a - (char *)m));
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep)
{
    printf("EXCEPTION %08lx at", (unsigned long)ep->ExceptionRecord->ExceptionCode);
    print_addr(ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        printf("  access %llu addr %llx\n", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
               (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    // unwind from the faulting context (CaptureStackBackTrace skips the UMD frames)
    CONTEXT c = *ep->ContextRecord;
    for (int i = 0; i < 48 && c.Pc; i++) {
        print_addr((void *)c.Pc);
        DWORD64 image = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Pc, &image, nullptr);
        if (!f) {
            c.Pc = c.Lr;                                // leaf function
            c.Lr = 0;
            continue;
        }
        void *handler_data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Pc, f, &c, &handler_data, &frame, nullptr);
    }
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

// usage: d3dtest [clear|copy] [FD_MESA_DEBUG flags, e.g. sysmem,noubwc]
//   clear: ClearRenderTargetView (freedreno: 3D draw or blit) -> copy to staging -> read
//   copy:  texture created with initial data (CPU upload) -> CopyResource (2D blitter) -> read
int main(int argc, char **argv)
{
    bool copy = argc > 1 && strcmp(argv[1], "copy") == 0;
    // vsonly: a draw whose triangle is completely off screen (VS runs, nothing rasterized)
    // tri:    full-screen triangle, constant-color pixel shader, then read back
    bool vsonly = argc > 1 && strcmp(argv[1], "vsonly") == 0;
    // tricull: tri with the default rasterizer (back-face culling, clockwise = front)
    bool tricull = argc > 1 && strcmp(argv[1], "tricull") == 0;
    bool tri = (argc > 1 && strcmp(argv[1], "tri") == 0) || tricull;
    // nort: like vsonly but with no render target bound at all (RB has no MRT)
    bool nort = argc > 1 && strcmp(argv[1], "nort") == 0;
    vsonly = vsonly || nort;
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetEnvironmentVariableA("TOPAZGPU_ENABLE", "1");  // the UMD refuses other processes
    if (argc > 2) {
        SetEnvironmentVariableA("FD_MESA_DEBUG", argv[2]);
    }
    printf("mode %s, FD_MESA_DEBUG=%s\n", argc > 1 ? argv[1] : "clear", argc > 2 ? argv[2] : "");
    AddVectoredExceptionHandler(1, crash_handler);
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
    static unsigned init[64 * 64];
    for (unsigned i = 0; i < 64 * 64; i++) {
        init[i] = 0xff4080ff;                          // same color the clear would write
    }
    D3D11_SUBRESOURCE_DATA sd = { init, 64 * 4, 0 };
    hr = dev->CreateTexture2D(&td, copy ? &sd : nullptr, &rt);
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
    if (vsonly || tri) {
        // SV_VertexID only: no vertex buffers, no input layout
        static const char hlsl[] =
            "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
            "  float2 p = float2((id << 1) & 2, id & 2);\n"
            "  return float4(p * float2(2, -2) + float2(-1, 1) + OFFSET, 0, 1);\n"
            "}\n"
            "float4 ps() : SV_Target { return float4(1.0, 0.5, 0.25, 1.0); }\n";
        D3D_SHADER_MACRO defs[] = { { "OFFSET", vsonly ? "float2(10, 10)" : "float2(0, 0)" }, { nullptr, nullptr } };
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        hr = D3DCompile(hlsl, sizeof(hlsl) - 1, "t", defs, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
        if (SUCCEEDED(hr)) {
            hr = D3DCompile(hlsl, sizeof(hlsl) - 1, "t", defs, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
        }
        if (FAILED(hr)) {
            printf("D3DCompile: %08lx %s\n", (unsigned long)hr, err ? (const char *)err->GetBufferPointer() : "");
            return 8;
        }
        ID3D11VertexShader *vs = nullptr;
        ID3D11PixelShader *ps = nullptr;
        dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        printf("shaders: vs %p ps %p\n", (void *)vs, (void *)ps);
        D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        if (!tricull) {
            D3D11_RASTERIZER_DESC rd = {};
            rd.FillMode = D3D11_FILL_SOLID;
            rd.CullMode = D3D11_CULL_NONE;
            rd.DepthClipEnable = TRUE;
            ID3D11RasterizerState *rs = nullptr;
            hr = dev->CreateRasterizerState(&rd, &rs);
            printf("CreateRasterizerState(cull none): %08lx\n", (unsigned long)hr);
            ctx->RSSetState(rs);
        }
        ctx->OMSetRenderTargets(nort ? 0 : 1, nort ? nullptr : &rtv, nullptr);
        D3D11_DEPTH_STENCIL_DESC dsd = {};               // default state has depth on (no DSV here)
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;         // 0 is invalid -> CreateDepthStencilState fails
        dsd.StencilReadMask = dsd.StencilWriteMask = 0xff;
        dsd.FrontFace.StencilFunc = dsd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dsd.FrontFace.StencilPassOp = dsd.FrontFace.StencilFailOp = dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dsd.BackFace = dsd.FrontFace;
        ID3D11DepthStencilState *dss = nullptr;
        hr = dev->CreateDepthStencilState(&dsd, &dss);
        printf("CreateDepthStencilState: %08lx\n", (unsigned long)hr);
        ctx->OMSetDepthStencilState(dss, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
    } else if (!copy) {
        ctx->ClearRenderTargetView(rtv, color);
    }
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
