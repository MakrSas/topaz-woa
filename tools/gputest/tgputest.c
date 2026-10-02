/*
 * tgputest - step A check of TopazGpuW (docs/P8_gpu.md) without any UMD: find the adapter by
 * its UMDRIVERPRIVATE magic, then drive it with the msm-ioctl escapes exactly like the freedreno
 * shim would: VERSION, GET_PARAM, two GEM_NEW + MMAP, an IB with CP_MEM_WRITE into the second
 * BO, GEM_SUBMIT, WAIT_FENCE, read back. Prints everything; exit code 0 = the GPU wrote 0xC0FFEE01.
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>
#include <d3dkmthk.h>

#include "../../drivers/TopazGpuW/topazgpu_escape.h"

typedef unsigned int __u32;
typedef int __s32;
typedef unsigned long long __u64;
typedef long long __s64;
#define DRM_COMMAND_BASE 0x40
#define DRM_MSM_GET_PARAM 0x00
#define DRM_MSM_GEM_NEW 0x02
#define DRM_MSM_GEM_INFO 0x03
#define DRM_MSM_GEM_SUBMIT 0x06
#define DRM_MSM_WAIT_FENCE 0x07
#define MSM_INFO_GET_OFFSET 0
#define MSM_INFO_GET_IOVA 1
struct drm_msm_param { __u32 pipe, param; __u64 value; __u32 len, pad; };
struct drm_msm_gem_new { __u64 size; __u32 flags, handle; };
struct drm_msm_gem_info { __u32 handle, info; __u64 value; __u32 len, pad; };
struct drm_msm_gem_submit_bo { __u32 flags, handle; __u64 presumed; };
struct drm_msm_gem_submit_cmd { __u32 type, submit_idx, submit_offset, size, pad, nr_relocs; __u64 relocs; };
struct drm_msm_gem_submit {
   __u32 flags, fence, nr_bos, nr_cmds; __u64 bos, cmds; __s32 fence_fd; __u32 queueid;
   __u64 in_syncobjs, out_syncobjs; __u32 nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct drm_msm_timespec { __s64 tv_sec, tv_nsec; };
struct drm_msm_wait_fence { __u32 fence, flags; struct drm_msm_timespec timeout; __u32 queueid; };

static D3DKMT_HANDLE g_Adapter;

static int Esc(unsigned nr, void *data, unsigned size)
{
   static struct topazgpu_escape e;
   D3DKMT_ESCAPE k = {0};
   NTSTATUS st;

   memset(&e, 0, sizeof(e));
   e.magic = TOPAZGPU_ESC_MAGIC;
   e.nr = nr;
   e.size = size;
   memcpy(e.data, data, size);
   k.hAdapter = g_Adapter;
   k.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   k.pPrivateDriverData = &e;
   k.PrivateDriverDataSize = sizeof(e);
   st = D3DKMTEscape(&k);
   if (st != 0) {
      printf("  escape %x: NTSTATUS %08lx\n", nr, (unsigned long)st);
      return -1000;
   }
   memcpy(data, e.data, size);
   return e.ret;
}

static unsigned Par(unsigned v)
{
   return (0x9669 >> (0xF & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
}

static unsigned Pkt7(unsigned op, unsigned cnt)
{
   return 0x70000000u | cnt | (Par(cnt) << 15) | ((op & 0x7f) << 16) | (Par(op) << 23);
}

static int NewBo(unsigned size, unsigned *handle, unsigned long long *iova, volatile unsigned **map)
{
   struct drm_msm_gem_new n = { size, 0x20000 /* MSM_BO_WC */, 0 };
   struct drm_msm_gem_info i = {0};
   struct topazgpu_mmap m = {0};
   int r = Esc(DRM_COMMAND_BASE + DRM_MSM_GEM_NEW, &n, sizeof(n));
   if (r) return r;
   *handle = n.handle;
   i.handle = n.handle; i.info = MSM_INFO_GET_IOVA;
   if ((r = Esc(DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &i, sizeof(i)))) return r;
   *iova = i.value;
   i.info = MSM_INFO_GET_OFFSET;
   if ((r = Esc(DRM_COMMAND_BASE + DRM_MSM_GEM_INFO, &i, sizeof(i)))) return r;
   m.offset = i.value; m.size = size;
   if ((r = Esc(TOPAZGPU_NR_MMAP, &m, sizeof(m)))) return r;
   *map = (volatile unsigned *)(uintptr_t)m.addr;
   printf("  BO %u: iova %llx, mapped at %p\n", *handle, *iova, (void *)*map);
   return 0;
}

/* C1 (docs/P8_gpu.md step C): does Dxgkrnl's scheduler drive our adapter? Plain DMA buffers through
   D3DKMTRender; the KMD log must show SubmitCommand and the fences must complete (otherwise Render
   blocks once the DMA buffer pool runs out). */
static int RenderTest(void)
{
   D3DKMT_CREATEDEVICE cd = {0};
   D3DKMT_CREATECONTEXT cc = {0};
   NTSTATUS st;
   void *cmd;
   unsigned i;

   cd.hAdapter = g_Adapter;
   st = D3DKMTCreateDevice(&cd);
   printf("CreateDevice: %08lx\n", (unsigned long)st);
   if (st)
      return 10;
   cc.hDevice = cd.hDevice;
   cc.NodeOrdinal = 0;
   cc.EngineAffinity = 1;
   cc.ClientHint = D3DKMT_CLIENTHINT_DX10;
   st = D3DKMTCreateContext(&cc);
   printf("CreateContext: %08lx cmdbuf %p size %u alloc %u patch %u\n", (unsigned long)st, cc.pCommandBuffer,
          cc.CommandBufferSize, cc.AllocationListSize, cc.PatchLocationListSize);
   if (st)
      return 11;
   cmd = cc.pCommandBuffer;
   for (i = 0; i < 20; i++) {
      D3DKMT_RENDER r = {0};
      LARGE_INTEGER t0, t1, f;
      memset(cmd, 0, 16);
      r.hContext = cc.hContext;
      r.CommandOffset = 0;
      r.CommandLength = 16;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&t0);
      st = D3DKMTRender(&r);
      QueryPerformanceCounter(&t1);
      printf("Render %2u: %08lx in %.1f ms, queued %u\n", i, (unsigned long)st,
             (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart, r.QueuedBufferCount);
      if (st)
         return 12;
      if (r.pNewCommandBuffer)
         cmd = r.pNewCommandBuffer;
   }
   Sleep(3000);
   return 0;
}

int main(int argc, char **argv)
{
   D3DKMT_ENUMADAPTERS2 en = {0};
   D3DKMT_ADAPTERINFO infos[16];
   struct topazgpu_version v = {0};
   static const unsigned params[] = { 1, 2, 3, 4, 5, 6, 14, 15 };
   unsigned i, ib, dst, magic;
   unsigned long long ibIova, dstIova;
   volatile unsigned *ibMap, *dstMap;
   struct drm_msm_gem_submit_bo bos[2];
   struct drm_msm_gem_submit_cmd cmd = {0};
   struct drm_msm_gem_submit sub = {0};
   struct drm_msm_wait_fence wf = {0};
   int r;

   en.NumAdapters = 16;
   en.pAdapters = infos;
   if (D3DKMTEnumAdapters2(&en) != 0) {
      printf("EnumAdapters2 failed\n");
      return 1;
   }
   for (i = 0; i < en.NumAdapters && !g_Adapter; i++) {
      D3DKMT_QUERYADAPTERINFO q = {0};
      ULONG priv = 0;
      q.hAdapter = infos[i].hAdapter;
      q.Type = KMTQAITYPE_UMDRIVERPRIVATE;
      q.pPrivateDriverData = &priv;
      q.PrivateDriverDataSize = sizeof(priv);
      if (D3DKMTQueryAdapterInfo(&q) == 0 && priv == TOPAZGPU_ESC_MAGIC)
         g_Adapter = infos[i].hAdapter;
   }
   printf("adapters: %u, TopazGpuW: %s\n", en.NumAdapters, g_Adapter ? "found" : "NOT FOUND");
   if (!g_Adapter)
      return 2;
   if (argc > 1 && strcmp(argv[1], "render") == 0)
      return RenderTest();

   r = Esc(TOPAZGPU_NR_VERSION, &v, sizeof(v));
   printf("VERSION: ret %d -> %s %d.%d\n", r, v.name, v.major, v.minor);
   if (r)
      return 3;
   for (i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
      struct drm_msm_param p = { 0x10, params[i], 0, 0, 0 };
      r = Esc(DRM_COMMAND_BASE + DRM_MSM_GET_PARAM, &p, sizeof(p));
      printf("GET_PARAM %2u: ret %d value %llx\n", params[i], r, p.value);
   }

   if (NewBo(4096, &ib, &ibIova, &ibMap) || NewBo(4096, &dst, &dstIova, &dstMap))
      return 4;
   dstMap[0] = 0x11111111;
   ibMap[0] = Pkt7(0x3d, 3);                            /* CP_MEM_WRITE */
   ibMap[1] = (unsigned)dstIova;
   ibMap[2] = (unsigned)(dstIova >> 32);
   ibMap[3] = 0xC0FFEE01;
   MemoryBarrier();

   bos[0].flags = 1; bos[0].handle = ib; bos[0].presumed = 0;
   bos[1].flags = 2; bos[1].handle = dst; bos[1].presumed = 0;
   cmd.type = 1; cmd.submit_idx = 0; cmd.submit_offset = 0; cmd.size = 16;
   sub.flags = 0x10; sub.nr_bos = 2; sub.nr_cmds = 1;
   sub.bos = (unsigned long long)(uintptr_t)bos;
   sub.cmds = (unsigned long long)(uintptr_t)&cmd;
   sub.queueid = 1;
   r = Esc(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, &sub, sizeof(sub));
   printf("SUBMIT: ret %d fence %u\n", r, sub.fence);
   if (r)
      return 5;
   wf.fence = sub.fence; wf.queueid = 1;
   r = Esc(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &wf, sizeof(wf));
   magic = dstMap[0];
   printf("WAIT_FENCE: ret %d; dst[0] = %08x %s\n", r, magic,
          magic == 0xC0FFEE01 ? "-> the GPU executed our IB through the SMMU page tables" : "-> FAILED");
   return magic == 0xC0FFEE01 ? 0 : 6;
}
