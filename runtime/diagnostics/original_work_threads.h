#ifndef PS3_ORIGINAL_WORK_THREADS_H
#define PS3_ORIGINAL_WORK_THREADS_H
#if defined(_WIN32)
#if defined(PS3RECOMP_ORIGINAL_OBSERVER) && PS3RECOMP_ORIGINAL_OBSERVER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#ifdef __cplusplus
extern "C" {
#endif
HANDLE WINAPI ps3_original_CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T,
                                        LPTHREAD_START_ROUTINE, LPVOID, DWORD,
                                        LPDWORD);
uintptr_t __cdecl ps3_original_beginthreadex(void *, unsigned,
                                            unsigned (__stdcall *)(void *),
                                            void *, unsigned, unsigned *);
DECLSPEC_NORETURN void WINAPI ps3_original_ExitThread(DWORD);
DECLSPEC_NORETURN void __cdecl ps3_original_endthreadex(unsigned);
#ifdef __cplusplus
}
#endif
#endif
#else
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
