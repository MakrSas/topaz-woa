/* byte order helpers for freedreno_dt.c on Windows (TopazGpu shim) */
#ifndef _TOPAZGPU_ARPA_INET_H_
#define _TOPAZGPU_ARPA_INET_H_
#include <stdint.h>
#include <stdlib.h>
static inline uint32_t ntohl(uint32_t v) { return _byteswap_ulong(v); }
static inline uint32_t htonl(uint32_t v) { return _byteswap_ulong(v); }
static inline uint16_t ntohs(uint16_t v) { return _byteswap_ushort(v); }
static inline uint16_t htons(uint16_t v) { return _byteswap_ushort(v); }
#endif
