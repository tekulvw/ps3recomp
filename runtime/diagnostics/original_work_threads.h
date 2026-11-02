#ifndef PS3_ORIGINAL_WORK_THREADS_H
#define PS3_ORIGINAL_WORK_THREADS_H
#if !defined(_WIN32)
#include <pthread.h>
#if defined(PS3RECOMP_ORIGINAL_OBSERVER) && PS3RECOMP_ORIGINAL_OBSERVER
#ifdef __cplusplus
extern "C" {
#endif
int ps3_original_pthread_create(pthread_t *, const pthread_attr_t *,
                                void *(*)(void *), void *);
#ifdef __cplusplus
}
#endif
#else
/* Identical original factory call and no allocation/counters in normal builds.
 */
#define ps3_original_pthread_create pthread_create
#endif
#endif
#endif
