/*
 * libdrm subset for freedreno on Windows: every DRM ioctl becomes one TopazGpu escape
 * (topazgpu_escape.h) to the WDDM KMD, which implements the Linux msm driver's ioctls with the
 * same structs. "fds" are small integers handed out by fd_wddm_open() and bound to the escape
 * callback of the D3D10 UMD device (gdikmt).
 */
#include <errno.h>
#include <stdio.h>
#include <io.h>
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

/* DWM creates/destroys D3D devices from several threads */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
static SRWLOCK fds_lock = SRWLOCK_INIT;

int
fd_wddm_open(fd_wddm_escape_fn escape, void *ctx)
{
   int ret = -1;
   AcquireSRWLockExclusive(&fds_lock);
   for (int i = 0; i < FD_WDDM_MAX; i++) {
      if (fds[i].refs == 0) {
         fds[i].escape = escape;
         fds[i].ctx = ctx;
         fds[i].refs = 1;
         ret = FD_WDDM_BASE + i;
         break;
      }
   }
   ReleaseSRWLockExclusive(&fds_lock);
   return ret;
}

void
fd_wddm_detach(void *ctx)
{
   AcquireSRWLockExclusive(&fds_lock);
   for (int i = 0; i < FD_WDDM_MAX; i++) {
      if (fds[i].refs > 0 && fds[i].ctx == ctx) {
         fds[i].refs = 0;
         fds[i].escape = NULL;
         fds[i].ctx = NULL;
      }
   }
   ReleaseSRWLockExclusive(&fds_lock);
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

/* TopazGpu: TOPAZ_ESCLOG=1 logs escape counts per DRM command nr once per second (busy-polling hunt) */
extern void TopazWddmLogf(const char *fmt, ...);
static unsigned esc_count[256];
static ULONGLONG esc_t0;
static int esc_log = -1;

static void
esc_stats(unsigned nr)
{
   if (esc_log < 0) {
      const char *e = getenv("TOPAZ_ESCLOG");
      esc_log = e && e[0] == '1';
   }
   if (!esc_log)
      return;
   esc_count[nr & 0xff]++;
   ULONGLONG now = GetTickCount64();
   if (!esc_t0)
      esc_t0 = now;
   if (now - esc_t0 >= 1000) {
      char line[512];
      int n = snprintf(line, sizeof(line), "ESC/s:");
      for (unsigned i = 0; i < 256 && n < (int)sizeof(line) - 16; i++)
         if (esc_count[i])
            n += snprintf(line + n, sizeof(line) - n, " %02x=%u", i, esc_count[i]);
      TopazWddmLogf("%s\n", line);
      memset(esc_count, 0, sizeof(esc_count));
      esc_t0 = now;
   }
}

static int
esc(int fd, unsigned nr, void *data, unsigned size)
{
   struct topazgpu_escape *e;
   esc_stats(nr);
   int i = slot(fd), ret;
   fd_wddm_escape_fn fn;
   void *ctx;

   if (i < 0 || size > TOPAZGPU_ESC_MAX_DATA)
      return -EINVAL;
   AcquireSRWLockShared(&fds_lock);
   fn = fds[i].escape;
   ctx = fds[i].ctx;
   ReleaseSRWLockShared(&fds_lock);
   if (!fn)
      return -EBADF;
   e = calloc(1, sizeof(*e));
   if (!e)
      return -ENOMEM;
   e->magic = TOPAZGPU_ESC_MAGIC;
   e->nr = nr;
   e->size = size;
   if (size)
      memcpy(e->data, data, size);
   ret = fn(ctx, e, sizeof(*e));
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

/* ---- POSIX bits freedreno needs on Windows (see include/) ---- */
#include <time.h>
#include "pthread.h"
#include "poll.h"
#include "sys/sysinfo.h"

int fd_wddm_mutex_lock(pthread_mutex_t *m) { AcquireSRWLockExclusive((PSRWLOCK)m); return 0; }
int fd_wddm_mutex_unlock(pthread_mutex_t *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); return 0; }
int fd_wddm_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
   return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0) ? 0 : -1;
}
int fd_wddm_cond_broadcast(pthread_cond_t *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); return 0; }
int fd_wddm_poll(struct pollfd *fds, unsigned long nfds, int timeout) { (void)fds; (void)nfds; (void)timeout; return -1; }
long fd_wddm_syscall(long n) { (void)n; return (long)GetCurrentThreadId(); }
int fd_wddm_usleep(unsigned usec) { Sleep((usec + 999) / 1000); return 0; }

int fd_wddm_sysinfo(struct sysinfo *si)
{
   MEMORYSTATUSEX ms = { sizeof(ms) };
   GlobalMemoryStatusEx(&ms);
   si->totalram = ms.ullTotalPhys;
   si->freeram = ms.ullAvailPhys;
   si->mem_unit = 1;
   return 0;
}

int fd_wddm_clock_gettime(int clk, struct timespec *ts)
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER now;
   (void)clk;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&now);
   ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
   ts->tv_nsec = (long)((now.QuadPart % freq.QuadPart) * 1000000000ll / freq.QuadPart);
   return 0;
}

#include <stdarg.h>
#include <stdio.h>
#include <io.h>

int fd_wddm_ioctl(int fd, unsigned long request, ...) { (void)fd; (void)request; errno = ENOTTY; return -1; }

int fd_wddm_vasprintf(char **out, const char *fmt, va_list ap)
{
   va_list ap2;
   int n;
   va_copy(ap2, ap);
   n = _vscprintf(fmt, ap2);
   va_end(ap2);
   if (n < 0 || !(*out = malloc((size_t)n + 1)))
      return -1;
   return vsnprintf(*out, (size_t)n + 1, fmt, ap);
}

int fd_wddm_asprintf(char **out, const char *fmt, ...)
{
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = fd_wddm_vasprintf(out, fmt, ap);
   va_end(ap);
   return n;
}

char *fd_wddm_strndup(const char *s, size_t n)
{
   size_t l = strnlen(s, n);
   char *r = malloc(l + 1);
   if (r) {
      memcpy(r, s, l);
      r[l] = 0;
   }
   return r;
}

int fd_wddm_ftruncate(int fd, long long len) { return _chsize_s(fd, len) == 0 ? 0 : -1; }

/* only used for shader disassembly dumps (debug options): not supported */
FILE *fd_wddm_open_memstream(char **ptr, size_t *size) { *ptr = NULL; *size = 0; return NULL; }

long fd_wddm_sysconf(int name)
{
   SYSTEM_INFO si;
   (void)name;
   GetSystemInfo(&si);
   return (long)si.dwNumberOfProcessors;
}
