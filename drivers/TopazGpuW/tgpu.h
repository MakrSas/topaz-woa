/*
 * TopazGpuW - WDDM driver for the Adreno 610 of Redmi Note 12 4G (topaz, SM6225).
 * Stage G2 step A (docs/P8_gpu.md): render-only adapter; the UMD (Mesa freedreno through
 * d3d10umd) talks to it with escapes that carry the Linux msm DRM ioctls (topazgpu_escape.h).
 *
 * hw.c   GPU power, zap shader (TZ PAS 13), SMMU page tables, SQE, ring, submit, fences
 * msm.c  BO table + the msm ioctls
 * ddi.c  DxgkInitialize + the WDDM DDIs
 */
#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include <dispmprt.h>
#include <d3dkmddi.h>

#include "msm_drm_k.h"
#include "topazgpu_escape.h"

#define TGPU_VERSION        "v0.5"
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
NTSTATUS  MmuMap(ULONGLONG Va, PMDL Mdl, SIZE_T Size);
VOID      MmuUnmap(ULONGLONG Va, SIZE_T Size);
ULONGLONG VaAlloc(SIZE_T Size);
VOID      VaFree(ULONGLONG Va, SIZE_T Size);
NTSTATUS  HwSubmit(const ULONGLONG *IbIova, const ULONG *IbDwords, ULONG Count, PULONG Fence);
ULONG     HwCompletedFence(VOID);
BOOLEAN   HwWaitFence(ULONG Fence, ULONG TimeoutMs);
ULONGLONG HwTimestamp(VOID);

/* ---- msm.c ---- */
VOID     MsmInit(VOID);
VOID     MsmCleanup(VOID);
NTSTATUS MsmEscape(struct topazgpu_escape *Esc);
