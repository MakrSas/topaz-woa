/* poll() stub: fence fds do not exist on the WDDM shim (sync_wait never has real fds). */
#ifndef _TOPAZGPU_POLL_H_
#define _TOPAZGPU_POLL_H_
#ifndef POLLIN
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#endif
#ifdef __cplusplus
extern "C" {
#endif
#ifndef _WINSOCK2API_
struct pollfd { int fd; short events; short revents; };
#endif
int fd_wddm_poll(struct pollfd *fds, unsigned long nfds, int timeout);
#define poll(f, n, t) fd_wddm_poll(f, n, t)
#ifdef __cplusplus
}
#endif
#endif
