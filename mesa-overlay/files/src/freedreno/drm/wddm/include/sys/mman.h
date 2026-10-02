/* mmap() over the TopazGpu escape (BO mapping into the calling process). */
#ifndef _SYS_MMAN_H_
#define _SYS_MMAN_H_
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void *)-1)
#ifdef __cplusplus
extern "C" {
#endif
void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t length);
#ifdef __cplusplus
}
#endif
#endif
