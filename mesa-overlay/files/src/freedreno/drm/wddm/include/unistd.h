/* Minimal unistd.h for freedreno on Windows: close() goes to the TopazGpu shim (fake DRM fds). */
#ifndef _TOPAZGPU_UNISTD_H_
#define _TOPAZGPU_UNISTD_H_
#include <io.h>
#include <process.h>
#ifdef __cplusplus
extern "C" {
#endif
int fd_wddm_close(int fd);
int fd_wddm_usleep(unsigned usec);
#ifdef __cplusplus
}
#endif
#define close(fd) fd_wddm_close(fd)
#define usleep(us) fd_wddm_usleep(us)
#ifndef getpid
#define getpid() _getpid()
#endif
#endif
