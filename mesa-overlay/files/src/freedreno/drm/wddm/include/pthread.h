/* pthread mutex/cond subset for freedreno on Windows (SRW locks via fd_wddm.c). */
#ifndef _TOPAZGPU_PTHREAD_H_
#define _TOPAZGPU_PTHREAD_H_
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { void *p; } pthread_mutex_t;
typedef struct { void *p; } pthread_cond_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
int fd_wddm_mutex_lock(pthread_mutex_t *m);
int fd_wddm_mutex_unlock(pthread_mutex_t *m);
int fd_wddm_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int fd_wddm_cond_broadcast(pthread_cond_t *c);
#define pthread_mutex_lock(m) fd_wddm_mutex_lock(m)
#define pthread_mutex_unlock(m) fd_wddm_mutex_unlock(m)
#define pthread_cond_wait(c, m) fd_wddm_cond_wait(c, m)
#define pthread_cond_broadcast(c) fd_wddm_cond_broadcast(c)
#ifdef __cplusplus
}
#endif
#endif
