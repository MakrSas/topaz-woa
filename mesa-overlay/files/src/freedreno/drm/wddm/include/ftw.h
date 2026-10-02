/* nftw() stub: there is no /proc/device-tree on Windows (freedreno_dt.c finds nothing). */
#ifndef _TOPAZGPU_FTW_H_
#define _TOPAZGPU_FTW_H_
#include <sys/stat.h>
#define FTW_F 0
#define FTW_D 1
#define FTW_DNR 2
#define FTW_NS 3
#define FTW_SL 4
#define FTW_DP 5
#define FTW_SLN 6
#define FTW_PHYS 1
struct FTW { int base; int level; };
static inline int
nftw(const char *path, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fds, int flags)
{
   (void)path; (void)fn; (void)fds; (void)flags;
   return -1;
}
#endif
