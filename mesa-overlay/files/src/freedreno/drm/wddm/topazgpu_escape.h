/*
 * Escape protocol between the freedreno WDDM shim (Mesa UMD) and the TopazGpu WDDM KMD.
 * One escape = one Linux msm DRM ioctl: the KMD implements the msm driver's ioctls
 * (include/drm-uapi/msm_drm.h) plus the generic GEM ones, with the same structs.
 * Shared by both sides (copy kept in topaz-woa drivers/TopazGpu and mesa-overlay).
 */
#ifndef TOPAZGPU_ESCAPE_H
#define TOPAZGPU_ESCAPE_H

#define TOPAZGPU_ESC_MAGIC      0x55504754u     /* 'TGPU' */
#define TOPAZGPU_ESC_MAX_DATA   1024

/* nr = DRM ioctl number: DRM_COMMAND_BASE (0x40) + DRM_MSM_* or the generic DRM ones below,
   or one of the TopazGpu extras. */
#define TOPAZGPU_NR_GEM_CLOSE   0x09            /* struct drm_gem_close */
#define TOPAZGPU_NR_GEM_FLINK   0x0a            /* struct drm_gem_flink */
#define TOPAZGPU_NR_GEM_OPEN    0x0b            /* struct drm_gem_open */
#define TOPAZGPU_NR_VERSION     0x1000          /* struct topazgpu_version */
#define TOPAZGPU_NR_MMAP        0x1001          /* struct topazgpu_mmap: map a BO into the caller */
#define TOPAZGPU_NR_MUNMAP      0x1002          /* struct topazgpu_mmap */

struct topazgpu_version {
   int major, minor, patch;
   char name[16];                               /* "msm" */
};

struct topazgpu_mmap {
   unsigned long long offset;                   /* MSM_INFO_GET_OFFSET token */
   unsigned long long size;
   unsigned long long addr;                     /* out: user VA (MMAP), in: VA (MUNMAP) */
};

struct topazgpu_escape {
   unsigned magic;
   unsigned nr;
   unsigned size;                               /* bytes of data[] */
   int ret;                                     /* out: 0 or -errno */
   unsigned char data[TOPAZGPU_ESC_MAX_DATA];
};

/* Allocation private driver data (D3DDDI_ALLOCATIONINFO.pPrivateDriverData), step C2: every
   allocation the UMD creates through pfnAllocateCb (swapchain/shared/primary resources) names the
   BO that holds its pixels; standard allocations created by Dxgkrnl (shared primary, shadow,
   staging, GDI) get the same struct from DxgkDdiGetStandardAllocationDriverData with bo = 0 and
   live in VidMM's aperture backing. */
#define TOPAZGPU_ALLOC_MAGIC    0x41475054u     /* 'TPGA' */
#define TOPAZGPU_ALLOC_UMD      0               /* kind: resource of the UMD */
#define TOPAZGPU_ALLOC_SHAREDPRIMARY 1          /* = D3DKMDT_STANDARDALLOCATION_* */
#define TOPAZGPU_ALLOC_SHADOW   2
#define TOPAZGPU_ALLOC_STAGING  3
#define TOPAZGPU_ALLOC_GDI      4
#define TOPAZGPU_ALLOC_F_PRIMARY 0x1

struct topazgpu_alloc {
   unsigned magic;
   unsigned kind;
   unsigned width, height, pitch;               /* pixels / bytes */
   unsigned format;                             /* D3DDDIFORMAT (21 = A8R8G8B8, 22 = X8R8G8B8) */
   unsigned bo;                                 /* BO name with the pixels, 0 = VidMM memory */
   unsigned bo_offset;
   unsigned size;                               /* bytes */
   unsigned vidpn;                              /* primary: VidPnSourceId */
   unsigned flags;                              /* TOPAZGPU_ALLOC_F_* */
   unsigned reserved[5];
};

#endif
