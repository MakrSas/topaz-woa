/* syscall(SYS_gettid) for freedreno_util.h on Windows. */
#ifndef _TOPAZGPU_SYS_SYSCALL_H_
#define _TOPAZGPU_SYS_SYSCALL_H_
#define SYS_gettid 186
#ifdef __cplusplus
extern "C" {
#endif
long fd_wddm_syscall(long n);
#define syscall(n, ...) fd_wddm_syscall(n)
#ifdef __cplusplus
}
#endif
#endif
