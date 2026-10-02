/* sysinfo() (total RAM) for freedreno_screen.c on Windows. */
#ifndef _TOPAZGPU_SYS_SYSINFO_H_
#define _TOPAZGPU_SYS_SYSINFO_H_
#ifdef __cplusplus
extern "C" {
#endif
struct sysinfo {
   unsigned long long totalram;
   unsigned long long freeram;
   unsigned int mem_unit;
};
int fd_wddm_sysinfo(struct sysinfo *si);
#define sysinfo(si) fd_wddm_sysinfo(si)
#ifdef __cplusplus
}
#endif
#endif
