/*
 * TopazGpu - Adreno 610 bring-up (stage G1, docs/P8_gpu.md) on Redmi Note 12 4G (topaz, SM6225).
 *
 * v0.1 is READ ONLY: it logs what the bootloader left in the GPU clock tree, so the power-up
 * sequence of v0.2 starts from facts. Registers (Linux gcc-sm6115.c / gpucc-sm6115.c):
 *   GCC 0x1400000:  GPU_CFG_AHB_CBCR 0x36004 (critical, always on), GPU_MEMNOC_GFX 0x3600c,
 *                   GPU_SNOC_DVM_GFX 0x36018, GPU_IREF 0x36100, GPU_THROTTLE_CORE 0x36048,
 *                   vote reg 0x79004 (bit15 GPU_GPLL0_CLK_SRC, bit16 GPU_GPLL0_DIV, bit31 throttle).
 *   GPU CC 0x5990000: PLL0 0x0, PLL1 0x100, GX_GFX3D RCG 0x101c, GMU RCG 0x1120, branches
 *                   (CBCR bit31 = CLK_OFF, bit0 = enable), GX GDSC 0x100c, CX GDSC 0x106c
 *                   (GDSCR bit31 = PWR_ON, bit0 = SW_COLLAPSE), GX clamp 0x1508, CX hw ctrl 0x1540,
 *                   GPU SMMU vote 0x5000.
 * The GPU core (0x5900000) and its SMMU (0x59a0000) are NOT touched: with clocks or GDSCs off a read
 * there hangs the bus. Log: C:\TopazGpu.log.
 */
#include "driver.h"

#define TOPAZ_GPU_VERSION   "v0.1"

#define GCC_BASE            0x01400000ULL
#define GCC_SIZE            0x80000
#define GPUCC_BASE          0x05990000ULL
#define GPUCC_SIZE          0x9000

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

static VOID DumpClocks(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *base[2];
    ULONG i, v;
    CHAR dec[64];

    pa.QuadPart = (LONGLONG)GCC_BASE;
    base[0] = (volatile UCHAR *)MmMapIoSpaceEx(pa, GCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)GPUCC_BASE;
    base[1] = (volatile UCHAR *)MmMapIoSpaceEx(pa, GPUCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (base[0] == NULL || base[1] == NULL) {
        LogPrint("map failed: gcc %p gpucc %p\n", base[0], base[1]);
    } else {
        for (i = 0; i < ARRAYSIZE(g_Regs); i++) {
            v = READ_REGISTER_ULONG((volatile ULONG *)(base[g_Regs[i].Block] + g_Regs[i].Offset));
            LogPrint("%s+%05x %-34s %08x %s\n", g_Regs[i].Block ? "gpucc" : "gcc  ", g_Regs[i].Offset,
                     g_Regs[i].Name, v, Decode(g_Regs[i].Kind, v, dec, sizeof(dec)));
        }
    }
    if (base[0] != NULL) {
        MmUnmapIoSpace((PVOID)base[0], GCC_SIZE);
    }
    if (base[1] != NULL) {
        MmUnmapIoSpace((PVOID)base[1], GPUCC_SIZE);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazGpu " TOPAZ_GPU_VERSION " (read-only clock probe) ====\n");
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
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);
    return WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    DumpClocks();
    return STATUS_SUCCESS;
}
