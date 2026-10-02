/*
 * Step C2 (docs/P8_gpu.md): the display side of TopazGpuW on the ACPI device GPU0 (TPZG0610).
 * Dxgkrnl treats an ACPI-enumerated display adapter as the platform GPU that owns the internal
 * panel, so this adapter has 1 VidPN source and 1 target (the DSI panel, 1080x2400 XRGB8888).
 *
 * Scanout: there is no MDP driver yet; the panel keeps scanning the UEFI GOP framebuffer at
 * 0x5C000000 (video mode, set up by the bootloader). The "flip" to a primary allocation is a CPU
 * copy of that allocation into the framebuffer (engine thread); blts into the scanned-out primary
 * copy their rectangles too. VSync is a 60 Hz timer reported as DXGK_INTERRUPT_CRTC_VSYNC through
 * DxgkCbSynchronizeExecution. Brightness: DCS 0x51 through DSI0 (same path as TopazDisplay
 * topaz_bl.cxx, docs/NOTES_brightness.md). VidPN code follows the KMDOD sample (TopazDisplay).
 */
#include "tgpu.h"

/* GUID_DEVINTERFACE_BRIGHTNESS (ntddvdeo.h, already included by dispmprt.h without initguid) */
static const GUID g_BrightnessGuid = { 0xFDE5BBA4, 0xB3F9, 0x46FB, { 0xBD, 0xAA, 0x07, 0x28, 0xCE, 0x31, 0x00, 0xB4 } };



/* panel timing (stock DT, m7 38 0c 0a): h fp/pw/bp 120/28/120, v fp/pw/bp 20/2/10, 60 Hz */
#define H_ACTIVE    1080
#define V_ACTIVE    2400
#define H_TOTAL     (1080 + 120 + 28 + 120)
#define V_TOTAL     (2400 + 20 + 2 + 10)
#define REFRESH     60

static PDXGKRNL_INTERFACE g_Dxgk;
static DXGK_DISPLAY_INFORMATION g_Post;
static PUCHAR g_Fb;                             /* WC mapping of the framebuffer */
static BOOLEAN g_Visible = TRUE;
static BOOLEAN g_Committed;

static TGPU_ALLOCATION * volatile g_ScanCur;    /* primary being "scanned out" */
static PHYSICAL_ADDRESS g_ScanPa;               /* its address as Dxgkrnl knows it */
static volatile LONG g_ScanDirty;
static volatile LONG g_FlipPending;
static volatile LONG g_VsyncOn;
static KMUTEX g_ScanLock;                       /* engine copies vs. DestroyAllocation */

static KTIMER g_VsyncTimer;
static KDPC g_VsyncDpc;
static BOOLEAN g_TimerOn;
static volatile LONG64 g_LastVsync;             /* KeQueryInterruptTime of the last vsync */
static ULONG g_VsyncCount;

/* ---------------------------------------------------------------- vsync */

static BOOLEAN VsyncSync(PVOID Ctx)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;

    UNREFERENCED_PARAMETER(Ctx);
    RtlZeroMemory(&d, sizeof(d));
    d.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    d.CrtcVsync.VidPnTargetId = 0;
    d.CrtcVsync.PhysicalAddress = g_ScanPa;
    g_Dxgk->DxgkCbNotifyInterrupt(g_Dxgk->DeviceHandle, &d);
    g_Dxgk->DxgkCbQueueDpc(g_Dxgk->DeviceHandle);
    return TRUE;
}

static KDEFERRED_ROUTINE VsyncDpc;
static VOID VsyncDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    BOOLEAN ret;
    BOOLEAN flip;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(A1);
    UNREFERENCED_PARAMETER(A2);
    g_LastVsync = (LONG64)KeQueryInterruptTime();
    if (InterlockedExchange(&g_ScanDirty, 0)) {
        EngKickScanout();
    }
    flip = InterlockedExchange(&g_FlipPending, 0) != 0;
    if (g_Dxgk != NULL && (g_VsyncOn || flip)) {
        g_Dxgk->DxgkCbSynchronizeExecution(g_Dxgk->DeviceHandle, VsyncSync, NULL, 0, &ret);
        if (++g_VsyncCount <= 3 || (flip && g_VsyncCount < 50)) {
            LogPrint("vsync %u%s pa %llx\n", g_VsyncCount, flip ? " (flip done)" : "", g_ScanPa.QuadPart);
        }
    }
}

VOID DispVsyncEnable(BOOLEAN Enable)
{
    InterlockedExchange(&g_VsyncOn, Enable ? 1 : 0);
}

NTSTATUS DispGetScanLine(DXGKARG_GETSCANLINE *A)
{
    LONG64 since = (LONG64)KeQueryInterruptTime() - g_LastVsync;     /* 100 ns */
    LONG64 frame = 10000000LL / REFRESH;
    ULONG line;

    if (since < 0 || since >= frame) {
        since = 0;
    }
    line = (ULONG)(since * V_TOTAL / frame);
    A->InVerticalBlank = line >= V_ACTIVE;
    A->ScanLine = line;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- scanout */

static VOID CopyRectToFb(PUCHAR Src, ULONG SrcPitch, ULONG SrcW, ULONG SrcH, const RECT *R)
{
    LONG l = R->left, t = R->top, r = R->right, b = R->bottom, y;

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)SrcW) r = SrcW;
    if (b > (LONG)SrcH) b = SrcH;
    if (r > (LONG)g_Post.Width) r = g_Post.Width;
    if (b > (LONG)g_Post.Height) b = g_Post.Height;
    if (r <= l || b <= t) {
        return;
    }
    for (y = t; y < b; y++) {
        RtlCopyMemory(g_Fb + (SIZE_T)y * g_Post.Pitch + (SIZE_T)l * 4, Src + (SIZE_T)y * SrcPitch + (SIZE_T)l * 4,
                      (SIZE_T)(r - l) * 4);
    }
}

VOID DispScanoutWork(VOID)
{
    TGPU_ALLOCATION *al;
    PUCHAR px;
    RECT all;
    static ULONG count;

    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    al = g_ScanCur;
    if (al != NULL && g_Fb != NULL && g_Visible && (px = AllocPixels(al)) != NULL) {
        all.left = all.top = 0;
        all.right = al->Desc.width;
        all.bottom = al->Desc.height;
        CopyRectToFb(px, al->Desc.pitch, al->Desc.width, al->Desc.height, &all);
        if (++count <= 5) {
            LogPrint("scanout: %ux%u pitch %u from %p (%s)\n", al->Desc.width, al->Desc.height, al->Desc.pitch, al,
                     al->Bo != NULL ? "bo" : "vidmm");
        }
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

VOID DispPresentRects(TGPU_ALLOCATION *Dst, const RECT *Rects, ULONG Count)
{
    PUCHAR px;
    ULONG i;

    if (Dst != g_ScanCur || g_Fb == NULL || !g_Visible) {
        return;
    }
    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    if (Dst == g_ScanCur && (px = AllocPixels(Dst)) != NULL) {
        for (i = 0; i < Count; i++) {
            CopyRectToFb(px, Dst->Desc.pitch, Dst->Desc.width, Dst->Desc.height, &Rects[i]);
        }
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

VOID DispAllocDestroyed(TGPU_ALLOCATION *Al)
{
    KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
    if (g_ScanCur == Al) {
        g_ScanCur = NULL;
        LogPrint("scanout: primary %p destroyed while scanned out\n", Al);
    }
    KeReleaseMutex(&g_ScanLock, FALSE);
}

/* may run at DIRQL (MMIO-style flip): only plain stores, the vsync DPC does the rest */
NTSTATUS DispSetSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *A)
{
    static ULONG count;

    g_ScanCur = (TGPU_ALLOCATION *)A->hAllocation;
    g_ScanPa = A->PrimaryAddress;
    InterlockedExchange(&g_ScanDirty, 1);
    InterlockedExchange(&g_FlipPending, 1);
    if (++count <= 20) {
        LogPrint("SetVidPnSourceAddress: src %u alloc %p seg %u pa %llx flags %x\n", A->VidPnSourceId, A->hAllocation,
                 A->PrimarySegment, A->PrimaryAddress.QuadPart, A->Flags.Value);
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispSetVisibility(const DXGKARG_SETVIDPNSOURCEVISIBILITY *A)
{
    LogPrint("SetVidPnSourceVisibility: src %u visible %u\n", A->VidPnSourceId, A->Visible);
    g_Visible = A->Visible ? TRUE : FALSE;
    if (g_Visible) {
        InterlockedExchange(&g_ScanDirty, 1);
    }
    return STATUS_SUCCESS;
}

VOID DispSystemWrite(PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y)
{
    UINT y;

    if (g_Fb == NULL) {
        return;
    }
    for (y = 0; y < H && Y + y < g_Post.Height; y++) {
        UINT w = (X + W > g_Post.Width) ? (X < g_Post.Width ? g_Post.Width - X : 0) : W;
        RtlCopyMemory(g_Fb + (SIZE_T)(Y + y) * g_Post.Pitch + (SIZE_T)X * 4, (PUCHAR)Src + (SIZE_T)y * Stride,
                      (SIZE_T)w * 4);
    }
}

/* ---------------------------------------------------------------- start / stop */

static VOID InitBrightness(VOID);

NTSTATUS DispStart(PDXGKRNL_INTERFACE Dxgk)
{
    NTSTATUS st;
    LARGE_INTEGER due;

    g_Dxgk = Dxgk;
    KeInitializeMutex(&g_ScanLock, 0);
    RtlZeroMemory(&g_Post, sizeof(g_Post));
    st = Dxgk->DxgkCbAcquirePostDisplayOwnership(Dxgk->DeviceHandle, &g_Post);
    LogPrint("AcquirePostDisplayOwnership: %08x %ux%u pitch %u fmt %u pa %llx target %u acpi %u\n", st, g_Post.Width,
             g_Post.Height, g_Post.Pitch, g_Post.ColorFormat, g_Post.PhysicAddress.QuadPart, g_Post.TargetId, g_Post.AcpiId);
    if (!NT_SUCCESS(st) || g_Post.Width == 0 || g_Post.PhysicAddress.QuadPart == 0) {
        /* same fallback as TopazDisplay: the GOP framebuffer the bootloader left running */
        g_Post.Width = TGPU_FB_WIDTH;
        g_Post.Height = TGPU_FB_HEIGHT;
        g_Post.Pitch = TGPU_FB_PITCH;
        g_Post.ColorFormat = D3DDDIFMT_X8R8G8B8;
        g_Post.PhysicAddress.QuadPart = (LONGLONG)TGPU_FB_PA;
        g_Post.TargetId = 0;
        LogPrint("POST info empty: using the fixed framebuffer %llx\n", TGPU_FB_PA);
    }
    if (g_Fb == NULL) {
        g_Fb = (PUCHAR)MmMapIoSpaceEx(g_Post.PhysicAddress, (SIZE_T)g_Post.Pitch * g_Post.Height,
                                      PAGE_READWRITE | PAGE_WRITECOMBINE);
    }
    LogPrint("framebuffer mapped at %p\n", g_Fb);
    InitBrightness();

    KeInitializeTimerEx(&g_VsyncTimer, NotificationTimer);
    KeInitializeDpc(&g_VsyncDpc, VsyncDpc, NULL);
    due.QuadPart = -10000LL * 16;
    KeSetTimerEx(&g_VsyncTimer, due, 1000 / REFRESH, &g_VsyncDpc);
    g_TimerOn = TRUE;
    return STATUS_SUCCESS;
}

VOID DispStop(VOID)
{
    if (g_TimerOn) {
        KeCancelTimer(&g_VsyncTimer);
        KeFlushQueuedDpcs();
        g_TimerOn = FALSE;
    }
    g_ScanCur = NULL;
    g_Dxgk = NULL;
}

VOID DispGetPostInfo(DXGK_DISPLAY_INFORMATION *Info)
{
    *Info = g_Post;
}

/* ---------------------------------------------------------------- children / EDID */

static LONG ReadCfg(LONG *Vot, LONG *Hpd)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\C:\\topaz\\gpuw.cfg");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    CHAR buf[32];
    LONG v[2] = { 0x7FFFFFFF, 0x7FFFFFFF };
    ULONG i, n = 0;
    BOOLEAN neg = FALSE, any = FALSE;
    LONG cur = 0;

    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        RtlZeroMemory(buf, sizeof(buf));
        if (NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, sizeof(buf) - 1, NULL, NULL))) {
            for (i = 0; i <= iosb.Information && n < 2; i++) {
                CHAR c = buf[i];
                if (c == '-') {
                    neg = TRUE;
                } else if (c >= '0' && c <= '9') {
                    cur = cur * 10 + (c - '0');
                    any = TRUE;
                } else {
                    if (any) {
                        v[n++] = neg ? -cur : cur;
                    }
                    cur = 0;
                    any = neg = FALSE;
                }
            }
        }
        ZwClose(h);
    }
    *Vot = v[0];
    *Hpd = v[1];
    return (LONG)n;
}

NTSTATUS DispQueryChildRelations(PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    LONG vot = (LONG)D3DKMDT_VOT_INTERNAL, hpd = (LONG)HpdAwarenessAlwaysConnected, cv, ch;

    /* POST/ACPI adapter: the panel is the internal display (C:\topaz\gpuw.cfg "<vot> <hpd>" overrides) */
    ReadCfg(&cv, &ch);
    if (cv != 0x7FFFFFFF) vot = cv;
    if (ch != 0x7FFFFFFF) hpd = ch;
    LogPrint("QueryChildRelations: size %u (%u slots) vot %d hpd %d\n", Size, Size / sizeof(*Rel), vot, hpd);
    if (Size < 2 * sizeof(*Rel)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(Rel, sizeof(*Rel));
    Rel[0].ChildDeviceType = TypeVideoOutput;
    Rel[0].ChildCapabilities.HpdAwareness = (DXGK_CHILD_DEVICE_HPD_AWARENESS)hpd;
    Rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = (D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY)vot;
    Rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    Rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    Rel[0].AcpiUid = 0;
    Rel[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DispQueryChildStatus(PDXGK_CHILD_STATUS St)
{
    switch (St->Type) {
    case StatusConnection:
        St->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        St->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

/* EDID 1.4 of the panel (as drivers/TopazDisplay/topaz_edid.cxx): DSI has no DDC */
static VOID BuildEdid(UCHAR *e)
{
    static const UCHAR header[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
    static const UCHAR chroma[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
    static const char name[13] = { 'T','o','p','a','z',' ','P','a','n','e','l','\n',' ' };
    const ULONG hActive = 1080, hFp = 120, hSync = 28, hBp = 120;
    const ULONG vActive = 2400, vFp = 20, vSync = 2, vBp = 10;
    const ULONG hBlank = hFp + hSync + hBp, vBlank = vFp + vSync + vBp;
    const ULONG clk10k = (hActive + hBlank) * (vActive + vBlank) * 60 / 10000;
    const ULONG wMm = 70, hMm = 155;
    UCHAR *d;
    ULONG i, sum = 0;

    RtlZeroMemory(e, 128);
    RtlCopyMemory(e, header, sizeof(header));
    e[8] = 0x52; e[9] = 0x1A;                       /* "TPZ" */
    e[10] = 0x25; e[11] = 0x62;                     /* 0x6225 */
    e[16] = 1; e[17] = 2026 - 1990;
    e[18] = 1; e[19] = 4;
    e[20] = 0xA0;
    e[21] = (UCHAR)((wMm + 5) / 10);
    e[22] = (UCHAR)((hMm + 5) / 10);
    e[23] = 0x78;
    e[24] = 0x02;
    RtlCopyMemory(&e[25], chroma, sizeof(chroma));
    for (i = 38; i < 54; i++) {
        e[i] = 0x01;
    }
    d = &e[54];
    d[0] = (UCHAR)(clk10k & 0xFF);
    d[1] = (UCHAR)(clk10k >> 8);
    d[2] = (UCHAR)(hActive & 0xFF);
    d[3] = (UCHAR)(hBlank & 0xFF);
    d[4] = (UCHAR)(((hActive >> 8) << 4) | (hBlank >> 8));
    d[5] = (UCHAR)(vActive & 0xFF);
    d[6] = (UCHAR)(vBlank & 0xFF);
    d[7] = (UCHAR)(((vActive >> 8) << 4) | (vBlank >> 8));
    d[8] = (UCHAR)(hFp & 0xFF);
    d[9] = (UCHAR)(hSync & 0xFF);
    d[10] = (UCHAR)(((vFp & 0xF) << 4) | (vSync & 0xF));
    d[11] = (UCHAR)(((hFp >> 8) << 6) | ((hSync >> 8) << 4) | ((vFp >> 4) << 2) | (vSync >> 4));
    d[12] = (UCHAR)(wMm & 0xFF);
    d[13] = (UCHAR)(hMm & 0xFF);
    d[14] = (UCHAR)(((wMm >> 8) << 4) | (hMm >> 8));
    d[17] = 0x1E;
    d = &e[72];
    d[3] = 0xFC;
    RtlCopyMemory(&d[5], name, sizeof(name));
    e[90 + 3] = 0x10;
    e[108 + 3] = 0x10;
    for (i = 0; i < 127; i++) {
        sum += e[i];
    }
    e[127] = (UCHAR)(256 - (sum & 0xFF));
}

NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UCHAR edid[128];
    ULONG n;

    if (Uid != 0 || Desc->DescriptorOffset >= sizeof(edid)) {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
    BuildEdid(edid);
    n = sizeof(edid) - Desc->DescriptorOffset;
    if (n > Desc->DescriptorLength) {
        n = Desc->DescriptorLength;
    }
    RtlCopyMemory(Desc->DescriptorBuffer, edid + Desc->DescriptorOffset, n);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- VidPN */

static VOID FillSignal(D3DKMDT_VIDEO_SIGNAL_INFO *S)
{
    S->VideoStandard = D3DKMDT_VSS_OTHER;
    S->TotalSize.cx = H_TOTAL;
    S->TotalSize.cy = V_TOTAL;
    S->ActiveSize.cx = H_ACTIVE;
    S->ActiveSize.cy = V_ACTIVE;
    S->VSyncFreq.Numerator = REFRESH;
    S->VSyncFreq.Denominator = 1;
    S->HSyncFreq.Numerator = V_TOTAL * REFRESH;
    S->HSyncFreq.Denominator = 1;
    S->PixelRate = (SIZE_T)H_TOTAL * V_TOTAL * REFRESH;
    S->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS AddSourceMode(const DXGK_VIDPNSOURCEMODESET_INTERFACE *If, D3DKMDT_HVIDPNSOURCEMODESET Set)
{
    D3DKMDT_VIDPN_SOURCE_MODE *m = NULL;
    NTSTATUS st = If->pfnCreateNewModeInfo(Set, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    m->Type = D3DKMDT_RMT_GRAPHICS;
    m->Format.Graphics.PrimSurfSize.cx = H_ACTIVE;
    m->Format.Graphics.PrimSurfSize.cy = V_ACTIVE;
    m->Format.Graphics.VisibleRegionSize = m->Format.Graphics.PrimSurfSize;
    m->Format.Graphics.Stride = H_ACTIVE * 4;
    m->Format.Graphics.PixelFormat = D3DDDIFMT_X8R8G8B8;
    m->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
    m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
    st = If->pfnAddMode(Set, m);
    if (!NT_SUCCESS(st)) {
        If->pfnReleaseModeInfo(Set, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS AddTargetMode(const DXGK_VIDPNTARGETMODESET_INTERFACE *If, D3DKMDT_HVIDPNTARGETMODESET Set)
{
    D3DKMDT_VIDPN_TARGET_MODE *m = NULL;
    NTSTATUS st = If->pfnCreateNewModeInfo(Set, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    FillSignal(&m->VideoSignalInfo);
    m->Preference = D3DKMDT_MP_PREFERRED;
    st = If->pfnAddMode(Set, m);
    if (!NT_SUCCESS(st)) {
        If->pfnReleaseModeInfo(Set, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispRecommendMonitorModes(const DXGKARG_RECOMMENDMONITORMODES *A)
{
    D3DKMDT_MONITOR_SOURCE_MODE *m = NULL;
    NTSTATUS st = A->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(A->hMonitorSourceModeSet, &m);

    if (!NT_SUCCESS(st)) {
        return st;
    }
    FillSignal(&m->VideoSignalInfo);
    m->Origin = D3DKMDT_MCO_DRIVER;
    m->Preference = D3DKMDT_MP_PREFERRED;
    m->ColorBasis = D3DKMDT_CB_SRGB;
    m->ColorCoeffDynamicRanges.FirstChannel = 8;
    m->ColorCoeffDynamicRanges.SecondChannel = 8;
    m->ColorCoeffDynamicRanges.ThirdChannel = 8;
    m->ColorCoeffDynamicRanges.FourthChannel = 8;
    st = A->pMonitorSourceModeSetInterface->pfnAddMode(A->hMonitorSourceModeSet, m);
    if (!NT_SUCCESS(st)) {
        A->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(A->hMonitorSourceModeSet, m);
        return st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET ? STATUS_SUCCESS : st;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DispEnumCofuncModality(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A)
{
    const DXGK_VIDPN_INTERFACE *vi = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = 0;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next = NULL;
    NTSTATUS st;

    st = Dxgk->DxgkCbQueryVidPnInterface(A->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = vi->pfnGetTopology(A->hConstrainingVidPn, &topo, &ti);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = ti->pfnAcquireFirstPathInfo(topo, &path);
    if (st == STATUS_GRAPHICS_DATASET_IS_EMPTY) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }
    while (path != NULL) {
        D3DKMDT_HVIDPNSOURCEMODESET sset = 0;
        D3DKMDT_HVIDPNTARGETMODESET tset = 0;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE *si = NULL;
        const DXGK_VIDPNTARGETMODESET_INTERFACE *tgi = NULL;
        const D3DKMDT_VIDPN_SOURCE_MODE *spin = NULL;
        const D3DKMDT_VIDPN_TARGET_MODE *tpin = NULL;
        D3DKMDT_VIDPN_PRESENT_PATH local;
        BOOLEAN modified = FALSE;

        /* source modes */
        if (!(A->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && A->EnumPivot.VidPnSourceId == path->VidPnSourceId)) {
            st = vi->pfnAcquireSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, &sset, &si);
            if (!NT_SUCCESS(st)) {
                break;
            }
            st = si->pfnAcquirePinnedModeInfo(sset, &spin);
            if (NT_SUCCESS(st) && spin == NULL) {
                vi->pfnReleaseSourceModeSet(A->hConstrainingVidPn, sset);
                sset = 0;
                st = vi->pfnCreateNewSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, &sset, &si);
                if (NT_SUCCESS(st)) {
                    st = AddSourceMode(si, sset);
                }
                if (NT_SUCCESS(st)) {
                    st = vi->pfnAssignSourceModeSet(A->hConstrainingVidPn, path->VidPnSourceId, sset);
                    if (NT_SUCCESS(st)) {
                        sset = 0;
                    }
                }
            } else if (spin != NULL) {
                si->pfnReleaseModeInfo(sset, spin);
            }
            if (sset != 0) {
                vi->pfnReleaseSourceModeSet(A->hConstrainingVidPn, sset);
            }
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        /* target modes */
        if (!(A->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && A->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            st = vi->pfnAcquireTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, &tset, &tgi);
            if (!NT_SUCCESS(st)) {
                break;
            }
            st = tgi->pfnAcquirePinnedModeInfo(tset, &tpin);
            if (NT_SUCCESS(st) && tpin == NULL) {
                vi->pfnReleaseTargetModeSet(A->hConstrainingVidPn, tset);
                tset = 0;
                st = vi->pfnCreateNewTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, &tset, &tgi);
                if (NT_SUCCESS(st)) {
                    st = AddTargetMode(tgi, tset);
                }
                if (NT_SUCCESS(st)) {
                    st = vi->pfnAssignTargetModeSet(A->hConstrainingVidPn, path->VidPnTargetId, tset);
                    if (NT_SUCCESS(st)) {
                        tset = 0;
                    }
                }
            } else if (tpin != NULL) {
                tgi->pfnReleaseModeInfo(tset, tpin);
            }
            if (tset != 0) {
                vi->pfnReleaseTargetModeSet(A->hConstrainingVidPn, tset);
            }
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        /* scaling / rotation support: identity only */
        local = *path;
        if (!(A->EnumPivotType == D3DKMDT_EPT_SCALING && A->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
              A->EnumPivot.VidPnTargetId == path->VidPnTargetId) &&
            path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED) {
            RtlZeroMemory(&local.ContentTransformation.ScalingSupport, sizeof(local.ContentTransformation.ScalingSupport));
            local.ContentTransformation.ScalingSupport.Identity = 1;
            modified = TRUE;
        }
        if (!(A->EnumPivotType == D3DKMDT_EPT_ROTATION && A->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
              A->EnumPivot.VidPnTargetId == path->VidPnTargetId) &&
            path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED) {
            RtlZeroMemory(&local.ContentTransformation.RotationSupport, sizeof(local.ContentTransformation.RotationSupport));
            local.ContentTransformation.RotationSupport.Identity = 1;
            local.ContentTransformation.RotationSupport.Offset0 = 1;
            modified = TRUE;
        }
        if (modified) {
            st = ti->pfnUpdatePathSupportInfo(topo, &local);
            if (!NT_SUCCESS(st)) {
                break;
            }
        }
        st = ti->pfnAcquireNextPathInfo(topo, path, &next);
        ti->pfnReleasePathInfo(topo, path);
        path = NULL;
        if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) {
            st = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(st)) {
            break;
        }
        path = next;
    }
    if (path != NULL) {
        ti->pfnReleasePathInfo(topo, path);
    }
    if (!NT_SUCCESS(st)) {
        LogPrint("EnumVidPnCofuncModality: %08x\n", st);
    }
    return st;
}

NTSTATUS DispCommitVidPn(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_COMMITVIDPN *A)
{
    const DXGK_VIDPN_INTERFACE *vi = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *ti = NULL;
    D3DKMDT_HVIDPNTOPOLOGY topo = 0;
    D3DKMDT_HVIDPNSOURCEMODESET sset = 0;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *pin = NULL;
    SIZE_T paths = 0;
    NTSTATUS st;

    if (A->Flags.PathPoweredOff) {
        return STATUS_SUCCESS;
    }
    st = Dxgk->DxgkCbQueryVidPnInterface(A->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (NT_SUCCESS(st)) {
        st = vi->pfnGetTopology(A->hFunctionalVidPn, &topo, &ti);
    }
    if (NT_SUCCESS(st)) {
        st = ti->pfnGetNumPaths(topo, &paths);
    }
    if (NT_SUCCESS(st) && paths != 0) {
        st = vi->pfnAcquireSourceModeSet(A->hFunctionalVidPn, A->AffectedVidPnSourceId, &sset, &si);
        if (NT_SUCCESS(st)) {
            st = si->pfnAcquirePinnedModeInfo(sset, &pin);
        }
    }
    if (NT_SUCCESS(st)) {
        g_Committed = pin != NULL;
        LogPrint("CommitVidPn: src %u paths %u mode %ux%u stride %u fmt %u primary %p\n", A->AffectedVidPnSourceId,
                 (ULONG)paths, pin ? pin->Format.Graphics.PrimSurfSize.cx : 0, pin ? pin->Format.Graphics.PrimSurfSize.cy : 0,
                 pin ? pin->Format.Graphics.Stride : 0, pin ? pin->Format.Graphics.PixelFormat : 0, A->hPrimaryAllocation);
        if (!g_Committed) {
            KeWaitForSingleObject(&g_ScanLock, Executive, KernelMode, FALSE, NULL);
            g_ScanCur = NULL;
            KeReleaseMutex(&g_ScanLock, FALSE);
        } else if (A->hPrimaryAllocation != NULL) {
            g_ScanCur = (TGPU_ALLOCATION *)A->hPrimaryAllocation;
            InterlockedExchange(&g_ScanDirty, 1);
        }
    } else {
        LogPrint("CommitVidPn: %08x\n", st);
    }
    if (pin != NULL) {
        si->pfnReleaseModeInfo(sset, pin);
    }
    if (sset != 0) {
        vi->pfnReleaseSourceModeSet(A->hFunctionalVidPn, sset);
    }
    return st;
}

/* ---------------------------------------------------------------- brightness (DCS 0x51 over DSI0) */

#define DSI0_PA             0x05E94000ULL
#define DSI_CTRL            0x004
#define DSI_CMD_DMA_CTRL    0x03C
#define DSI_DMA_CMD_OFFSET  0x048
#define DSI_DMA_CMD_LENGTH  0x04C
#define DSI_TRIG_CTRL       0x084
#define DSI_DMA_SW_TRIGGER  0x090
#define DSI_INT_CTRL        0x110
#define DSI_TPG_CTRL        0x15C

static volatile UCHAR *g_Dsi;
static PUCHAR g_BlVa;
static PHYSICAL_ADDRESS g_BlPa;
static UCHAR g_Brightness = 100;
static KMUTEX g_BlLock;

static ULONG DsiRd(ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off)); }
static VOID DsiWr(ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off), V); }

static VOID InitBrightness(VOID)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;

    if (g_Dsi != NULL) {
        return;
    }
    KeInitializeMutex(&g_BlLock, 0);
    pa.QuadPart = (LONGLONG)DSI0_PA;
    g_Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x400, PAGE_READWRITE | PAGE_NOCACHE);
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    g_BlVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
    if (g_BlVa != NULL) {
        g_BlPa = MmGetPhysicalAddress(g_BlVa);
    }
    LogPrint("brightness: DSI0 %p ctrl %08x, DMA buffer %llx\n", g_Dsi, g_Dsi ? DsiRd(DSI_CTRL) : 0, g_BlPa.QuadPart);
}

static BOOLEAN DsiDcsLong(const UCHAR *Payload, ULONG Len)
{
    ULONG size = (4 + Len + 3) & ~3u, ctrl, dma, trig, i;
    BOOLEAN done = FALSE;

    if (g_Dsi == NULL || g_BlVa == NULL || Len > 60) {
        return FALSE;
    }
    RtlFillMemory(g_BlVa, 64, 0xFF);
    g_BlVa[0] = (UCHAR)Len;
    g_BlVa[1] = (UCHAR)(Len >> 8);
    g_BlVa[2] = 0x39;                           /* DCS long write, VC 0 */
    g_BlVa[3] = 0xC0;                           /* last, long */
    RtlCopyMemory(g_BlVa + 4, Payload, Len);
    KeMemoryBarrier();
    ctrl = DsiRd(DSI_CTRL);
    trig = DsiRd(DSI_TRIG_CTRL);
    if ((trig & 7) != 4) {
        DsiWr(DSI_TRIG_CTRL, (trig & ~7u) | 4);
    }
    DsiWr(DSI_CTRL, ctrl | 4 | 1);
    DsiWr(DSI_INT_CTRL, DsiRd(DSI_INT_CTRL) | 1);
    DsiWr(DSI_TPG_CTRL, 0);
    DsiWr(DSI_DMA_CMD_OFFSET, g_BlPa.LowPart);
    DsiWr(DSI_DMA_CMD_LENGTH, size);
    dma = DsiRd(DSI_CMD_DMA_CTRL);
    DsiWr(DSI_CMD_DMA_CTRL, dma & ~(1u << 26));
    DsiWr(DSI_DMA_SW_TRIGGER, 1);
    for (i = 0; i < 50; i++) {
        if (DsiRd(DSI_INT_CTRL) & 1) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    DsiWr(DSI_INT_CTRL, DsiRd(DSI_INT_CTRL) | 1);
    DsiWr(DSI_CMD_DMA_CTRL, dma);
    DsiWr(DSI_CTRL, ctrl);
    if (trig != DsiRd(DSI_TRIG_CTRL)) {
        DsiWr(DSI_TRIG_CTRL, trig);
    }
    return done;
}

static VOID APIENTRY BlRef(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }
static VOID APIENTRY BlDeref(PVOID Ctx) { UNREFERENCED_PARAMETER(Ctx); }

static NTSTATUS APIENTRY BlGetPossible(HANDLE Ctx, ULONG BufferSize, UCHAR *LevelCount, UCHAR *Levels)
{
    ULONG i, n = BufferSize < 101 ? BufferSize : 101;

    UNREFERENCED_PARAMETER(Ctx);
    if (LevelCount == NULL || Levels == NULL || n == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < n; i++) {
        Levels[i] = (UCHAR)i;
    }
    *LevelCount = (UCHAR)n;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY BlSet(HANDLE Ctx, UCHAR Pct)
{
    UCHAR cmd[3];
    ULONG level;
    BOOLEAN done;

    UNREFERENCED_PARAMETER(Ctx);
    if (Pct > 100) {
        Pct = 100;
    }
    g_Brightness = Pct;
    level = (ULONG)Pct * 0x7FF / 100;
    if (level < 1) {
        level = 1;
    }
    cmd[0] = 0x51;
    cmd[1] = (UCHAR)(level >> 8);
    cmd[2] = (UCHAR)level;
    KeWaitForSingleObject(&g_BlLock, Executive, KernelMode, FALSE, NULL);
    done = DsiDcsLong(cmd, sizeof(cmd));
    KeReleaseMutex(&g_BlLock, FALSE);
    LogPrint("brightness %u%% -> 0x%03x: %s\n", Pct, level, done ? "done" : "NO DONE");
    return done ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static NTSTATUS APIENTRY BlGet(HANDLE Ctx, UCHAR *Pct)
{
    UNREFERENCED_PARAMETER(Ctx);
    if (Pct == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Pct = g_Brightness;
    return STATUS_SUCCESS;
}

NTSTATUS DispBrightnessQueryInterface(PQUERY_INTERFACE Qi)
{
    DXGK_BRIGHTNESS_INTERFACE *b;

    if (Qi == NULL || Qi->InterfaceType == NULL ||
        RtlCompareMemory(Qi->InterfaceType, &g_BrightnessGuid, sizeof(GUID)) != sizeof(GUID)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (g_Dsi == NULL || g_BlVa == NULL || Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE) || Qi->Interface == NULL) {
        LogPrint("QueryInterface brightness v%u size %u: not available\n", Qi->Version, Qi->Size);
        return STATUS_NOT_SUPPORTED;
    }
    b = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
    RtlZeroMemory(b, sizeof(*b));
    b->Size = sizeof(*b);
    b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
    b->InterfaceReference = BlRef;
    b->InterfaceDereference = BlDeref;
    b->GetPossibleBrightness = BlGetPossible;
    b->SetBrightness = BlSet;
    b->GetBrightness = BlGet;
    LogPrint("QueryInterface brightness v%u: provided\n", Qi->Version);
    return STATUS_SUCCESS;
}
