/*
 * TopazGpuW - WDDM driver for the Adreno 610 of Redmi Note 12 4G (topaz, SM6225).
 * Stage G2 step A (docs/P8_gpu.md): render-only adapter; the UMD (Mesa freedreno through
 * d3d10umd) talks to it with escapes that carry the Linux msm DRM ioctls (topazgpu_escape.h).
 *
 * hw.c   GPU power, zap shader (TZ PAS 13), SMMU page tables, SQE, ring, submit, fences
 * msm.c  BO table + the msm ioctls
 * ddi.c  DxgkInitialize + the WDDM DDIs
 * disp.c step C2: display side (POST framebuffer, VidPN, EDID, scanout copy, vsync, brightness)
 * eng.c  step C2: the CPU "engine" that executes paging/present DMA buffers in a worker thread
 */
#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include <dispmprt.h>
#include <d3dkmddi.h>

#include "msm_drm_k.h"
#include "topazgpu_escape.h"

#define TGPU_VERSION        "v0.45"
#define TGPU_POOL_TAG       'WupG'

/* ---- log.c ---- */
VOID LogOpen(VOID);
VOID LogClose(VOID);
VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...);
VOID LogHex(_In_z_ PCSTR Prefix, _In_reads_(Len) const UCHAR *Buf, _In_ ULONG Len);

/* ---- hw.c ---- */
#define TGPU_VA_USER_START  0x01000000ULL       /* BOs: [VA_USER_START, VA_KERNEL_START) */
#define TGPU_VA_KERNEL      0xF0000000ULL       /* ring, SQE, fence page */
#define TGPU_VA_END         0x100000000ULL      /* A610: 4 GB GPU VA (ADRENO_QUIRK_4GB_VA) */

typedef struct _TGPU_BO {
    LIST_ENTRY  Link;
    ULONG       Handle;                         /* also its global flink name */
    LONG        Refs;
    SIZE_T      Size;                           /* page multiple */
    ULONG       Flags;                          /* MSM_BO_* */
    PMDL        Mdl;                            /* backing pages */
    PVOID       KernelVa;                       /* write-combined kernel mapping (lazy) */
    ULONGLONG   Iova;                           /* GPU VA */
    ULONG       LastFence;                      /* last submit that used it */
    LIST_ENTRY  Maps;                           /* TGPU_MAP: user mappings */
} TGPU_BO;

typedef struct _TGPU_MAP {
    LIST_ENTRY  Link;
    PEPROCESS   Process;
    PVOID       UserVa;
} TGPU_MAP;

BOOLEAN   HwStart(VOID);
VOID      HwStop(VOID);
BOOLEAN   HwReady(VOID);
/* TGPU_IDENTITY 1: the GPU SMMU context bank is pass-through (as in G1) and every GPU buffer is
   physically contiguous below 4 GB with GPU VA = PA. The LPAE page-table path (0) still faults on
   the first CP fetch (translation fault, FSYNR0 level 0) - to be debugged later. */
#define TGPU_IDENTITY 1
PMDL      TgAllocPages(SIZE_T Size, PULONGLONG Iova, PVOID *KernelVa);
VOID      TgFreePages(PMDL Mdl, PVOID KernelVa, SIZE_T Size, ULONGLONG Iova);
NTSTATUS  MmuMap(ULONGLONG Va, PMDL Mdl, SIZE_T Size);
VOID      MmuUnmap(ULONGLONG Va, SIZE_T Size);
ULONGLONG VaAlloc(SIZE_T Size);
VOID      VaFree(ULONGLONG Va, SIZE_T Size);
NTSTATUS  HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence);
ULONG     HwCompletedFence(VOID);
BOOLEAN   HwWaitFence(ULONG Fence, ULONG TimeoutMs);
BOOLEAN   HwWedged(VOID);
BOOLEAN   HwRecover(VOID);
ULONG     HwGmemSize(VOID);
ULONGLONG HwTimestamp(VOID);

/* ---- msm.c ---- */
VOID     MsmInit(VOID);
VOID     MsmCleanup(VOID);
NTSTATUS MsmEscape(struct topazgpu_escape *Esc, PVOID Owner);
VOID     MsmReleaseOwner(PVOID Owner);           /* DestroyDevice: drop the BO references of a device */
VOID     MsmDumpIova(PCSTR Tag, ULONGLONG Iova, ULONG Before, ULONG After);

TGPU_BO *MsmBoCreate(SIZE_T Size);             /* refs 1, NULL on failure */
TGPU_BO *MsmBoAcquire(ULONG Name);              /* +1 reference, NULL if no such BO */
VOID     MsmBoRelease(TGPU_BO *Bo);

/* ---- allocations (ddi.c) ---- */
typedef struct _TGPU_ALLOCATION {
    volatile LONG Refs;                         /* CreateAllocation 1, + scanout use */
    struct topazgpu_alloc Desc;
    SIZE_T      Size;
    TGPU_BO    *Bo;                             /* UMD allocations: BO with the pixels */
    PMDL        ApMdl;                          /* VidMM backing mapped into the aperture */
    PMDL        ApPartial;                      /* partial MDL when MdlOffset != 0 */
    PVOID       ApVa;                           /* kernel mapping of the VidMM backing */
    SIZE_T      ApBytes;
} TGPU_ALLOCATION;

PUCHAR AllocPixels(TGPU_ALLOCATION *Al);        /* NULL if no CPU view yet */
VOID   AllocRelease(TGPU_ALLOCATION *Al);
VOID   AllocMapAperture(TGPU_ALLOCATION *Al, PMDL Mdl, ULONG MdlOffset, SIZE_T Pages);
VOID   AllocUnmapAperture(TGPU_ALLOCATION *Al);

/* ---- ddi.c: aperture page table ("GART"), v0.30. VidMM maps some allocations into the aperture
   with hAllocation == NULL (CDD's shadow surface), so CPU views come from the aperture address. */
#define TGPU_APERTURE_BASE  0xC0000000ULL
#define TGPU_APERTURE_SIZE  (1024ull * 1024 * 1024)
PVOID GartMapVa(ULONGLONG Addr, SIZE_T Bytes, PMDL *Mdl);    /* NULL if any page is not mapped */
VOID  GartUnmapVa(PVOID Va, PMDL Mdl);

/* ---- eng.c: commands in the DMA buffer private data ---- */
#define TG_CMD_NOP          0
#define TG_CMD_BLT          1                   /* Src rect -> Dst rect (+ sub rects) */
#define TG_CMD_FILL         2                   /* Dst rects with Color */
#define TG_CMD_PG_FILL      3                   /* paging FILL: Dst, Bytes, Color */
#define TG_CMD_PG_MAP       4                   /* MAP_APERTURE_SEGMENT */
#define TG_CMD_PG_UNMAP     5                   /* UNMAP_APERTURE_SEGMENT */
#define TG_CMD_MAX_RECTS    8

typedef struct _TG_CMD {
    ULONG             Op;
    ULONG             NumRects;
    TGPU_ALLOCATION  *Src;
    TGPU_ALLOCATION  *Dst;
    RECT              SrcRect;
    RECT              DstRect;
    RECT              Rects[TG_CMD_MAX_RECTS];  /* destination sub rects; 0 = DstRect */
    ULONG             Color;
    ULONG             MdlOffset;
    PMDL              Mdl;
    SIZE_T            Pages;
    SIZE_T            Bytes;
    ULONG             SrcSeg, DstSeg;           /* v0.30: final placement, written by DxgkDdiPatch */
    ULONGLONG         SrcAddr, DstAddr;
} TG_CMD;

typedef VOID (*TG_FENCE_DONE)(PVOID Ctx, ULONG Fence);
NTSTATUS EngStart(TG_FENCE_DONE Done, PVOID Ctx);
VOID     EngStop(VOID);
NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence);   /* any IRQL <= DISPATCH */
VOID     EngKickScanout(VOID);

/* ---- disp.c ---- */
#define TGPU_FB_PA          0x5C000000ULL      /* UEFI GOP framebuffer, 1080x2400 XRGB8888 */
#define TGPU_FB_WIDTH       1080
#define TGPU_FB_HEIGHT      2400
#define TGPU_FB_PITCH       (TGPU_FB_WIDTH * 4)

NTSTATUS DispStart(PDXGKRNL_INTERFACE Dxgk);
VOID     DispSurveyMdp(VOID);
VOID     DispStop(VOID);
VOID     DispGetPostInfo(DXGK_DISPLAY_INFORMATION *Info);
VOID     DispPresentRects(TGPU_ALLOCATION *Dst, const RECT *Rects, ULONG Count);   /* engine: Dst changed */
VOID     DispScanoutWork(VOID);                                                   /* engine thread */
VOID     DispSystemWrite(PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y);
VOID     DispAllocDestroyed(TGPU_ALLOCATION *Al);
VOID     DispVsyncEnable(BOOLEAN Enable);
NTSTATUS DispSetSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *A);
NTSTATUS DispSetVisibility(const DXGKARG_SETVIDPNSOURCEVISIBILITY *A);
NTSTATUS DispCommitVidPn(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_COMMITVIDPN *A);
NTSTATUS DispEnumCofuncModality(PDXGKRNL_INTERFACE Dxgk, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *A);
NTSTATUS DispRecommendMonitorModes(const DXGKARG_RECOMMENDMONITORMODES *A);
NTSTATUS DispQueryChildRelations(PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size);
NTSTATUS DispQueryChildStatus(PDXGK_CHILD_STATUS St);
NTSTATUS DispQueryDeviceDescriptor(ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc);
NTSTATUS DispGetScanLine(DXGKARG_GETSCANLINE *A);
NTSTATUS DispBrightnessQueryInterface(PQUERY_INTERFACE Qi);
