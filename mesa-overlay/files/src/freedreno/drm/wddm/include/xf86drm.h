/* The part of libdrm freedreno uses, implemented over the TopazGpu escape (fd_wddm.c). */
#ifndef _XF86DRM_H_
#define _XF86DRM_H_
#include <stdint.h>
#include <stddef.h>
#include "drm-uapi/drm.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2
typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandNone(int fd, unsigned long drmCommandIndex);
int drmCommandRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmOpenWithType(const char *name, const char *busid, int type);

/* TopazGpu: fake DRM fd bound to an escape callback (the D3D10 UMD's KMD channel). */
typedef int (*fd_wddm_escape_fn)(void *ctx, void *data, unsigned size);
int fd_wddm_open(fd_wddm_escape_fn escape, void *ctx);
/* the D3D device behind ctx is gone: its slots must never escape again */
void fd_wddm_detach(void *ctx);
#ifdef __cplusplus
}
#endif
#endif
