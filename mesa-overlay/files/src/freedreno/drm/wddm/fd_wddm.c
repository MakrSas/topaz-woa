/*
 * libdrm subset for freedreno on Windows: every DRM ioctl becomes one TopazGpu escape
 * (topazgpu_escape.h) to the WDDM KMD, which implements the Linux msm driver's ioctls with the
 * same structs. "fds" are small integers handed out by fd_wddm_open() and bound to the escape
 * callback of the D3D10 UMD device (gdikmt).
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "xf86drm.h"
#include "sys/mman.h"
#include "drm-uapi/msm_drm.h"
#include "topazgpu_escape.h"

#define FD_WDDM_BASE   0x7000
#define FD_WDDM_MAX    16

static struct {
   fd_wddm_escape_fn escape;
   void *ctx;
   int refs;
} fds[FD_WDDM_MAX];

int
fd_wddm_open(fd_wddm_escape_fn escape, void *ctx)
{
   for (int i = 0; i < FD_WDDM_MAX; i++) {
      if (fds[i].refs == 0) {
         fds[i].escape = escape;
         fds[i].ctx = ctx;
         fds[i].refs = 1;
         return FD_WDDM_BASE + i;
      }
   }
   return -1;
}

static int
slot(int fd)
{
   int i = fd - FD_WDDM_BASE;
   return (i >= 0 && i < FD_WDDM_MAX && fds[i].refs > 0) ? i : -1;
}

int
fd_wddm_close(int fd)
{
   int i = slot(fd);
   if (i < 0)
      return _close(fd);
   if (--fds[i].refs == 0)
      fds[i].escape = NULL;
   return 0;
}

static int
esc(int fd, unsigned nr, void *data, unsigned size)
{
   struct topazgpu_escape *e;
   int i = slot(fd), ret;

   if (i < 0 || size > TOPAZGPU_ESC_MAX_DATA)
      return -EINVAL;
   e = calloc(1, sizeof(*e));
   if (!e)
      return -ENOMEM;
   e->magic = TOPAZGPU_ESC_MAGIC;
   e->nr = nr;
   e->size = size;
   if (size)
      memcpy(e->data, data, size);
   ret = fds[i].escape(fds[i].ctx, e, sizeof(*e));
   if (ret == 0) {
      ret = e->ret;
      if (size)
         memcpy(data, e->data, size);
   } else {
      ret = -EIO;
   }
   free(e);
   return ret;
}

/* libdrm convention: 0 or -1 with errno for drmIoctl, -errno for drmCommand* */
int
drmIoctl(int fd, unsigned long request, void *arg)
{
   int ret = esc(fd, request & 0xff, arg, IOCPARM_LEN(request));
   if (ret < 0) {
      errno = -ret;
      return -1;
   }
   return 0;
}

int
drmCommandNone(int fd, unsigned long idx)
{
   return esc(fd, DRM_COMMAND_BASE + idx, NULL, 0);
}

int
drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

int
drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

int
drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{
   return esc(fd, DRM_COMMAND_BASE + idx, data, size);
}

drmVersionPtr
drmGetVersion(int fd)
{
   struct topazgpu_version v = {0};
   drmVersionPtr r;

   if (esc(fd, TOPAZGPU_NR_VERSION, &v, sizeof(v)) != 0)
      return NULL;
   r = calloc(1, sizeof(*r) + sizeof(v.name));
   if (!r)
      return NULL;
   r->version_major = v.major;
   r->version_minor = v.minor;
   r->version_patchlevel = v.patch;
   r->name = (char *)(r + 1);
   memcpy(r->name, v.name, sizeof(v.name) - 1);
   r->name_len = (int)strlen(r->name);
   r->date = r->desc = r->name + r->name_len;
   return r;
}

void
drmFreeVersion(drmVersionPtr v)
{
   free(v);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   (void)capability;
   *value = 0;
   return -EINVAL;
}

int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *p) { (void)fd; (void)h; (void)f; (void)p; return -ENOSYS; }
int drmPrimeFDToHandle(int fd, int p, uint32_t *h) { (void)fd; (void)p; (void)h; return -ENOSYS; }
int drmSyncobjDestroy(int fd, uint32_t h) { (void)fd; (void)h; return -ENOSYS; }
int drmSyncobjFDToHandle(int fd, int o, uint32_t *h) { (void)fd; (void)o; (void)h; return -ENOSYS; }
int drmSyncobjSignal(int fd, const uint32_t *h, uint32_t n) { (void)fd; (void)h; (void)n; return -ENOSYS; }
int drmOpenWithType(const char *n, const char *b, int t) { (void)n; (void)b; (void)t; return -1; }

/* mmap(fd, offset) of a BO: offset is the MSM_INFO_GET_OFFSET token from the KMD */
void *
mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
   struct topazgpu_mmap m = { (unsigned long long)offset, length, 0 };
   (void)addr;
   (void)prot;
   (void)flags;
   if (esc(fd, TOPAZGPU_NR_MMAP, &m, sizeof(m)) != 0 || m.addr == 0)
      return MAP_FAILED;
   return (void *)(uintptr_t)m.addr;
}

int
munmap(void *addr, size_t length)
{
   struct topazgpu_mmap m = { 0, length, (unsigned long long)(uintptr_t)addr };
   /* the mapping belongs to whichever device made it: try every open one */
   for (int i = 0; i < FD_WDDM_MAX; i++) {
      if (fds[i].refs > 0 && esc(FD_WDDM_BASE + i, TOPAZGPU_NR_MUNMAP, &m, sizeof(m)) == 0)
         return 0;
   }
   return -1;
}
