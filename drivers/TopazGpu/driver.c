/*
 * TopazGpu - Adreno 610 bring-up (stage G1, docs/P8_gpu.md) on Redmi Note 12 4G (topaz, SM6225).
 *
 * v0.1 logged the clock tree: the bootloader leaves the GPU fully off (both GDSCs collapsed, all
 * GPU CC clocks off, PLLs unconfigured). v0.2 powers it up the way Linux does for A610 (gdsc.c
 * gdsc_enable, a6xx_pm_resume with the GMU wrapper) and reads the first GPU registers, only when
 * C:\topaz\gpu.on exists (deleted before the attempt: a hang is never retried at the next boot).
 * GX_GFX3D runs from GCC GPLL0 (600 MHz / 2 = 300 MHz, parent 5), so no GPU CC PLL is needed yet.
 * Registers (Linux gcc-sm6115.c / gpucc-sm6115.c):
 *   GCC 0x1400000:  GPU_CFG_AHB_CBCR 0x36004 (critical, always on), GPU_MEMNOC_GFX 0x3600c,
 *                   GPU_SNOC_DVM_GFX 0x36018, GPU_IREF 0x36100, GPU_THROTTLE_CORE 0x36048,
 *                   vote reg 0x79004 (bit15 GPU_GPLL0_CLK_SRC, bit16 GPU_GPLL0_DIV, bit31 throttle).
 *   GPU CC 0x5990000: PLL0 0x0, PLL1 0x100, GX_GFX3D RCG 0x101c, GMU RCG 0x1120, branches
 *                   (CBCR bit31 = CLK_OFF, bit0 = enable), GX GDSC 0x100c, CX GDSC 0x106c
 *                   (GDSCR bit31 = PWR_ON, bit0 = SW_COLLAPSE), GX clamp 0x1508, CX hw ctrl 0x1540,
 *                   GPU SMMU vote 0x5000.
 * The GPU core (0x5900000) is read only after both GDSCs report PWR_ON and the core/AHB clocks run
 * (a read with them off hangs the bus). The SMMU (0x59a0000) is not touched yet. Log: C:\TopazGpu.log.
 */
#include "driver.h"

#define TOPAZ_GPU_VERSION   "v0.2"

#define GCC_BASE            0x01400000ULL
#define GCC_SIZE            0x80000
#define GPUCC_BASE          0x05990000ULL
#define GPUCC_SIZE          0x9000
#define GPU_BASE            0x05900000ULL
#define GPU_SIZE            0x40000

#define CBCR_EN             (1u << 0)
#define CBCR_OFF            (1u << 31)
#define GDSC_COLLAPSE       (1u << 0)
#define GDSC_PWR_ON         (1u << 31)
#define RCG_UPDATE          (1u << 0)
#define SRC_GPLL0           5               /* parent index of GPLL0_OUT_MAIN in GPU CC maps 0 and 1 */

/* A6xx registers, dword offsets (registers/adreno/a6xx.xml) */
#define A6XX_RBBM_STATUS            0x0210
#define A6XX_RBBM_PERFCTR_CNTL      0x0500
#define A6XX_CP_HW_FAULT            0x0821
#define A6XX_CP_ALWAYS_ON_COUNTER   0x0980

typedef struct _REG_DESC {
    UCHAR  Block;                           /* 0 = GCC, 1 = GPU CC */
    UCHAR  Kind;                            /* 0 raw, 1 branch (CBCR), 2 GDSC, 3 RCG cmd, 4 PLL mode */
    ULONG  Offset;
    PCSTR  Name;
} REG_DESC;

static const REG_DESC g_Regs[] = {
    { 0, 1, 0x36004, "gcc_gpu_cfg_ahb" },
    { 0, 1, 0x3600c, "gcc_gpu_memnoc_gfx" },
    { 0, 1, 0x36018, "gcc_gpu_snoc_dvm_gfx" },
    { 0, 1, 0x36100, "gcc_gpu_iref" },
    { 0, 1, 0x36048, "gcc_gpu_throttle_core" },
    { 0, 0, 0x79004, "gcc_apcs_clock_branch_ena_vote (b15 gpll0, b16 gpll0_div)" },
    { 1, 4, 0x0000,  "gpucc_pll0_mode" },
    { 1, 0, 0x0004,  "gpucc_pll0_l" },
    { 1, 0, 0x000c,  "gpucc_pll0_user_ctl" },
    { 1, 4, 0x0100,  "gpucc_pll1_mode" },
    { 1, 0, 0x0104,  "gpucc_pll1_l" },
    { 1, 3, 0x101c,  "gpucc_gx_gfx3d_rcg_cmd" },
    { 1, 0, 0x1020,  "gpucc_gx_gfx3d_rcg_cfg" },
    { 1, 3, 0x1120,  "gpucc_gmu_rcg_cmd" },
    { 1, 0, 0x1124,  "gpucc_gmu_rcg_cfg" },
    { 1, 1, 0x1004,  "gpucc_cxo_aon" },
    { 1, 1, 0x1054,  "gpucc_gx_gfx3d" },
    { 1, 1, 0x1060,  "gpucc_gx_cxo" },
    { 1, 1, 0x1078,  "gpucc_ahb" },
    { 1, 1, 0x107c,  "gpucc_crc_ahb" },
    { 1, 1, 0x108c,  "gpucc_cx_snoc_dvm" },
    { 1, 1, 0x1090,  "gpucc_sleep" },
    { 1, 1, 0x1098,  "gpucc_cx_gmu" },
    { 1, 1, 0x109c,  "gpucc_cxo" },
    { 1, 1, 0x10a4,  "gpucc_cx_gfx3d" },
    { 1, 1, 0x5000,  "gpucc_hlos1_vote_gpu_smmu" },
    { 1, 2, 0x100c,  "gpucc_gx_gdsc" },
    { 1, 2, 0x106c,  "gpucc_cx_gdsc" },
    { 1, 0, 0x1008,  "gpucc_gx_bcr" },
    { 1, 0, 0x1508,  "gpucc_gx_clamp_io" },
    { 1, 0, 0x1540,  "gpucc_cx_gds_hw_ctrl" },
};

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

static PCSTR Decode(UCHAR Kind, ULONG V, PCHAR Buf, SIZE_T Len)
{
    switch (Kind) {
    case 1:
        RtlStringCbPrintfA(Buf, Len, "en %u %s", V & 1, (V >> 31) ? "CLK_OFF" : "running");
        break;
    case 2:
        RtlStringCbPrintfA(Buf, Len, "sw_collapse %u %s", V & 1, (V >> 31) ? "PWR_ON" : "off");
        break;
    case 3:
        RtlStringCbPrintfA(Buf, Len, "root_en %u root_off %u", (V >> 1) & 1, (V >> 31) & 1);
        break;
    case 4:
        RtlStringCbPrintfA(Buf, Len, "outctrl %u bypassnl %u reset_n %u lock %u", V & 1, (V >> 1) & 1,
                           (V >> 2) & 1, (V >> 31) & 1);
        break;
    default:
        Buf[0] = 0;
        break;
    }
    return Buf;
}

static volatile UCHAR *g_Gcc, *g_GpuCc;

static ULONG Rd(volatile UCHAR *B, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(B + Off));
}

static VOID Wr(volatile UCHAR *B, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), Val);
}

static VOID Rmw(volatile UCHAR *B, ULONG Off, ULONG Clear, ULONG Set)
{
    Wr(B, Off, (Rd(B, Off) & ~Clear) | Set);
}

/* Polls until (reg & Mask) == Want, up to Us microseconds. */
static BOOLEAN Poll(volatile UCHAR *B, ULONG Off, ULONG Mask, ULONG Want, ULONG Us)
{
    ULONG i;

    for (i = 0; i <= Us; i++) {
        if ((Rd(B, Off) & Mask) == Want) {
            return TRUE;
        }
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static VOID DumpClocks(PCSTR Tag)
{
    ULONG i, v;
    CHAR dec[64];
    volatile UCHAR *base[2] = { g_Gcc, g_GpuCc };

    LogPrint("--- clock tree (%s)\n", Tag);
    for (i = 0; i < ARRAYSIZE(g_Regs); i++) {
        v = Rd(base[g_Regs[i].Block], g_Regs[i].Offset);
        LogPrint("%s+%05x %-34s %08x %s\n", g_Regs[i].Block ? "gpucc" : "gcc  ", g_Regs[i].Offset,
                 g_Regs[i].Name, v, Decode(g_Regs[i].Kind, v, dec, sizeof(dec)));
    }
}

static BOOLEAN BranchOn(volatile UCHAR *B, ULONG Off, PCSTR Name)
{
    BOOLEAN ok;

    Rmw(B, Off, 0, CBCR_EN);
    ok = Poll(B, Off, CBCR_OFF, 0, 200);
    LogPrint("  %-26s on: %08x %s\n", Name, Rd(B, Off), ok ? "running" : "STILL OFF");
    return ok;
}

static BOOLEAN RcgSet(ULONG Cmd, ULONG Src, ULONG Div, PCSTR Name)
{
    BOOLEAN ok;

    Wr(g_GpuCc, Cmd + 4, (Src << 8) | (2 * Div - 1));
    Rmw(g_GpuCc, Cmd, 0, RCG_UPDATE);
    ok = Poll(g_GpuCc, Cmd, RCG_UPDATE, 0, 500);
    LogPrint("  %-26s src %u div %u: cmd %08x cfg %08x %s\n", Name, Src, Div, Rd(g_GpuCc, Cmd),
             Rd(g_GpuCc, Cmd + 4), ok ? "updated" : "UPDATE STUCK");
    return ok;
}

/* gdsc.c gdsc_enable(): [SW_RESET: pulse the BCR] [CLAMP_IO: release the clamp] clear SW_COLLAPSE,
   poll PWR_ON (CX: in the GDS HW controller register, GX: in the GDSCR itself). */
static BOOLEAN GdscOn(ULONG Gdscr, ULONG Bcr, ULONG Clamp, ULONG StatusReg, PCSTR Name)
{
    BOOLEAN ok;

    if (Bcr != 0) {
        Rmw(g_GpuCc, Bcr, 0, 1);
        KeStallExecutionProcessor(1);
        Rmw(g_GpuCc, Bcr, 1, 0);
    }
    if (Clamp != 0) {
        Rmw(g_GpuCc, Clamp, 1, 0);
    }
    Rmw(g_GpuCc, Gdscr, GDSC_COLLAPSE, 0);
    KeStallExecutionProcessor(1);
    ok = Poll(g_GpuCc, StatusReg, GDSC_PWR_ON, GDSC_PWR_ON, 500);
    LogPrint("  %-26s on: gdscr %08x status %08x %s\n", Name, Rd(g_GpuCc, Gdscr), Rd(g_GpuCc, StatusReg),
             ok ? "PWR_ON" : "NO PWR_ON");
    return ok;
}

static BOOLEAN PowerUp(VOID)
{
    BOOLEAN ok = TRUE;

    LogPrint("--- power up\n");
    /* GCC side: GPLL0 to the GPU CC, bus/NoC clocks of the GPU and its SMMU */
    Rmw(g_Gcc, 0x79004, 0, 1u << 15);
    LogPrint("  gcc vote 79004 = %08x (gpu_gpll0_clk_src)\n", Rd(g_Gcc, 0x79004));
    BranchOn(g_Gcc, 0x71154, "gcc_bimc_gpu_axi");
    BranchOn(g_Gcc, 0x3600c, "gcc_gpu_memnoc_gfx");       /* BRANCH_VOTED: may read off until used */
    BranchOn(g_Gcc, 0x36018, "gcc_gpu_snoc_dvm_gfx");
    /* GPU CC always-on side */
    ok &= BranchOn(g_GpuCc, 0x109c, "gpucc_cxo");
    ok &= BranchOn(g_GpuCc, 0x1078, "gpucc_ahb");
    /* CX power domain, then its clocks */
    ok &= GdscOn(0x106c, 0, 0, 0x1540, "gpu_cx_gdsc");
    if (!ok) {
        return FALSE;
    }
    RcgSet(0x1120, SRC_GPLL0, 3, "gmu_clk_src (200 MHz)");
    Rmw(g_GpuCc, 0x1098, 0xFF0, 0xFF0);                  /* cx_gmu: wakeup/sleep 0xf (Linux probe) */
    BranchOn(g_GpuCc, 0x1098, "gpucc_cx_gmu");
    BranchOn(g_GpuCc, 0x5000, "gpucc_hlos1_vote_gpu_smmu");
    /* GX power domain (SW_RESET via GPU_GX_BCR 0x1008, CLAMP_IO 0x1508), then the core clock */
    ok &= GdscOn(0x100c, 0x1008, 0x1508, 0x100c, "gpu_gx_gdsc");
    if (!ok) {
        return FALSE;
    }
    ok &= RcgSet(0x101c, SRC_GPLL0, 2, "gx_gfx3d_clk_src (300 MHz)");
    Rmw(g_GpuCc, 0x1054, 0, (1u << 14) | (1u << 13));    /* force mem core / periph on (Linux probe) */
    ok &= BranchOn(g_GpuCc, 0x1054, "gpucc_gx_gfx3d");
    BranchOn(g_GpuCc, 0x1060, "gpucc_gx_cxo");
    return ok;
}

static VOID PowerDown(VOID)
{
    LogPrint("--- power down\n");
    Rmw(g_GpuCc, 0x1054, CBCR_EN, 0);
    Rmw(g_GpuCc, 0x100c, 0, GDSC_COLLAPSE);
    Rmw(g_GpuCc, 0x1508, 0, 1);
    Rmw(g_GpuCc, 0x1098, CBCR_EN, 0);
    Rmw(g_GpuCc, 0x5000, CBCR_EN, 0);
    Rmw(g_GpuCc, 0x106c, 0, GDSC_COLLAPSE);
    LogPrint("  gx gdscr %08x, cx gdscr %08x\n", Rd(g_GpuCc, 0x100c), Rd(g_GpuCc, 0x106c));
}

static VOID FirstGpuReads(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *gpu;
    ULONG lo1, hi1, lo2, hi2;
    LARGE_INTEGER wait;

    pa.QuadPart = (LONGLONG)GPU_BASE;
    gpu = (volatile UCHAR *)MmMapIoSpaceEx(pa, GPU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (gpu == NULL) {
        LogPrint("GPU map failed\n");
        return;
    }
    LogPrint("--- first GPU reads\n");
    LogPrint("  RBBM_STATUS      %08x\n", Rd(gpu, 4 * A6XX_RBBM_STATUS));
    LogPrint("  RBBM_PERFCTR_CNTL %08x\n", Rd(gpu, 4 * A6XX_RBBM_PERFCTR_CNTL));
    LogPrint("  CP_HW_FAULT      %08x\n", Rd(gpu, 4 * A6XX_CP_HW_FAULT));
    lo1 = Rd(gpu, 4 * A6XX_CP_ALWAYS_ON_COUNTER);
    hi1 = Rd(gpu, 4 * A6XX_CP_ALWAYS_ON_COUNTER + 4);
    wait.QuadPart = -10000LL * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    lo2 = Rd(gpu, 4 * A6XX_CP_ALWAYS_ON_COUNTER);
    hi2 = Rd(gpu, 4 * A6XX_CP_ALWAYS_ON_COUNTER + 4);
    LogPrint("  CP_ALWAYS_ON_COUNTER %08x%08x -> %08x%08x after 10 ms (delta %u, 19.2 MHz = ~192000)\n",
             hi1, lo1, hi2, lo2, lo2 - lo1);
    MmUnmapIoSpace((PVOID)gpu, GPU_SIZE);
}

static BOOLEAN FileProbe(PCWSTR Path, BOOLEAN Delete)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, (Delete ? DELETE : FILE_READ_ATTRIBUTES) | SYNCHRONIZE, &oa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 FILE_OPEN, (Delete ? FILE_DELETE_ON_CLOSE : 0) | FILE_SYNCHRONOUS_IO_NONALERT |
                                 FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static BOOLEAN g_Powered;

static VOID HwStart(VOID)
{
    PHYSICAL_ADDRESS pa;

    pa.QuadPart = (LONGLONG)GCC_BASE;
    g_Gcc = (volatile UCHAR *)MmMapIoSpaceEx(pa, GCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)GPUCC_BASE;
    g_GpuCc = (volatile UCHAR *)MmMapIoSpaceEx(pa, GPUCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (g_Gcc == NULL || g_GpuCc == NULL) {
        LogPrint("map failed: gcc %p gpucc %p\n", g_Gcc, g_GpuCc);
        return;
    }
    DumpClocks("as found");
    if (!FileProbe(L"\\??\\C:\\topaz\\gpu.on", TRUE)) {
        LogPrint("C:\\topaz\\gpu.on absent: read only\n");
        return;
    }
    LogPrint("C:\\topaz\\gpu.on found (deleted, one shot): powering the GPU up\n");
    g_Powered = TRUE;
    if (PowerUp()) {
        DumpClocks("powered");
        FirstGpuReads();
    } else {
        DumpClocks("power-up FAILED");
    }
}

static VOID HwStop(VOID)
{
    if (g_Powered && g_GpuCc != NULL) {
        PowerDown();
        g_Powered = FALSE;
    }
    if (g_Gcc != NULL) {
        MmUnmapIoSpace((PVOID)g_Gcc, GCC_SIZE);
        g_Gcc = NULL;
    }
    if (g_GpuCc != NULL) {
        MmUnmapIoSpace((PVOID)g_GpuCc, GPUCC_SIZE);
        g_GpuCc = NULL;
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazGpu " TOPAZ_GPU_VERSION " (power-up test) ====\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDFDEVICE device;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    HwStart();
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Translated);
    HwStop();
    return STATUS_SUCCESS;
}
