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
 * v0.3: GPU powered, registers respond, always-on counter ticks at 19.2 MHz.
 * v0.6: CP start (docs/P8_gpu.md "G1 v0.6 plan"): identity GPU SMMU bank for SID 0 mask 1, hw_init
 * subset, SQE microcode (C:\topaz\fw\gpu\a630_sqe.fw minus its first dword), 32 KB ring,
 * CP_ME_INIT, CP_SET_SECURE_MODE 0 (zap), CP_MEM_WRITE 0xC0FFEE00 into a buffer the CPU reads back.
 * v0.5: mem_setup size aligned to 4 KB like Linux (v0.4: init_image ok, mem_setup with 0x830 ->
 * 0xffcfffba, auth_and_reset -> 0xffcfffbc); PAS shutdown before init_image.
 * v0.4: + GPU SMMU (0x59a0000) dump, read only; + zap shader through TZ PAS id 13 the way Linux
 * qcom_mdt_load() does it: metadata = the whole a610_zap.mdt (ELF header + hash), the one PT_LOAD
 * segment (relocatable, paddr 0x5000, a610_zap.b02) goes to the reserved region 0x55B15000.
 * Files: C:\topaz\fw\gpu\a610_zap.{mdt,b02}. If init_image fails because the zap is already
 * loaded (driver restart), SET_REMOTE_STATE(resume, 13) is tried like Linux does on GPU resume.
 * The GPU core (0x5900000) is read only after both GDSCs report PWR_ON and the core/AHB clocks run
 * (a read with them off hangs the bus). The SMMU (0x59a0000) is not touched yet. Log: C:\TopazGpu.log.
 */
#include "driver.h"

#define TOPAZ_GPU_VERSION   "v0.6"

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

#define SMMU_BASE           0x059A0000ULL
#define SMMU_SIZE           0x10000
#define ZAP_REGION          0x55B15000ULL
#define ZAP_REGION_SIZE     0x2000
#define GPU_PAS_ID          13

#define SCM_FN(svc, cmd)    (0x42000000u | ((ULONG)(svc) << 8) | (ULONG)(cmd))
#define SCM_SVC_BOOT        0x01
#define SCM_BOOT_SET_REMOTE_STATE 0x0a
#define SCM_SVC_PIL         0x02
#define PIL_INIT_IMAGE      0x01
#define PIL_MEM_SETUP       0x02
#define PIL_AUTH_RESET      0x05
#define PIL_SHUTDOWN        0x06
#define SCM_ARG_RW          2

typedef struct _ARM_SMC_ARGS {
    ULONG_PTR Arg0, Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7;
} ARM_SMC_ARGS;

VOID TopazArmCallSmc(ARM_SMC_ARGS *Args);

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
    /* CX power domain first: the GPU CC cxo/ahb branches live in it and stay CLK_OFF before (v0.2) */
    ok &= GdscOn(0x106c, 0, 0, 0x1540, "gpu_cx_gdsc");
    if (!ok) {
        return FALSE;
    }
    ok &= BranchOn(g_GpuCc, 0x109c, "gpucc_cxo");
    ok &= BranchOn(g_GpuCc, 0x1078, "gpucc_ahb");
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

/* ---- TZ calls (same as TopazModem ModemPas.c, incl. the INTERRUPTED resume loop) ---- */

static ULONG_PTR Scm(ULONG Fn, ULONG_PTR ArgInfo, ULONG_PTR A, ULONG_PTR B, ULONG_PTR C, ULONG_PTR *Res1)
{
    ARM_SMC_ARGS args;
    ULONG n;
    ULONG_PTR a6;

    RtlZeroMemory(&args, sizeof(args));
    args.Arg0 = Fn;
    args.Arg1 = ArgInfo;
    args.Arg2 = A;
    args.Arg3 = B;
    args.Arg4 = C;
    TopazArmCallSmc(&args);
    for (n = 0; args.Arg0 == 1 && n < 100000; n++) {
        a6 = args.Arg6;
        RtlZeroMemory(&args, sizeof(args));
        args.Arg0 = 1;
        args.Arg1 = ArgInfo;
        args.Arg2 = A;
        args.Arg3 = B;
        args.Arg4 = C;
        args.Arg6 = a6;
        TopazArmCallSmc(&args);
    }
    if (Res1 != NULL) {
        *Res1 = args.Arg1;
    }
    return args.Arg0;
}

/* Whole file into a nonpaged buffer (caller frees with ExFreePoolWithTag 'upGT'). */
static PUCHAR ReadWholeFile(PCWSTR Path, PULONG Size)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    HANDLE h;
    PUCHAR buf = NULL;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, GENERIC_READ | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                                 NULL, 0))) {
        return NULL;
    }
    if (NT_SUCCESS(ZwQueryInformationFile(h, &iosb, &info, sizeof(info), FileStandardInformation)) &&
        info.EndOfFile.QuadPart > 0 && info.EndOfFile.QuadPart < 0x100000) {
        *Size = info.EndOfFile.LowPart;
        buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, *Size, 'upGT');
        if (buf != NULL && !NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, *Size, NULL, NULL))) {
            ExFreePoolWithTag(buf, 'upGT');
            buf = NULL;
        }
    }
    ZwClose(h);
    return buf;
}

static VOID ZapLoad(VOID)
{
    PUCHAR mdt, seg = NULL, meta = NULL;
    ULONG mdtSize = 0, segSize = 0, phoff, phnum, i, ptype, off, paddr, filesz, memsz, flags;
    ULONG minAddr = MAXULONG, maxAddr = 0, loadIdx = MAXULONG;
    ULONG_PTR st, res = 0;
    PHYSICAL_ADDRESS lo, hi, bound, pa;
    volatile UCHAR *dst;

    LogPrint("--- zap shader (PAS %u)\n", GPU_PAS_ID);
    mdt = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a610_zap.mdt", &mdtSize);
    if (mdt == NULL || mdtSize < 52) {
        LogPrint("  a610_zap.mdt missing\n");
        goto out;
    }
    phoff = *(ULONG *)(mdt + 28);
    phnum = *(USHORT *)(mdt + 44);
    for (i = 0; i < phnum && phoff + 32 * (i + 1) <= mdtSize; i++) {
        ptype  = *(ULONG *)(mdt + phoff + 32 * i);
        off    = *(ULONG *)(mdt + phoff + 32 * i + 4);
        paddr  = *(ULONG *)(mdt + phoff + 32 * i + 12);
        filesz = *(ULONG *)(mdt + phoff + 32 * i + 16);
        memsz  = *(ULONG *)(mdt + phoff + 32 * i + 20);
        flags  = *(ULONG *)(mdt + phoff + 32 * i + 24);
        LogPrint("  phdr %u type %u off %x paddr %x filesz %u memsz %u flags %08x\n", i, ptype, off, paddr,
                 filesz, memsz, flags);
        if (ptype == 1 && memsz != 0 && ((flags >> 24) & 7) != 2) {
            minAddr = min(minAddr, paddr);
            maxAddr = max(maxAddr, (paddr + memsz + 0xFFF) & ~0xFFFu);   /* Linux: ALIGN(.., SZ_4K) */
            loadIdx = i;
        }
    }
    if (loadIdx == MAXULONG || maxAddr - minAddr > ZAP_REGION_SIZE) {
        LogPrint("  unexpected layout (load %u, %x..%x)\n", loadIdx, minAddr, maxAddr);
        goto out;
    }
    seg = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a610_zap.b02", &segSize);
    if (seg == NULL) {
        LogPrint("  a610_zap.b02 missing\n");
        goto out;
    }
    /* metadata: physically contiguous, below 4 GB, TZ reads it by PA */
    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;
    bound.QuadPart = 0;
    meta = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(ROUND_TO_PAGES(mdtSize), lo, hi, bound, MmWriteCombined);
    if (meta == NULL) {
        LogPrint("  metadata alloc failed\n");
        goto out;
    }
    RtlCopyMemory(meta, mdt, mdtSize);
    pa = MmGetPhysicalAddress(meta);
    /* v0.4 left PAS 13 half-initialised (init_image ok, mem_setup with an unaligned size refused):
       start from a clean state */
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, GPU_PAS_ID, 0, 0, &res);
    LogPrint("  shutdown: ret %llx res %llx\n", (ULONGLONG)st, (ULONGLONG)res);
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), GPU_PAS_ID, (ULONG_PTR)pa.QuadPart, 0, &res);
    LogPrint("  init_image: ret %llx res %llx (meta %u bytes @%llx)\n", (ULONGLONG)st, (ULONGLONG)res, mdtSize,
             pa.QuadPart);
    if (st != 0 || res != 0) {
        st = Scm(SCM_FN(SCM_SVC_BOOT, SCM_BOOT_SET_REMOTE_STATE), 2, 0, GPU_PAS_ID, 0, &res);
        LogPrint("  set_remote_state(resume): ret %llx res %llx (0/0 = zap already loaded, resumed)\n",
                 (ULONGLONG)st, (ULONGLONG)res);
        goto out;
    }
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, GPU_PAS_ID, (ULONG_PTR)ZAP_REGION, maxAddr - minAddr, &res);
    LogPrint("  mem_setup %llx+%x: ret %llx res %llx\n", ZAP_REGION, maxAddr - minAddr, (ULONGLONG)st, (ULONGLONG)res);
    pa.QuadPart = (LONGLONG)ZAP_REGION;
    dst = (volatile UCHAR *)MmMapIoSpaceEx(pa, ZAP_REGION_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (dst == NULL) {
        LogPrint("  map of the zap region failed\n");
        goto out;
    }
    RtlZeroMemory((PVOID)dst, ZAP_REGION_SIZE);
    paddr = *(ULONG *)(mdt + phoff + 32 * loadIdx + 12);
    RtlCopyMemory((PVOID)(dst + (paddr - minAddr)), seg, min(segSize, ZAP_REGION_SIZE - (paddr - minAddr)));
    KeMemoryBarrier();
    MmUnmapIoSpace((PVOID)dst, ZAP_REGION_SIZE);
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, GPU_PAS_ID, 0, 0, &res);
    LogPrint("  auth_and_reset: ret %llx res %llx (0/0 = TZ accepted the zap shader)\n", (ULONGLONG)st, (ULONGLONG)res);
out:
    if (meta != NULL) {
        MmFreeContiguousMemorySpecifyCache(meta, ROUND_TO_PAGES(mdtSize), MmWriteCombined);
    }
    if (seg != NULL) {
        ExFreePoolWithTag(seg, 'upGT');
    }
    if (mdt != NULL) {
        ExFreePoolWithTag(mdt, 'upGT');
    }
}

/* ---- GPU SMMU (read only) ---- */

static VOID SmmuDump(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *s, *cb;
    ULONG cr0, id0, id1, nsmr, ncb, psize, npage, i, smr, s2cr;

    pa.QuadPart = (LONGLONG)SMMU_BASE;
    s = (volatile UCHAR *)MmMapIoSpaceEx(pa, SMMU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (s == NULL) {
        LogPrint("SMMU map failed\n");
        return;
    }
    cr0 = Rd(s, 0x0);
    id0 = Rd(s, 0x20);
    id1 = Rd(s, 0x24);
    nsmr = id0 & 0xFF;
    ncb = id1 & 0xFF;
    psize = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
    npage = 1u << (((id1 >> 28) & 7) + 1);
    LogPrint("--- GPU SMMU: sCR0 %08x (CLIENTPD %u USFCFG %u) ID0 %08x ID1 %08x ID2 %08x: %u SMRs %u CBs page %x x%u\n",
             cr0, cr0 & 1, (cr0 >> 10) & 1, id0, id1, Rd(s, 0x28), nsmr, ncb, psize, npage);
    for (i = 0; i < nsmr && i < 64; i++) {
        smr = Rd(s, 0x800 + 4 * i);
        s2cr = Rd(s, 0xC00 + 4 * i);
        if (smr != 0 || s2cr != 0) {
            LogPrint("  SMR%u %08x (valid %u sid %x mask %x) S2CR %08x type %u cb %u\n", i, smr, smr >> 31,
                     smr & 0xFFFF, (smr >> 16) & 0x7FFF, s2cr, (s2cr >> 16) & 3, s2cr & 0xFF);
        }
    }
    for (i = 0; i < ncb && (npage + i + 1) * psize <= SMMU_SIZE; i++) {
        cb = s + (SIZE_T)(npage + i) * psize;
        LogPrint("  CB%u CBAR %08x CBA2R %08x SCTLR %08x TCR %08x TTBR0 %08x%08x FSR %08x\n", i,
                 Rd(s, psize + 4 * i), Rd(s, psize + 0x800 + 4 * i), Rd(cb, 0x0), Rd(cb, 0x30), Rd(cb, 0x24),
                 Rd(cb, 0x20), Rd(cb, 0x58));
    }
    MmUnmapIoSpace((PVOID)s, SMMU_SIZE);
}

/* ---- CP start (v0.6) ---- */

#define RB_BYTES            0x8000
#define RB_DWORDS           (RB_BYTES / 4)
#define CP_ME_INIT          0x48
#define CP_SET_SECURE_MODE  0x66
#define CP_MEM_WRITE        0x3d
#define CP_NOP              0x10

static volatile UCHAR *g_Gpu;
static PULONG g_Ring, g_Sqe, g_Test;
static ULONG g_SqeBytes, g_Wptr;

static ULONG Par(ULONG V)
{
    return (0x9669 >> (0xF & (V ^ (V >> 4) ^ (V >> 8) ^ (V >> 12) ^ (V >> 16) ^ (V >> 20) ^ (V >> 24) ^ (V >> 28)))) & 1;
}

static ULONG Pkt7(ULONG Op, ULONG Cnt)
{
    return 0x70000000u | Cnt | (Par(Cnt) << 15) | ((Op & 0x7F) << 16) | (Par(Op) << 23);
}

static VOID GpuWr(ULONG Reg, ULONG Val)
{
    Wr(g_Gpu, 4 * Reg, Val);
}

static ULONG GpuRd(ULONG Reg)
{
    return Rd(g_Gpu, 4 * Reg);
}

static VOID GpuWr64(ULONG Reg, ULONGLONG Val)
{
    GpuWr(Reg, (ULONG)Val);
    GpuWr(Reg + 1, (ULONG)(Val >> 32));
}

static PULONG DmaAlloc(ULONG Bytes, PULONGLONG Pa)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    PVOID va;

    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;
    bound.QuadPart = 0;
    va = MmAllocateContiguousMemorySpecifyCache(Bytes, lo, hi, bound, MmNonCached);
    if (va != NULL) {
        RtlZeroMemory(va, Bytes);
        *Pa = (ULONGLONG)MmGetPhysicalAddress(va).QuadPart;
    }
    return (PULONG)va;
}

static VOID Emit(ULONG V)
{
    g_Ring[g_Wptr % RB_DWORDS] = V;
    g_Wptr++;
}

static VOID SmmuFault(PCSTR Tag)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *s;

    pa.QuadPart = (LONGLONG)SMMU_BASE;
    s = (volatile UCHAR *)MmMapIoSpaceEx(pa, SMMU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (s != NULL) {
        LogPrint("  [%s] SMMU gfsr %08x CB0 SCTLR %08x FSR %08x FAR %08x%08x FSYNR0 %08x\n", Tag, Rd(s, 0x48),
                 Rd(s, 0x8000), Rd(s, 0x8058), Rd(s, 0x8064), Rd(s, 0x8060), Rd(s, 0x8068));
        MmUnmapIoSpace((PVOID)s, SMMU_SIZE);
    }
}

/* Kick the ring and wait until the CP has consumed everything (RPTR == WPTR), up to Ms. */
static BOOLEAN Submit(PCSTR What, ULONG Ms)
{
    ULONG i, rptr = 0, wptr = g_Wptr % RB_DWORDS;

    KeMemoryBarrier();
    GpuWr(0x807, wptr);                                  /* CP_RB_WPTR */
    for (i = 0; i < Ms * 100; i++) {
        rptr = GpuRd(0x806);                             /* CP_RB_RPTR */
        if (rptr == wptr) {
            LogPrint("  %s: done (rptr %u) after ~%u us, RBBM_STATUS %08x\n", What, rptr, i * 10, GpuRd(0x210));
            return TRUE;
        }
        KeStallExecutionProcessor(10);
    }
    LogPrint("  %s: TIMEOUT rptr %u wptr %u RBBM_STATUS %08x INT_0_STATUS %08x CP_HW_FAULT %08x\n", What, rptr,
             wptr, GpuRd(0x210), GpuRd(0x201), GpuRd(0x821));
    SmmuFault(What);
    return FALSE;
}

static BOOLEAN SmmuIdentity(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *s;

    pa.QuadPart = (LONGLONG)SMMU_BASE;
    s = (volatile UCHAR *)MmMapIoSpaceEx(pa, SMMU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (s == NULL) {
        return FALSE;
    }
    /* CB0 (page 8 of 4 KB): pass-through clone of what UEFI uses on the apps SMMU (TopazModem v0.7) */
    Wr(s, 0x8058, 0xFFFFFFFF);                           /* FSR clear */
    Wr(s, 0x8020, 0);
    Wr(s, 0x8024, 0);                                    /* TTBR0 */
    Wr(s, 0x8030, 0);                                    /* TCR */
    Wr(s, 0x8010, 0);                                    /* TCR2 */
    Wr(s, 0x8038, 0);
    Wr(s, 0x803C, 0);                                    /* MAIR */
    Wr(s, 0x1000 + 0x800, 1);                            /* CBA2R0: VA64 */
    Wr(s, 0x1000, 0x0001f000);                           /* CBAR0 */
    KeMemoryBarrier();
    Wr(s, 0x8000, 0xE0);                                 /* SCTLR: M=0, fault report/irq */
    KeMemoryBarrier();
    Wr(s, 0xC00, 0);                                     /* S2CR0: translate via CB0 */
    Wr(s, 0x800, (1u << 31) | (1u << 16) | 0);           /* SMR0: SID 0 mask 1 */
    KeMemoryBarrier();
    LogPrint("  SMMU: SMR0 %08x S2CR0 %08x CBAR0 %08x CBA2R0 %08x CB0 SCTLR %08x\n", Rd(s, 0x800), Rd(s, 0xC00),
             Rd(s, 0x1000), Rd(s, 0x1800), Rd(s, 0x8000));
    MmUnmapIoSpace((PVOID)s, SMMU_SIZE);
    return TRUE;
}

static VOID HwInitRegs(VOID)
{
    ULONG i;
    static const ULONG addrMode[] = { 0x842, 0xC01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0xE00,
                                      0xae01, 0xb601, 0xF810 };

    GpuWr(0x3c45, 0);                                    /* GBIF_HALT */
    GpuRd(0x3c45);
    GpuWr(0x16, 0);                                      /* RBBM_GBIF_HALT */
    GpuRd(0x16);
    GpuWr(0xF803, 0);                                    /* RBBM_SECVID_TSB_CNTL */
    GpuWr64(0xF800, 0);                                  /* TSB_TRUSTED_BASE */
    GpuWr(0xF802, 0);                                    /* TSB_TRUSTED_SIZE */
    for (i = 0; i < ARRAYSIZE(addrMode); i++) {
        GpuWr(addrMode[i], 1);                           /* 64-bit addressing */
    }
    for (i = 0; i < 4; i++) {
        GpuWr(0x3c03 + i, 0x00071620);                   /* GBIF_QSB_SIDE0..3 (a610) */
    }
    GpuWr(0x11, 3);                                      /* RBBM_GBIF_CLIENT_QOS_CNTL */
    GpuWr64(0xE05, 0x1fffffffff000ull + 0xfc0);          /* UCHE_WRITE_RANGE_MAX */
    GpuWr64(0xE09, 0x1fffffffff000ull);                  /* UCHE_TRAP_BASE */
    GpuWr64(0xE07, 0x1fffffffff000ull);                  /* UCHE_WRITE_THRU_BASE */
    GpuWr64(0xE0B, 0x100000);                            /* UCHE_GMEM_RANGE_MIN */
    GpuWr64(0xE0D, 0x100000 + 0x21000 - 1);              /* .._MAX: gmem 128K + 4K */
    GpuWr(0xE18, 0x804);                                 /* UCHE_FILTER_CNTL */
    GpuWr(0xE17, 4);                                     /* UCHE_CACHE_WAYS */
    GpuWr(0x8c2, 0x00800060);                            /* CP_ROQ_THRESHOLDS_2 */
    GpuWr(0x8c1, 0x40201b16);                            /* CP_ROQ_THRESHOLDS_1 */
    GpuWr(0x8C3, 48);                                    /* CP_MEM_POOL_SIZE */
    GpuWr(0x9e00, 0x00080000);                           /* PC_DBG_ECO_CNTL: prim fifo threshold */
    GpuWr(0x98d, 1);                                     /* CP_AHB_CNTL */
    GpuWr(0x1f, (1u << 30) | 0x3ffff);                   /* RBBM_INTERFACE_HANG_INT_CNTL */
    GpuWr(0xe19, 0x81);                                  /* UCHE_CLIENT_PF */
}

static VOID CpStart(VOID)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR fw;
    ULONG fwSize = 0;
    ULONGLONG ringPa, sqePa, testPa;

    LogPrint("--- CP start\n");
    pa.QuadPart = (LONGLONG)GPU_BASE;
    g_Gpu = (volatile UCHAR *)MmMapIoSpaceEx(pa, GPU_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    fw = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a630_sqe.fw", &fwSize);
    if (g_Gpu == NULL || fw == NULL || fwSize <= 4) {
        LogPrint("  map/firmware failed (gpu %p sqe %p %u)\n", g_Gpu, fw, fwSize);
        goto out;
    }
    g_SqeBytes = ROUND_TO_PAGES(fwSize - 4);
    g_Sqe = DmaAlloc(g_SqeBytes, &sqePa);
    g_Ring = DmaAlloc(RB_BYTES, &ringPa);
    g_Test = DmaAlloc(PAGE_SIZE, &testPa);
    if (g_Sqe == NULL || g_Ring == NULL || g_Test == NULL) {
        LogPrint("  DMA alloc failed\n");
        goto out;
    }
    RtlCopyMemory(g_Sqe, fw + 4, fwSize - 4);
    g_Test[0] = 0x11111111;
    LogPrint("  sqe %u bytes (first dword %08x skipped) @%llx, ring @%llx, test @%llx\n", fwSize - 4,
             *(ULONG *)fw, sqePa, ringPa, testPa);
    if (!SmmuIdentity()) {
        LogPrint("  SMMU map failed\n");
        goto out;
    }
    HwInitRegs();
    GpuWr64(0x830, sqePa);                               /* CP_SQE_INSTR_BASE */
    GpuWr64(0x800, ringPa);                              /* CP_RB_BASE */
    GpuWr(0x802, 12 | (2 << 8) | (1u << 27));            /* CP_RB_CNTL: 32 KB, blk 32 B, NO_UPDATE */
    g_Wptr = 0;
    GpuWr(0x807, 0);
    GpuWr(0x808, 1);                                     /* CP_SQE_CNTL: start */
    LogPrint("  SQE started: RBBM_STATUS %08x CP_HW_FAULT %08x rptr %u\n", GpuRd(0x210), GpuRd(0x821), GpuRd(0x806));

    Emit(Pkt7(CP_ME_INIT, 8));
    Emit(0x2f);
    Emit(3);
    Emit(0x20000000);
    Emit(0);
    Emit(0);
    Emit(0);
    Emit(0);
    Emit(0);
    if (!Submit("CP_ME_INIT", 100)) {
        goto out;
    }
    Emit(Pkt7(CP_SET_SECURE_MODE, 1));
    Emit(0);
    if (!Submit("CP_SET_SECURE_MODE 0", 100)) {
        goto out;
    }
    Emit(Pkt7(CP_MEM_WRITE, 3));
    Emit((ULONG)testPa);
    Emit((ULONG)(testPa >> 32));
    Emit(0xC0FFEE00);
    Submit("CP_MEM_WRITE", 100);
    KeStallExecutionProcessor(100);
    LogPrint("  test buffer: %08x (0xC0FFEE00 = the GPU wrote to memory)\n", g_Test[0]);
    SmmuFault("after test");
out:
    if (fw != NULL) {
        ExFreePoolWithTag(fw, 'upGT');
    }
}

static VOID CpStop(VOID)
{
    if (g_Gpu != NULL) {
        GpuWr(0x808, 0);                                 /* CP_SQE_CNTL: stop */
        MmUnmapIoSpace((PVOID)g_Gpu, GPU_SIZE);
        g_Gpu = NULL;
    }
}

static VOID CpFree(VOID)
{
    if (g_Sqe != NULL) {
        MmFreeContiguousMemorySpecifyCache(g_Sqe, g_SqeBytes, MmNonCached);
        g_Sqe = NULL;
    }
    if (g_Ring != NULL) {
        MmFreeContiguousMemorySpecifyCache(g_Ring, RB_BYTES, MmNonCached);
        g_Ring = NULL;
    }
    if (g_Test != NULL) {
        MmFreeContiguousMemorySpecifyCache(g_Test, PAGE_SIZE, MmNonCached);
        g_Test = NULL;
    }
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
        SmmuDump();
        ZapLoad();
        CpStart();
    } else {
        DumpClocks("power-up FAILED");
    }
}

static VOID HwStop(VOID)
{
    CpStop();
    if (g_Powered && g_GpuCc != NULL) {
        PowerDown();
        g_Powered = FALSE;
    }
    CpFree();
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
