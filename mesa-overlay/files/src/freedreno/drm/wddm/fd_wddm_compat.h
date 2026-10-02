/* Force-included into every Mesa file when freedreno is built for Windows (TopazGpu). */
#ifndef FD_WDDM_COMPAT_H
#define FD_WDDM_COMPAT_H
#include <time.h>
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
