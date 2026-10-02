/* Force-included into every Mesa file when freedreno is built for Windows (TopazGpu). */
#ifndef FD_WDDM_COMPAT_H
#define FD_WDDM_COMPAT_H
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int fd_wddm_asprintf(char **out, const char *fmt, ...);
int fd_wddm_vasprintf(char **out, const char *fmt, va_list ap);
char *fd_wddm_strndup(const char *s, size_t n);
int fd_wddm_ftruncate(int fd, long long len);
FILE *fd_wddm_open_memstream(char **ptr, size_t *size);
#ifdef __cplusplus
}
#endif
#define asprintf fd_wddm_asprintf
#define vasprintf fd_wddm_vasprintf
#define strndup fd_wddm_strndup
#define ftruncate fd_wddm_ftruncate
#define open_memstream fd_wddm_open_memstream
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
#ifdef __cplusplus
extern "C" {
#endif
int fd_wddm_clock_gettime(int clk, struct timespec *ts);
#ifdef __cplusplus
}
#endif
#define clock_gettime(c, t) fd_wddm_clock_gettime(c, t)
#endif
#endif
