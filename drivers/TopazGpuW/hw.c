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

static volatile UCHAR *g_Gcc, *g_GpuCc, *g_Gpu, *g_Smmu;
static BOOLEAN g_Ready, g_Failed;
static FAST_MUTEX g_HwLock;
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

static BOOLEAN PowerUp(VOID)
{
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
    hi.QuadPart = (LONGLONG)-1;
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
    if (g_Ready) {
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
    if (g_Ready) {
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
    Wr(g_Smmu, SMMU_CB0 + 0x030, 25 | (1u << 23));       /* TCR: T0SZ 25 (39-bit), 4 KB, non-cacheable walks, EPD1 */
    Wr(g_Smmu, SMMU_CB0 + 0x010, 2);                     /* TCR2: PASize 40-bit */
    Wr(g_Smmu, SMMU_CB0 + 0x038, 0x44);                  /* MAIR0: attr0 = Normal non-cacheable */
    Wr(g_Smmu, SMMU_CB0 + 0x03C, 0);
    __dsb(_ARM64_BARRIER_SY);
    Wr(g_Smmu, SMMU_CB0 + 0x000, 0x67);                  /* SCTLR: CFIE CFRE AFE TRE M */
    Wr(g_Smmu, 0xC00, 0);                                /* S2CR0 -> CB0 */
    Wr(g_Smmu, 0x800, (1u << 31) | (1u << 16));          /* SMR0: SID 0 mask 1 */
    TlbFlush();
    LogPrint("  SMMU: TTBR0 %llx SCTLR %08x TCR %08x\n", g_L1Pa.QuadPart, Rd(g_Smmu, SMMU_CB0), Rd(g_Smmu, SMMU_CB0 + 0x30));
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

static BOOLEAN KBufAlloc(KBUF *B, SIZE_T Size)
{
    PHYSICAL_ADDRESS lo, hi, skip;

    lo.QuadPart = 0;
    hi.QuadPart = (LONGLONG)-1;
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
}

static VOID KBufFree(KBUF *B)
{
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
    return FALSE;
}

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
    for (i = 0; i < 4; i++) {
        GpuWr(0x3c03 + i, 0x00071620);
    }
    GpuWr(0x11, 3);
    GpuWr64(0xE05, 0x1fffffffff000ull + 0xfc0);
    GpuWr64(0xE09, 0x1fffffffff000ull);
    GpuWr64(0xE07, 0x1fffffffff000ull);
    GpuWr64(0xE0B, 0x100000);
    GpuWr64(0xE0D, 0x100000 + 0x21000 - 1);
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
}

static BOOLEAN CpStart(VOID)
{
    PUCHAR fw;
    ULONG fwSize = 0;

    fw = ReadWholeFile(L"\\??\\C:\\topaz\\fw\\gpu\\a630_sqe.fw", &fwSize);
    if (fw == NULL || fwSize <= 4 || !KBufAlloc(&g_Sqe, fwSize - 4) || !KBufAlloc(&g_Ring, RB_BYTES) ||
        !KBufAlloc(&g_Mem, PAGE_SIZE)) {
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

    ExAcquireFastMutex(&g_HwLock);
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
    if (!ZapLoad() || !SmmuSetup() || !CpStart()) {
        goto fail;
    }
    g_Ready = TRUE;
    ok = TRUE;
    goto out;
fail:
    g_Failed = TRUE;                                     /* never retried until reboot */
    LogPrint("--- GPU start FAILED\n");
out:
    ExReleaseFastMutex(&g_HwLock);
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
    ExInitializeFastMutex(&g_HwLock);
}

/* IBs + fence: CP_INDIRECT_BUFFER per cmd, then CACHE_FLUSH_TS writes the seqno to g_Mem+0 */
NTSTATUS HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence)
{
    ULONG i, need = 4 * Count + 5, spin;

    if (!g_Ready) {
        return STATUS_DEVICE_NOT_READY;
    }
    ExAcquireFastMutex(&g_HwLock);
    for (spin = 0; RingFree() < need && spin < 100000; spin++) {
        KeStallExecutionProcessor(10);
    }
    if (RingFree() < need) {
        ExReleaseFastMutex(&g_HwLock);
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
    ExReleaseFastMutex(&g_HwLock);
    return STATUS_SUCCESS;
}

ULONG HwCompletedFence(VOID)
{
    return g_Mem.Va != NULL ? *(volatile ULONG *)g_Mem.Va : 0;
}

BOOLEAN HwWaitFence(ULONG Fence, ULONG TimeoutMs)
{
    LARGE_INTEGER d;
    ULONG waited = 0;

    d.QuadPart = -2000;                                  /* 200 us */
    while ((LONG)(HwCompletedFence() - Fence) < 0) {
        if (waited >= TimeoutMs * 5) {
            LogPrint("wait fence %u: timeout, completed %u, rptr %u wptr %u RBBM_STATUS %08x\n", Fence,
                     HwCompletedFence(), GpuRd(0x806), g_Wptr % RB_DWORDS, GpuRd(0x210));
            SmmuFault("wait");
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
