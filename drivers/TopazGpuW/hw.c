/*
 * Adreno 610 hardware layer of TopazGpuW. The power-up, zap and CP start sequences are the ones
 * proven by the TopazGpu KMDF test driver (G1, docs/P8_gpu.md); new here: real SMMU stage-1 page
 * tables (ARMv8 LPAE, 4 KB granule, 39-bit input, one context bank for everything) so BOs can be
 * scattered pages at stable GPU VAs, IB submission and fences.
 */
#include "tgpu.h"
#include <intrin.h>

#define GCC_BASE            0x01400000ULL
#define GCC_SIZE            0x80000
#define GPUCC_BASE          0x05990000ULL
#define GPUCC_SIZE          0x9000
#define GPU_BASE            0x05900000ULL
#define GPU_SIZE            0x40000
#define SMMU_BASE           0x059A0000ULL
#define SMMU_SIZE           0x10000
#define SMMU_CB0            0x8000          /* 4 KB pages x 8: context banks start at +0x8000 */
#define ZAP_REGION          0x55B15000ULL
#define ZAP_REGION_SIZE     0x2000
#define GPU_PAS_ID          13

#define CBCR_EN             (1u << 0)
#define CBCR_OFF            (1u << 31)
#define GDSC_COLLAPSE       (1u << 0)
#define GDSC_PWR_ON         (1u << 31)
#define RCG_UPDATE          (1u << 0)
#define SRC_GPLL0           5

#define SCM_FN(svc, cmd)    (0x42000000u | ((ULONG)(svc) << 8) | (ULONG)(cmd))
#define SCM_SVC_PIL         0x02
#define PIL_INIT_IMAGE      0x01
#define PIL_MEM_SETUP       0x02
#define PIL_AUTH_RESET      0x05
#define PIL_SHUTDOWN        0x06
#define SCM_ARG_RW          2

#define RB_BYTES            0x8000
#define RB_DWORDS           (RB_BYTES / 4)
#define CP_NOP              0x10
#define CP_INDIRECT_BUFFER  0x3f
#define CP_EVENT_WRITE      0x46
#define CP_ME_INIT          0x48
#define CP_SET_SECURE_MODE  0x66
#define CACHE_FLUSH_TS      4

typedef struct _ARM_SMC_ARGS {
    ULONG_PTR Arg0, Arg1, Arg2, Arg3, Arg4, Arg5, Arg6, Arg7;
} ARM_SMC_ARGS;
VOID TopazArmCallSmc(ARM_SMC_ARGS *Args);

/* kernel-owned GPU buffer: pages + WC kernel mapping + GPU VA */
typedef struct _KBUF {
    PMDL      Mdl;
    PVOID     Va;
    ULONGLONG Iova;
    SIZE_T    Size;
} KBUF;

static volatile UCHAR *g_Gcc, *g_GpuCc, *g_Gpu, *g_Smmu, *g_Gmu;
static BOOLEAN g_Ready, g_Failed, g_SmmuOn;
static BOOLEAN g_Wedged;                                 /* v0.18: a fence timed out, GPU hung */
static ULONG g_GmemSize = 0x21000;                       /* 128K + 4K (bengal); v0.24: C:\topaz\gpu.gmem overrides */
static KMUTEX g_HwLock;          /* not FAST_MUTEX: file I/O (zap, SQE) at APC_LEVEL deadlocks */
static KBUF g_Ring, g_Sqe, g_Mem;               /* g_Mem: fence at +0 */
static ULONG g_Wptr, g_Seqno;

/* page tables */
static PULONGLONG g_L1;
static PHYSICAL_ADDRESS g_L1Pa;
static PULONGLONG g_L2[4];                      /* VA < 4 GB: L1 entries 0..3 */
static PULONGLONG g_L3[2048];                   /* one per 2 MB */

/* VA allocator: first-fit list of free ranges */
typedef struct _VA_RANGE {
    LIST_ENTRY Link;
    ULONGLONG  Start, Size;
} VA_RANGE;
static LIST_ENTRY g_VaFree;
static ULONGLONG g_KernelVaNext = TGPU_VA_KERNEL;

/* ---------------------------------------------------------------- MMIO helpers */

static ULONG Rd(volatile UCHAR *B, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(B + Off)); }
static VOID Wr(volatile UCHAR *B, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(B + Off), V); }
static VOID Rmw(volatile UCHAR *B, ULONG Off, ULONG Clr, ULONG Set) { Wr(B, Off, (Rd(B, Off) & ~Clr) | Set); }
static VOID GpuWr(ULONG Reg, ULONG V) { Wr(g_Gpu, 4 * Reg, V); }
static ULONG GpuRd(ULONG Reg) { return Rd(g_Gpu, 4 * Reg); }
static VOID GpuWr64(ULONG Reg, ULONGLONG V) { GpuWr(Reg, (ULONG)V); GpuWr(Reg + 1, (ULONG)(V >> 32)); }

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

static volatile UCHAR *MapIo(ULONGLONG Pa, SIZE_T Size)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)Pa;
    return (volatile UCHAR *)MmMapIoSpaceEx(pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

/* ---------------------------------------------------------------- power (TopazGpu v0.3) */

static BOOLEAN BranchOn(volatile UCHAR *B, ULONG Off, PCSTR Name)
{
    BOOLEAN ok;
    Rmw(B, Off, 0, CBCR_EN);
    ok = Poll(B, Off, CBCR_OFF, 0, 200);
    if (!ok) {
        LogPrint("  %s stays off (%08x)\n", Name, Rd(B, Off));
    }
    return ok;
}

static BOOLEAN RcgSet(ULONG Cmd, ULONG Src, ULONG Div)
{
    Wr(g_GpuCc, Cmd + 4, (Src << 8) | (2 * Div - 1));
    Rmw(g_GpuCc, Cmd, 0, RCG_UPDATE);
    return Poll(g_GpuCc, Cmd, RCG_UPDATE, 0, 500);
}

static BOOLEAN GdscOn(ULONG Gdscr, ULONG Bcr, ULONG Clamp, ULONG Status)
{
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
    return Poll(g_GpuCc, Status, GDSC_PWR_ON, GDSC_PWR_ON, 500);
}

/* A previous (failed) start can leave the GPU powered with a wedged CP: collapse GX then CX first so
   PowerUp starts from reset state. */
static VOID PowerCycleIfOn(VOID)
{
    if ((Rd(g_GpuCc, 0x100c) & GDSC_PWR_ON) == 0 && (Rd(g_GpuCc, 0x1540) & GDSC_PWR_ON) == 0) {
        return;
    }
    LogPrint("  GPU already powered (GX %08x CX %08x): power cycling\n", Rd(g_GpuCc, 0x100c), Rd(g_GpuCc, 0x1540));
    if (Rd(g_GpuCc, 0x100c) & GDSC_PWR_ON) {
        Wr(g_Gpu, 4 * 0x808, 0);                         /* CP_SQE_CNTL: stop */
    }
    Rmw(g_GpuCc, 0x1054, CBCR_EN, 0);                    /* gx_gfx3d off */
    Rmw(g_GpuCc, 0x100c, 0, GDSC_COLLAPSE);              /* GX GDSC collapse */
    Rmw(g_GpuCc, 0x1508, 0, 1);                          /* GX clamp */
    Poll(g_GpuCc, 0x100c, GDSC_PWR_ON, 0, 1000);
    Rmw(g_GpuCc, 0x1098, CBCR_EN, 0);
    Rmw(g_GpuCc, 0x106c, 0, GDSC_COLLAPSE);              /* CX GDSC collapse (votable: may stay on) */
    KeStallExecutionProcessor(1000);
    LogPrint("  after collapse: GX %08x CX %08x\n", Rd(g_GpuCc, 0x100c), Rd(g_GpuCc, 0x1540));
}

/* v0.21: SP/TP RAM power (SPTPRAC) through the GMU wrapper (sm6115 DT: gmu@596a000). Linux does this
   for the gmu-wrapper A619 (holi) only; bengal's A610 does not need it, but khaje (our SM6225) is not
   upstream. Step B: blits work, every 3D draw hangs with SP/HLSQ busy. Read first, power on if off. */
#define GMU_BASE            0x0596A000ULL
#define GMU_SIZE            0x30000
#define GMU_SPTPRAC_CLK     (4 * 0x80)                   /* GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL */
#define GMU_SPTPRAC_PWR     (4 * 0x81)                   /* GMU_GX_SPTPRAC_POWER_CONTROL */
#define GMU_SPTPRAC_STATUS  (4 * 0x50d0)                 /* GMU_SPTPRAC_PWR_CLK_STATUS */

static VOID Sptprac(VOID);
static VOID Sptprac(VOID)
{
    ULONG st, i;

    if (g_Gmu == NULL) {
        g_Gmu = MapIo(GMU_BASE, GMU_SIZE);
        if (g_Gmu == NULL) {
            LogPrint("  sptprac: map failed\n");
            return;
        }
    }
    st = Rd(g_Gmu, GMU_SPTPRAC_STATUS);
    LogPrint("  sptprac: status %08x pwr %08x clk %08x\n", st, Rd(g_Gmu, GMU_SPTPRAC_PWR), Rd(g_Gmu, GMU_SPTPRAC_CLK));
    if ((st & 0x38) == 0x28) {
        return;                                          /* GDSC power on + clock on */
    }
    Wr(g_Gmu, GMU_SPTPRAC_PWR, 0x778000);
    for (i = 0; i < 100 && (Rd(g_Gmu, GMU_SPTPRAC_STATUS) & 0x38) != 0x28; i++) {
        KeStallExecutionProcessor(1);
    }
    LogPrint("  sptprac: power on -> status %08x after %u us\n", Rd(g_Gmu, GMU_SPTPRAC_STATUS), i);
}

static BOOLEAN PowerUp(VOID)
{
    PowerCycleIfOn();
    Rmw(g_Gcc, 0x79004, 0, 1u << 15);                    /* GPLL0 to the GPU CC */
    BranchOn(g_Gcc, 0x71154, "gcc_bimc_gpu_axi");
    BranchOn(g_Gcc, 0x3600c, "gcc_gpu_memnoc_gfx");      /* voted: may read off */
    BranchOn(g_Gcc, 0x36018, "gcc_gpu_snoc_dvm_gfx");
    if (!GdscOn(0x106c, 0, 0, 0x1540)) {
        LogPrint("  CX GDSC: no PWR_ON\n");
        return FALSE;
    }
    if (!BranchOn(g_GpuCc, 0x109c, "gpucc_cxo") || !BranchOn(g_GpuCc, 0x1078, "gpucc_ahb")) {
        return FALSE;
    }
    RcgSet(0x1120, SRC_GPLL0, 3);                        /* gmu 200 MHz */
    Rmw(g_GpuCc, 0x1098, 0xFF0, 0xFF0);
    BranchOn(g_GpuCc, 0x1098, "gpucc_cx_gmu");
    BranchOn(g_GpuCc, 0x5000, "gpucc_hlos1_vote_gpu_smmu");
    if (!GdscOn(0x100c, 0x1008, 0x1508, 0x100c)) {
        LogPrint("  GX GDSC: no PWR_ON\n");
        return FALSE;
    }
    if (!RcgSet(0x101c, SRC_GPLL0, 2)) {                 /* core 300 MHz */
        return FALSE;
    }
    Rmw(g_GpuCc, 0x1054, 0, (1u << 14) | (1u << 13));
    if (!BranchOn(g_GpuCc, 0x1054, "gpucc_gx_gfx3d")) {
        return FALSE;
    }
    BranchOn(g_GpuCc, 0x1060, "gpucc_gx_cxo");
    return TRUE;
}

/* ---------------------------------------------------------------- TZ: zap shader (TopazGpu v0.5) */

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
        buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, *Size, TGPU_POOL_TAG);
        if (buf != NULL && !NT_SUCCESS(ZwReadFile(h, NULL, NULL, NULL, &iosb, buf, *Size, NULL, NULL))) {
            ExFreePoolWithTag(buf, TGPU_POOL_TAG);
            buf = NULL;
        }
    }
    ZwClose(h);
    return buf;
}

/* metadata = the whole .mdt; the one PT_LOAD segment (a610_zap.b02) goes to ZAP_REGION */
static BOOLEAN ZapLoad(VOID)
{
    PUCHAR mdt, seg = NULL, meta = NULL;
    ULONG mdtSize = 0, segSize = 0, phoff, phnum, i, paddr, memsz, flags, minA = MAXULONG, maxA = 0, segPaddr = 0;
    ULONG_PTR st, res = 0;
    PHYSICAL_ADDRESS lo, hi, bound, pa;
    volatile UCHAR *dst;
    BOOLEAN ok = FALSE;

    mdt = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a610_zap.mdt", &mdtSize);
    seg = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a610_zap.b02", &segSize);
    if (mdt == NULL || seg == NULL || mdtSize < 52) {
        LogPrint("  zap files missing\n");
        goto out;
    }
    phoff = *(ULONG *)(mdt + 28);
    phnum = *(USHORT *)(mdt + 44);
    for (i = 0; i < phnum && phoff + 32 * (i + 1) <= mdtSize; i++) {
        paddr = *(ULONG *)(mdt + phoff + 32 * i + 12);
        memsz = *(ULONG *)(mdt + phoff + 32 * i + 20);
        flags = *(ULONG *)(mdt + phoff + 32 * i + 24);
        if (*(ULONG *)(mdt + phoff + 32 * i) == 1 && memsz != 0 && ((flags >> 24) & 7) != 2) {
            minA = min(minA, paddr);
            maxA = max(maxA, (paddr + memsz + 0xFFF) & ~0xFFFu);
            segPaddr = paddr;
        }
    }
    if (maxA <= minA || maxA - minA > ZAP_REGION_SIZE) {
        LogPrint("  zap layout unexpected\n");
        goto out;
    }
    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;
    bound.QuadPart = 0;
    meta = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(ROUND_TO_PAGES(mdtSize), lo, hi, bound, MmWriteCombined);
    if (meta == NULL) {
        goto out;
    }
    RtlCopyMemory(meta, mdt, mdtSize);
    pa = MmGetPhysicalAddress(meta);
    Scm(SCM_FN(SCM_SVC_PIL, PIL_SHUTDOWN), 1, GPU_PAS_ID, 0, 0, &res);
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_INIT_IMAGE), 2 | (SCM_ARG_RW << 6), GPU_PAS_ID, (ULONG_PTR)pa.QuadPart, 0, &res);
    if (st != 0 || res != 0) {
        LogPrint("  zap init_image: %llx/%llx\n", (ULONGLONG)st, (ULONGLONG)res);
        goto out;
    }
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_MEM_SETUP), 3, GPU_PAS_ID, (ULONG_PTR)ZAP_REGION, maxA - minA, &res);
    if (st != 0 || res != 0) {
        LogPrint("  zap mem_setup: %llx/%llx\n", (ULONGLONG)st, (ULONGLONG)res);
        goto out;
    }
    dst = MapIo(ZAP_REGION, ZAP_REGION_SIZE);
    if (dst == NULL) {
        goto out;
    }
    RtlZeroMemory((PVOID)dst, ZAP_REGION_SIZE);
    RtlCopyMemory((PVOID)(dst + (segPaddr - minA)), seg, min(segSize, ZAP_REGION_SIZE - (segPaddr - minA)));
    __dsb(_ARM64_BARRIER_SY);
    MmUnmapIoSpace((PVOID)dst, ZAP_REGION_SIZE);
    st = Scm(SCM_FN(SCM_SVC_PIL, PIL_AUTH_RESET), 1, GPU_PAS_ID, 0, 0, &res);
    LogPrint("  zap auth_and_reset: %llx/%llx\n", (ULONGLONG)st, (ULONGLONG)res);
    ok = (st == 0 && res == 0);
out:
    if (meta != NULL) {
        MmFreeContiguousMemorySpecifyCache(meta, ROUND_TO_PAGES(mdtSize), MmWriteCombined);
    }
    if (seg != NULL) {
        ExFreePoolWithTag(seg, TGPU_POOL_TAG);
    }
    if (mdt != NULL) {
        ExFreePoolWithTag(mdt, TGPU_POOL_TAG);
    }
    return ok;
}

/* ---------------------------------------------------------------- SMMU stage-1 page tables */

#define PTE_VALID       (1ull << 0)
#define PTE_TABLE       (1ull << 1)             /* table (L1/L2) or page (L3) */
#define PTE_AP_UNPRIV   (1ull << 6)             /* AP[1]: EL0 access (GPU is unprivileged) */
#define PTE_SH_OUTER    (2ull << 8)
#define PTE_AF          (1ull << 10)
#define PTE_PAGE        (PTE_VALID | PTE_TABLE | PTE_AP_UNPRIV | PTE_SH_OUTER | PTE_AF)    /* AttrIndx 0 */

static PULONGLONG TableAlloc(PHYSICAL_ADDRESS *Pa)
{
    PHYSICAL_ADDRESS lo, hi, bound;
    PULONGLONG t;

    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;                          /* v0.14: walks/fetches above 4 GB faulted */
    bound.QuadPart = 0;
    t = (PULONGLONG)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, bound, MmNonCached);
    if (t != NULL) {
        RtlZeroMemory(t, PAGE_SIZE);
        *Pa = MmGetPhysicalAddress(t);
    }
    return t;
}

static PULONGLONG L3For(ULONGLONG Va, BOOLEAN Create)
{
    ULONG i1 = (ULONG)(Va >> 30), i2 = (ULONG)((Va >> 21) & 511), i3 = (ULONG)(Va >> 21);
    PHYSICAL_ADDRESS pa;

    if (i1 >= 4) {
        return NULL;
    }
    if (g_L2[i1] == NULL) {
        if (!Create || (g_L2[i1] = TableAlloc(&pa)) == NULL) {
            return NULL;
        }
        g_L1[i1] = (ULONGLONG)pa.QuadPart | PTE_VALID | PTE_TABLE;
    }
    if (g_L3[i3] == NULL) {
        if (!Create || (g_L3[i3] = TableAlloc(&pa)) == NULL) {
            return NULL;
        }
        g_L2[i1][i2] = (ULONGLONG)pa.QuadPart | PTE_VALID | PTE_TABLE;
    }
    return g_L3[i3];
}

static VOID TlbFlush(VOID)
{
    __dsb(_ARM64_BARRIER_SY);
    Wr(g_Smmu, SMMU_CB0 + 0x618, 0);                     /* CB_TLBIALL */
    Wr(g_Smmu, SMMU_CB0 + 0x7F0, 0);                     /* CB_TLBSYNC */
    Poll(g_Smmu, SMMU_CB0 + 0x7F4, 1, 0, 1000);          /* CB_TLBSTATUS.SACTIVE */
}

NTSTATUS MmuMap(ULONGLONG Va, PMDL Mdl, SIZE_T Size)
{
    PPFN_NUMBER pfn = MmGetMdlPfnArray(Mdl);
    SIZE_T i, n = Size >> PAGE_SHIFT;
    PULONGLONG l3;

    for (i = 0; i < n; i++) {
        l3 = L3For(Va + (i << PAGE_SHIFT), TRUE);
        if (l3 == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        l3[((Va >> PAGE_SHIFT) + i) & 511] = ((ULONGLONG)pfn[i] << PAGE_SHIFT) | PTE_PAGE;
    }
    if (g_SmmuOn) {
        TlbFlush();
    }
    return STATUS_SUCCESS;
}

VOID MmuUnmap(ULONGLONG Va, SIZE_T Size)
{
    SIZE_T i, n = Size >> PAGE_SHIFT;
    PULONGLONG l3;

    for (i = 0; i < n; i++) {
        l3 = L3For(Va + (i << PAGE_SHIFT), FALSE);
        if (l3 != NULL) {
            l3[((Va >> PAGE_SHIFT) + i) & 511] = 0;
        }
    }
    if (g_SmmuOn) {
        TlbFlush();
    }
}

static BOOLEAN SmmuSetup(VOID)
{
    g_L1 = TableAlloc(&g_L1Pa);
    if (g_L1 == NULL) {
        return FALSE;
    }
    Wr(g_Smmu, SMMU_CB0 + 0x000, 0);                     /* SCTLR off while programming */
    Wr(g_Smmu, SMMU_CB0 + 0x058, 0xFFFFFFFF);            /* FSR clear */
    Wr(g_Smmu, 0x1000 + 0x800, 1);                       /* CBA2R0: AArch64 descriptors */
    Wr(g_Smmu, 0x1000, 0x0001f000);                      /* CBAR0: S1 translate, S2 bypass */
    Wr(g_Smmu, SMMU_CB0 + 0x020, (ULONG)g_L1Pa.QuadPart);
    Wr(g_Smmu, SMMU_CB0 + 0x024, (ULONG)(g_L1Pa.QuadPart >> 32));   /* TTBR0, ASID 0 */
    /* TCR: T0SZ 32 = exactly the 4 GB the A610 uses (walk starts at level 1, 4 entries), 4 KB granule,
       non-cacheable walks, EPD1. TCR2: PASize 36-bit = this SMMU's OAS (IDR2 0x5511); v0.12 had 39-bit
       VA + 40-bit PA and got a translation fault at level 0 on the first CP fetch. */
    Wr(g_Smmu, SMMU_CB0 + 0x030, 32 | (1u << 23));
    Wr(g_Smmu, SMMU_CB0 + 0x010, 1);
    Wr(g_Smmu, SMMU_CB0 + 0x038, 0x44);                  /* MAIR0: attr0 = Normal non-cacheable */
    Wr(g_Smmu, SMMU_CB0 + 0x03C, 0);
    __dsb(_ARM64_BARRIER_SY);
#if TGPU_IDENTITY
    Wr(g_Smmu, SMMU_CB0 + 0x000, 0xE0);                  /* SCTLR: M=0 pass-through (G1) */
#else
    Wr(g_Smmu, SMMU_CB0 + 0x000, 0x67);                  /* SCTLR: CFIE CFRE AFE TRE M */
#endif
    Wr(g_Smmu, 0xC00, 0);                                /* S2CR0 -> CB0 */
    Wr(g_Smmu, 0x800, (1u << 31) | (1u << 16));          /* SMR0: SID 0 mask 1 */
    g_SmmuOn = TRUE;
    TlbFlush();
    LogPrint("  SMMU: TTBR0 %08x%08x SCTLR %08x TCR %08x TCR2 %08x CBA2R %08x CBAR %08x MAIR0 %08x IDR2 %08x\n",
             Rd(g_Smmu, SMMU_CB0 + 0x24), Rd(g_Smmu, SMMU_CB0 + 0x20), Rd(g_Smmu, SMMU_CB0), Rd(g_Smmu, SMMU_CB0 + 0x30),
             Rd(g_Smmu, SMMU_CB0 + 0x10), Rd(g_Smmu, 0x1800), Rd(g_Smmu, 0x1000), Rd(g_Smmu, SMMU_CB0 + 0x38),
             Rd(g_Smmu, 0x28));
    return TRUE;
}

static VOID SmmuFault(PCSTR Tag)
{
    LogPrint("  [%s] SMMU gfsr %08x CB0 FSR %08x FAR %08x%08x FSYNR0 %08x\n", Tag, Rd(g_Smmu, 0x48),
             Rd(g_Smmu, SMMU_CB0 + 0x58), Rd(g_Smmu, SMMU_CB0 + 0x64), Rd(g_Smmu, SMMU_CB0 + 0x60),
             Rd(g_Smmu, SMMU_CB0 + 0x68));
}

/* ---------------------------------------------------------------- GPU VA allocator */

ULONGLONG VaAlloc(SIZE_T Size)
{
    PLIST_ENTRY e;
    VA_RANGE *r;
    ULONGLONG va;

    Size = ROUND_TO_PAGES(Size);
    for (e = g_VaFree.Flink; e != &g_VaFree; e = e->Flink) {
        r = CONTAINING_RECORD(e, VA_RANGE, Link);
        if (r->Size >= Size) {
            va = r->Start;
            r->Start += Size;
            r->Size -= Size;
            if (r->Size == 0) {
                RemoveEntryList(&r->Link);
                ExFreePoolWithTag(r, TGPU_POOL_TAG);
            }
            return va;
        }
    }
    return 0;
}

VOID VaFree(ULONGLONG Va, SIZE_T Size)
{
    VA_RANGE *r = (VA_RANGE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VA_RANGE), TGPU_POOL_TAG);
    if (r != NULL) {                                    /* no coalescing: fine for now */
        r->Start = Va;
        r->Size = ROUND_TO_PAGES(Size);
        InsertHeadList(&g_VaFree, &r->Link);
    }
}

/* ---------------------------------------------------------------- kernel buffers */

/* GPU-visible pages: identity mode = contiguous WC memory below 4 GB, GPU VA = PA */
PMDL TgAllocPages(SIZE_T Size, PULONGLONG Iova, PVOID *KernelVa)
{
#if TGPU_IDENTITY
    PHYSICAL_ADDRESS lo, hi, bound;
    PVOID va;
    PMDL mdl;

    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;
    bound.QuadPart = 0;
    Size = ROUND_TO_PAGES(Size);
    va = MmAllocateContiguousMemorySpecifyCache(Size, lo, hi, bound, MmWriteCombined);
    if (va == NULL) {
        return NULL;
    }
    mdl = IoAllocateMdl(va, (ULONG)Size, FALSE, FALSE, NULL);
    if (mdl == NULL) {
        MmFreeContiguousMemorySpecifyCache(va, Size, MmWriteCombined);
        return NULL;
    }
    MmBuildMdlForNonPagedPool(mdl);
    RtlZeroMemory(va, Size);
    *KernelVa = va;
    *Iova = (ULONGLONG)MmGetPhysicalAddress(va).QuadPart;
    return mdl;
#else
    PHYSICAL_ADDRESS lo, hi, skip;
    PMDL mdl;

    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;
    skip.QuadPart = 0;
    Size = ROUND_TO_PAGES(Size);
    mdl = MmAllocatePagesForMdlEx(lo, hi, skip, Size, MmWriteCombined, MM_ALLOCATE_FULLY_REQUIRED);
    if (mdl == NULL) {
        return NULL;
    }
    *KernelVa = NULL;
    *Iova = VaAlloc(Size + PAGE_SIZE);
    if (*Iova == 0 || !NT_SUCCESS(MmuMap(*Iova, mdl, Size))) {
        MmFreePagesFromMdl(mdl);
        ExFreePool(mdl);
        return NULL;
    }
    return mdl;
#endif
}

VOID TgFreePages(PMDL Mdl, PVOID KernelVa, SIZE_T Size, ULONGLONG Iova)
{
#if TGPU_IDENTITY
    UNREFERENCED_PARAMETER(Iova);
    IoFreeMdl(Mdl);
    MmFreeContiguousMemorySpecifyCache(KernelVa, ROUND_TO_PAGES(Size), MmWriteCombined);
#else
    UNREFERENCED_PARAMETER(KernelVa);
    MmuUnmap(Iova, Size);
    VaFree(Iova, Size + PAGE_SIZE);
    MmFreePagesFromMdl(Mdl);
    ExFreePool(Mdl);
#endif
}

static BOOLEAN KBufAlloc(KBUF *B, SIZE_T Size)
{
#if TGPU_IDENTITY
    B->Size = ROUND_TO_PAGES(Size);
    B->Mdl = TgAllocPages(B->Size, &B->Iova, &B->Va);
    return B->Mdl != NULL;
#else
    PHYSICAL_ADDRESS lo, hi, skip;

    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;                          /* v0.14: walks/fetches above 4 GB faulted */
    skip.QuadPart = 0;
    B->Size = ROUND_TO_PAGES(Size);
    B->Mdl = MmAllocatePagesForMdlEx(lo, hi, skip, B->Size, MmWriteCombined, MM_ALLOCATE_FULLY_REQUIRED);
    if (B->Mdl == NULL) {
        return FALSE;
    }
    B->Va = MmMapLockedPagesSpecifyCache(B->Mdl, KernelMode, MmWriteCombined, NULL, FALSE, NormalPagePriority);
    if (B->Va == NULL) {
        return FALSE;
    }
    RtlZeroMemory(B->Va, B->Size);
    B->Iova = g_KernelVaNext;
    g_KernelVaNext += B->Size + PAGE_SIZE;               /* guard page */
    return NT_SUCCESS(MmuMap(B->Iova, B->Mdl, B->Size));
#endif
}

static VOID KBufFree(KBUF *B)
{
#if TGPU_IDENTITY
    if (B->Mdl != NULL) {
        TgFreePages(B->Mdl, B->Va, B->Size, B->Iova);
    }
    RtlZeroMemory(B, sizeof(*B));
    return;
#endif
    if (B->Va != NULL) {
        MmUnmapLockedPages(B->Va, B->Mdl);
    }
    if (B->Mdl != NULL) {
        MmFreePagesFromMdl(B->Mdl);
        ExFreePool(B->Mdl);
    }
    RtlZeroMemory(B, sizeof(*B));
}

/* ---------------------------------------------------------------- CP */

static ULONG Par(ULONG V)
{
    return (0x9669 >> (0xF & (V ^ (V >> 4) ^ (V >> 8) ^ (V >> 12) ^ (V >> 16) ^ (V >> 20) ^ (V >> 24) ^ (V >> 28)))) & 1;
}

static ULONG Pkt7(ULONG Op, ULONG Cnt)
{
    return 0x70000000u | Cnt | (Par(Cnt) << 15) | ((Op & 0x7F) << 16) | (Par(Op) << 23);
}

static VOID Emit(ULONG V)
{
    ((PULONG)g_Ring.Va)[g_Wptr % RB_DWORDS] = V;
    g_Wptr++;
}

static VOID Kick(VOID)
{
    __dsb(_ARM64_BARRIER_SY);
    GpuWr(0x807, g_Wptr % RB_DWORDS);                    /* CP_RB_WPTR */
}

static ULONG RingFree(VOID)
{
    ULONG rptr = GpuRd(0x806), wptr = g_Wptr % RB_DWORDS;
    return (rptr + RB_DWORDS - wptr - 1) % RB_DWORDS;
}

static BOOLEAN RingIdle(ULONG Ms)
{
    ULONG i;
    for (i = 0; i < Ms * 100; i++) {
        if (GpuRd(0x806) == g_Wptr % RB_DWORDS) {
            return TRUE;
        }
        KeStallExecutionProcessor(10);
    }
    LogPrint("  ring stuck: rptr %u wptr %u RBBM_STATUS %08x CP_HW_FAULT %08x\n", GpuRd(0x806), g_Wptr % RB_DWORDS,
             GpuRd(0x210), GpuRd(0x821));
    SmmuFault("ring");
    {
        ULONGLONG va = g_Sqe.Iova;
        ULONG i1 = (ULONG)(va >> 30);
        LogPrint("  walk %llx: L1[%u] %llx", va, i1, g_L1[i1]);
        if (g_L2[i1] != NULL) {
            LogPrint(" L2[%u] %llx", (ULONG)((va >> 21) & 511), g_L2[i1][(va >> 21) & 511]);
        }
        if (g_L3[va >> 21] != NULL) {
            LogPrint(" L3[%u] %llx", (ULONG)((va >> 12) & 511), g_L3[va >> 21][(va >> 12) & 511]);
        }
        LogPrint("\n");
    }
    return FALSE;
}

/* v0.20: hardware clock gating as Linux a6xx_set_hwcg(gpu, true) does for a610. Step B hung in the
   shader pipeline (SP/HLSQ busy) with the reset-default CGC settings. */
static const ULONG g_A612Hwcg[][2] = {        /* Linux a6xx_catalog.c a612_hwcg (a610 uses it) */
    { 0x000B0, 0x22222222 },     /* RBBM_CLOCK_CNTL_SP0 */
    { 0x000B4, 0x02222220 },     /* RBBM_CLOCK_CNTL2_SP0 */
    { 0x000B8, 0x00000081 },     /* RBBM_CLOCK_DELAY_SP0 */
    { 0x000BC, 0x0000f3cf },     /* RBBM_CLOCK_HYST_SP0 */
    { 0x000C0, 0x22222222 },     /* RBBM_CLOCK_CNTL_TP0 */
    { 0x000C4, 0x22222222 },     /* RBBM_CLOCK_CNTL2_TP0 */
    { 0x000C8, 0x22222222 },     /* RBBM_CLOCK_CNTL3_TP0 */
    { 0x000CC, 0x00022222 },     /* RBBM_CLOCK_CNTL4_TP0 */
    { 0x000D0, 0x11111111 },     /* RBBM_CLOCK_DELAY_TP0 */
    { 0x000D4, 0x11111111 },     /* RBBM_CLOCK_DELAY2_TP0 */
    { 0x000D8, 0x11111111 },     /* RBBM_CLOCK_DELAY3_TP0 */
    { 0x000DC, 0x00011111 },     /* RBBM_CLOCK_DELAY4_TP0 */
    { 0x000E0, 0x77777777 },     /* RBBM_CLOCK_HYST_TP0 */
    { 0x000E4, 0x77777777 },     /* RBBM_CLOCK_HYST2_TP0 */
    { 0x000E8, 0x77777777 },     /* RBBM_CLOCK_HYST3_TP0 */
    { 0x000EC, 0x00077777 },     /* RBBM_CLOCK_HYST4_TP0 */
    { 0x000F0, 0x22222222 },     /* RBBM_CLOCK_CNTL_RB0 */
    { 0x000F4, 0x01202222 },     /* RBBM_CLOCK_CNTL2_RB0 */
    { 0x000F8, 0x00002220 },     /* RBBM_CLOCK_CNTL_CCU0 */
    { 0x00100, 0x00040f00 },     /* RBBM_CLOCK_HYST_RB_CCU0 */
    { 0x00104, 0x05522022 },     /* RBBM_CLOCK_CNTL_RAC */
    { 0x00105, 0x00005555 },     /* RBBM_CLOCK_CNTL2_RAC */
    { 0x00106, 0x00000011 },     /* RBBM_CLOCK_DELAY_RAC */
    { 0x00107, 0x00445044 },     /* RBBM_CLOCK_HYST_RAC */
    { 0x00108, 0x04222222 },     /* RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
    { 0x00111, 0x00002222 },     /* RBBM_CLOCK_MODE_VFD */
    { 0x00114, 0x02222222 },     /* RBBM_CLOCK_MODE_GPC */
    { 0x00117, 0x00000002 },     /* RBBM_CLOCK_DELAY_HLSQ_2 */
    { 0x0011B, 0x00002222 },     /* RBBM_CLOCK_MODE_HLSQ */
    { 0x00109, 0x00004000 },     /* RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
    { 0x00112, 0x00002222 },     /* RBBM_CLOCK_DELAY_VFD */
    { 0x00115, 0x00000200 },     /* RBBM_CLOCK_DELAY_GPC */
    { 0x0011C, 0x00000000 },     /* RBBM_CLOCK_DELAY_HLSQ */
    { 0x0010A, 0x00000000 },     /* RBBM_CLOCK_HYST_TSE_RAS_RBBM */
    { 0x00113, 0x00000000 },     /* RBBM_CLOCK_HYST_VFD */
    { 0x00116, 0x04104004 },     /* RBBM_CLOCK_HYST_GPC */
    { 0x0011D, 0x00000000 },     /* RBBM_CLOCK_HYST_HLSQ */
    { 0x0010B, 0x22222222 },     /* RBBM_CLOCK_CNTL_UCHE */
    { 0x00110, 0x00000004 },     /* RBBM_CLOCK_HYST_UCHE */
    { 0x0010F, 0x00000002 },     /* RBBM_CLOCK_DELAY_UCHE */
    { 0x0533, 0x00000182 },      /* RBBM_ISDB_CNT */
    { 0x00044, 0x00000000 },     /* RBBM_RAC_THRESHOLD_CNT */
    { 0x00042, 0x00000000 },     /* RBBM_SP_HYST_CNT */
    { 0x00118, 0x00000222 },     /* RBBM_CLOCK_CNTL_GMU_GX */
    { 0x00119, 0x00000111 },     /* RBBM_CLOCK_DELAY_GMU_GX */
    { 0x0011A, 0x00000555 },     /* RBBM_CLOCK_HYST_GMU_GX */
};

static VOID HwInitRegs(VOID)
{
    ULONG i;
    static const ULONG addrMode[] = { 0x842, 0xC01, 0x8601, 0x8e05, 0x9e01, 0xbe05, 0xa601, 0x9601, 0xE00,
                                      0xae01, 0xb601, 0xF810 };

    GpuWr(0x3c45, 0);
    GpuRd(0x3c45);
    GpuWr(0x16, 0);
    GpuRd(0x16);
    GpuWr(0xF803, 0);
    GpuWr64(0xF800, 0);
    GpuWr(0xF802, 0);
    for (i = 0; i < ARRAYSIZE(addrMode); i++) {
        GpuWr(addrMode[i], 1);
    }
    LogPrint("  RBBM_CLOCK_CNTL was %08x\n", GpuRd(0xAE));
    for (i = 0; i < ARRAYSIZE(g_A612Hwcg); i++) {
        GpuWr(g_A612Hwcg[i][0], g_A612Hwcg[i][1]);
    }
    GpuWr(0xAE, 0xaaa8aa82);                             /* RBBM_CLOCK_CNTL: a610 clock_cntl_on */
    for (i = 0; i < 4; i++) {
        GpuWr(0x3c03 + i, 0x00071620);
    }
    GpuWr(0x11, 3);
    GpuWr64(0xE05, 0x1fffffffff000ull + 0xfc0);
    GpuWr64(0xE09, 0x1fffffffff000ull);
    GpuWr64(0xE07, 0x1fffffffff000ull);
    GpuWr64(0xE0B, 0x100000);
    GpuWr64(0xE0D, 0x100000 + g_GmemSize - 1);
    GpuWr(0xE18, 0x804);
    GpuWr(0xE17, 4);
    GpuWr(0x8c2, 0x00800060);
    GpuWr(0x8c1, 0x40201b16);
    GpuWr(0x8C3, 48);
    GpuWr(0x9e00, 0x00080000);
    GpuWr(0x98d, 1);
    GpuWr(0x1f, (1u << 30) | 0x3ffff);
    GpuWr(0xe19, 0x81);
    GpuWr(0x500, 1);                                     /* RBBM_PERFCTR_CNTL */
    /* v0.17: rest of Linux a6xx hw_init for a610 (freedreno renders assuming it) */
    GpuWr(0x90E, 47);                                    /* CP_MEM_POOL_DBG_ADDR */
    GpuWr(0x50B, 0xffffffff);                            /* RBBM_PERFCTR_GPU_BUSY_MASKED */
    GpuWr(0x8D0, 0);                                     /* CP_PERFCTR_CP_SEL0 = ALWAYS_COUNT */
    /* UBWC (a6xx_calc_ubwc_config a610: highest bank bit 13, min_acc_len 1, swizzle 7) */
    GpuWr(0x8e08, 0x9);                                  /* RB_NC_MODE_CNTL */
    GpuWr(0xb604, 0x9);                                  /* TPL1_NC_MODE_CNTL */
    GpuWr(0xae02, 0x9);                                  /* SP_NC_MODE_CNTL */
    GpuWr(0xE01, 1u << 23);                              /* UCHE_MODE_CNTL */
    GpuWr(0x534, 0);                                     /* RBBM_NC_MODE_CNTL */
}

/* v0.24 experiment: 3D draws hang with RB/CCU busy; CCU's sysmem cache sits at the end of GMEM as
   Mesa computes it from GMEM_SIZE. C:\topaz\gpu.gmem = hex size to test a smaller khaje GMEM. */
static VOID LoadGmemOverride(VOID)
{
    ULONG size = 0, v = 0, i;
    PUCHAR f = ReadWholeFile(L"\\??\\C:\\topaz\\gpu.gmem", &size);

    if (f == NULL) {
        return;
    }
    for (i = 0; i < size; i++) {
        UCHAR c = f[i];
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else if (c == 'x' || c == 'X') v = 0;
    }
    ExFreePoolWithTag(f, TGPU_POOL_TAG);
    if (v >= 0x4000 && v <= 0x100000) {
        g_GmemSize = v;
    }
    LogPrint("  GMEM size override: %x\n", g_GmemSize);
}

ULONG HwGmemSize(VOID)
{
    return g_GmemSize;
}

static BOOLEAN CpStart(VOID)
{
    PUCHAR fw;
    ULONG fwSize = 0;

    fw = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a630_sqe.fw", &fwSize);
    /* v0.25: buffers survive HwRecover (fence readers may race with it), only the SQE copy is
       reallocated if the firmware file grew */
    if (fw != NULL && g_Sqe.Va != NULL && g_Sqe.Size < fwSize - 4) {
        KBufFree(&g_Sqe);
    }
    if (fw == NULL || fwSize <= 4 || (g_Sqe.Va == NULL && !KBufAlloc(&g_Sqe, fwSize - 4)) ||
        (g_Ring.Va == NULL && !KBufAlloc(&g_Ring, RB_BYTES)) || (g_Mem.Va == NULL && !KBufAlloc(&g_Mem, PAGE_SIZE))) {
        LogPrint("  CP buffers/firmware failed\n");
        if (fw != NULL) {
            ExFreePoolWithTag(fw, TGPU_POOL_TAG);
        }
        return FALSE;
    }
    RtlCopyMemory(g_Sqe.Va, fw + 4, fwSize - 4);
    ExFreePoolWithTag(fw, TGPU_POOL_TAG);
    HwInitRegs();
    GpuWr64(0x830, g_Sqe.Iova);                          /* CP_SQE_INSTR_BASE */
    GpuWr64(0x800, g_Ring.Iova);                         /* CP_RB_BASE */
    GpuWr(0x802, 12 | (2 << 8) | (1u << 27));            /* CP_RB_CNTL */
    g_Wptr = 0;
    GpuWr(0x807, 0);
    __dsb(_ARM64_BARRIER_SY);
    GpuWr(0x808, 1);                                     /* CP_SQE_CNTL */

    Emit(Pkt7(CP_ME_INIT, 8));
    Emit(0x2f);
    Emit(3);
    Emit(0x20000000);
    Emit(0);
    Emit(0);
    Emit(0);
    Emit(0);
    Emit(0);
    Kick();
    if (!RingIdle(100)) {
        return FALSE;
    }
    Emit(Pkt7(CP_SET_SECURE_MODE, 1));
    Emit(0);
    Kick();
    if (!RingIdle(100)) {
        return FALSE;
    }
    LogPrint("  CP up (ME_INIT, SET_SECURE_MODE 0) through the SMMU page tables\n");
    return TRUE;
}

/* ---------------------------------------------------------------- public */

BOOLEAN HwStart(VOID)
{
    BOOLEAN ok = FALSE;

    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    if (g_Ready || g_Failed) {
        ok = g_Ready;
        goto out;
    }
    LogPrint("--- GPU start\n");
    InitializeListHead(&g_VaFree);
    VaFree(TGPU_VA_USER_START, (SIZE_T)(TGPU_VA_KERNEL - TGPU_VA_USER_START));
    g_Gcc = MapIo(GCC_BASE, GCC_SIZE);
    g_GpuCc = MapIo(GPUCC_BASE, GPUCC_SIZE);
    g_Gpu = MapIo(GPU_BASE, GPU_SIZE);
    g_Smmu = MapIo(SMMU_BASE, SMMU_SIZE);
    if (g_Gcc == NULL || g_GpuCc == NULL || g_Gpu == NULL || g_Smmu == NULL) {
        LogPrint("  map failed\n");
        goto fail;
    }
    if (!PowerUp()) {
        goto fail;
    }
    LogPrint("  powered: RBBM_STATUS %08x\n", GpuRd(0x210));
    /* v0.22: Sptprac() NOT called: on khaje reading the GMU wrapper (0x597E340) hangs the bus ->
       bugcheck 0x101 CLOCK_WATCHDOG_TIMEOUT (v0.21). Kept for reference only. */
    UNREFERENCED_PARAMETER(Sptprac);
    LoadGmemOverride();
    if (!ZapLoad() || !SmmuSetup() || !CpStart()) {
        goto fail;
    }
    g_Wedged = FALSE;
    g_Ready = TRUE;
    ok = TRUE;
    goto out;
fail:
    g_Failed = TRUE;                                     /* never retried until reboot */
    LogPrint("--- GPU start FAILED\n");
out:
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok;
}

/* v0.25: bring a wedged GPU back without restarting the WDDM device (a PnP restart waits for DWM to
   release the adapter and holds the PnP lock meanwhile -> explorer/network hung). Called by the next
   submit after a hang. All earlier fences are marked complete: their work is lost. */
BOOLEAN HwRecover(VOID)
{
    BOOLEAN ok = FALSE;

    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    if (!g_Wedged) {
        ok = g_Ready;
        goto out;
    }
    LogPrint("--- GPU recover after hang (seqno %u, completed %u)\n", g_Seqno, HwCompletedFence());
    g_Ready = FALSE;
    if (!PowerUp() || !ZapLoad() || !SmmuSetup() || !CpStart()) {
        g_Failed = TRUE;
        LogPrint("--- GPU recover FAILED\n");
        goto out;
    }
    *(volatile ULONG *)g_Mem.Va = g_Seqno;
    g_Wedged = FALSE;
    g_Ready = TRUE;
    ok = TRUE;
    LogPrint("--- GPU recovered\n");
out:
    KeReleaseMutex(&g_HwLock, FALSE);
    return ok;
}

VOID HwStop(VOID)
{
    if (g_Gpu != NULL && g_Ready) {
        GpuWr(0x808, 0);
    }
    g_Ready = FALSE;
}

BOOLEAN HwReady(VOID)
{
    return g_Ready;
}

VOID HwInit(VOID);
VOID HwInit(VOID)
{
    KeInitializeMutex(&g_HwLock, 0);
}

/* IBs + fence: CP_INDIRECT_BUFFER per cmd, then CACHE_FLUSH_TS writes the seqno to g_Mem+0 */
NTSTATUS HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence)
{
    ULONG i, need = 4 * Count + 5, spin;

    if (g_Wedged) {
        return STATUS_DEVICE_HARDWARE_ERROR;              /* restart the adapter to recover */
    }

    if (!g_Ready) {
        return STATUS_DEVICE_NOT_READY;
    }
    KeWaitForSingleObject(&g_HwLock, Executive, KernelMode, FALSE, NULL);
    for (spin = 0; RingFree() < need && spin < 100000; spin++) {
        KeStallExecutionProcessor(10);
    }
    if (RingFree() < need) {
        KeReleaseMutex(&g_HwLock, FALSE);
        LogPrint("submit: ring full\n");
        return STATUS_DEVICE_BUSY;
    }
    for (i = 0; i < Count; i++) {
        Emit(Pkt7(CP_INDIRECT_BUFFER, 3));
        Emit((ULONG)IbIova[i]);
        Emit((ULONG)(IbIova[i] >> 32));
        Emit(IbDwords[i]);
    }
    *Fence = ++g_Seqno;
    Emit(Pkt7(CP_EVENT_WRITE, 4));
    Emit(CACHE_FLUSH_TS);
    Emit((ULONG)g_Mem.Iova);
    Emit((ULONG)(g_Mem.Iova >> 32));
    Emit(*Fence);
    Kick();
    KeReleaseMutex(&g_HwLock, FALSE);
    return STATUS_SUCCESS;
}

ULONG HwCompletedFence(VOID)
{
    return g_Mem.Va != NULL ? *(volatile ULONG *)g_Mem.Va : 0;
}

BOOLEAN HwWedged(VOID)
{
    return g_Wedged;
}

/* v0.18: where the CP stopped. IBx_BASE is the fetch pointer (the ROQ prefetches ahead), so dump the
   dwords before it too. */
static VOID HangDump(VOID)
{
    ULONGLONG ib1 = GpuRd(0x928) | ((ULONGLONG)GpuRd(0x929) << 32);
    ULONGLONG ib2 = GpuRd(0x92B) | ((ULONGLONG)GpuRd(0x92C) << 32);

    LogPrint("  hang: IB1 %llx rem %u, IB2 %llx rem %u, CP_HW_FAULT %08x CP_INT %08x RBBM_INT0 %08x\n",
             ib1, GpuRd(0x92A), ib2, GpuRd(0x92D), GpuRd(0x821), GpuRd(0x823), GpuRd(0x201));
    LogPrint("  hang: RBBM_STATUS %08x STATUS1 %08x STATUS2 %08x STATUS3 %08x CP_STATUS_1 %08x\n",
             GpuRd(0x210), GpuRd(0x211), GpuRd(0x212), GpuRd(0x213), GpuRd(0x825));
    LogPrint("  hang: CP_SCRATCH0-7 %08x %08x %08x %08x %08x %08x %08x %08x (freedreno markers: 5 emit, 6 pass, 7 draw/blit)\n",
             GpuRd(0x883), GpuRd(0x884), GpuRd(0x885), GpuRd(0x886), GpuRd(0x887), GpuRd(0x888), GpuRd(0x889),
             GpuRd(0x88A));
    MsmDumpIova("IB1", ib1, 64, 16);
    if (GpuRd(0x92D) != 0 || ib2 != 0) {
        MsmDumpIova("IB2", ib2, 64, 16);
    }
}

BOOLEAN HwWaitFence(ULONG Fence, ULONG TimeoutMs)
{
    LARGE_INTEGER d;
    ULONG waited = 0;

    d.QuadPart = -2000;                                  /* 200 us */
    while ((LONG)(HwCompletedFence() - Fence) < 0) {
        if (g_Wedged) {
            return FALSE;                                /* fail fast: no 5 s loop per caller */
        }
        if (waited >= TimeoutMs * 5) {
            LogPrint("wait fence %u: timeout, completed %u, rptr %u wptr %u RBBM_STATUS %08x -> GPU wedged\n",
                     Fence, HwCompletedFence(), GpuRd(0x806), g_Wptr % RB_DWORDS, GpuRd(0x210));
            SmmuFault("wait");
            HangDump();
            g_Wedged = TRUE;
            return FALSE;
        }
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        waited++;
    }
    return TRUE;
}

ULONGLONG HwTimestamp(VOID)
{
    ULONG lo, hi;
    if (!g_Ready) {
        return 0;
    }
    hi = GpuRd(0x981);
    lo = GpuRd(0x980);
    return ((ULONGLONG)hi << 32) | lo;
}
