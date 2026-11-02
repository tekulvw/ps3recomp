#ifndef PS3RECOMP_RSX_NATIVE_OWNER_GATE_H
#define PS3RECOMP_RSX_NATIVE_OWNER_GATE_H
/* Temporary host-owner synchronization only. No graphics/pipeline logic.
 * State fields are accessed under lock. Caller supplies per-thread nesting;
 * native leases are never recursive and never wait for an original encoder. */
#include <stdint.h>
#include <limits.h>
#ifdef RSX_NATIVE_OWNER_PTHREAD
#include <pthread.h>
#include <stddef.h>
typedef pthread_mutex_t rsx_owner_lock;
#define RSX_OWNER_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void rsx_owner_lock_acquire(rsx_owner_lock *lock){pthread_mutex_lock(lock);}
static inline int rsx_owner_lock_try(rsx_owner_lock *lock){return pthread_mutex_trylock(lock)==0;}
static inline void rsx_owner_lock_release(rsx_owner_lock *lock){pthread_mutex_unlock(lock);}
#else
#include "../../runtime/platform/win32_compat.h"
typedef SRWLOCK rsx_owner_lock;
#define RSX_OWNER_LOCK_INIT SRWLOCK_INIT
static inline void rsx_owner_lock_acquire(rsx_owner_lock *lock){AcquireSRWLockExclusive(lock);}
static inline int rsx_owner_lock_try(rsx_owner_lock *lock){return TryAcquireSRWLockExclusive(lock);}
static inline void rsx_owner_lock_release(rsx_owner_lock *lock){ReleaseSRWLockExclusive(lock);}
#endif
typedef struct rsx_native_owner_gate {
    rsx_owner_lock lock;
    uint64_t generation;
    unsigned retained;
    int dirty, failed, closing;
} rsx_native_owner_gate;
typedef struct rsx_native_owner_thread {
    rsx_native_owner_gate *gate;
    unsigned original_depth, native;
} rsx_native_owner_thread;
#define RSX_NATIVE_OWNER_GATE_INIT {RSX_OWNER_LOCK_INIT,0,0,1,0,0}
static inline int rsx_owner_original_begin(rsx_native_owner_gate *g,rsx_native_owner_thread *t) {
    if(t->native){if(t->gate==g)g->failed=1;return -6;}
    if(t->original_depth){if(t->gate!=g || t->original_depth==UINT_MAX)return -6;++t->original_depth;return 0;}
    rsx_owner_lock_acquire(&g->lock);t->gate=g;t->original_depth=1;return 0;
}
static inline void rsx_owner_original_end(rsx_native_owner_gate *g,rsx_native_owner_thread *t) {
    if(t->gate!=g || !t->original_depth)return;
    if(!--t->original_depth){t->gate=NULL;rsx_owner_lock_release(&g->lock);}
}
static inline void rsx_owner_dirty(rsx_native_owner_gate *g,rsx_native_owner_thread *t) {
    if(t->gate==g && t->original_depth)g->dirty=1;
}
/* Only call after the real Close/Execute/fence/Reset original boundary. A
 * successful later boundary cannot erase an earlier uncertain/failed one. */
static inline void rsx_owner_submitted(rsx_native_owner_gate *g,rsx_native_owner_thread *t,int success) {
    if(t->gate!=g || !t->original_depth)return;
    if(!success){g->dirty=1;g->failed=1;}else if(!g->failed)g->dirty=0;
}
static inline int rsx_owner_generation(rsx_native_owner_gate *g,rsx_native_owner_thread *t,uint64_t generation) {
    if(t->gate!=g || !t->original_depth)return -6;
    if(g->retained)return -4;
    if(!generation || generation<=g->generation)return -3;
    g->generation=generation;g->dirty=1;g->failed=g->closing=0;return 0;
}
static inline int rsx_owner_retain(rsx_native_owner_gate *g,rsx_native_owner_thread *t,uint64_t generation) {
    if(t->gate || !rsx_owner_lock_try(&g->lock))return -4;
    int status=0;
    if(!generation || generation!=g->generation || g->closing)status=-3;
    else if(g->failed)status=-6;
    else if(g->retained==UINT_MAX)status=-4;
    else ++g->retained;
    rsx_owner_lock_release(&g->lock);return status;
}
static inline int rsx_owner_release(rsx_native_owner_gate *g,rsx_native_owner_thread *t,uint64_t generation) {
    if(t->gate)return -4;
    rsx_owner_lock_acquire(&g->lock);int status=0;
    if(generation!=g->generation || !g->retained)status=-3;else --g->retained;
    rsx_owner_lock_release(&g->lock);return status;
}
static inline int rsx_owner_native_begin(rsx_native_owner_gate *g,rsx_native_owner_thread *t,uint64_t generation) {
    if(t->gate || !rsx_owner_lock_try(&g->lock))return -4;
    int status=0;
    if(!generation || generation!=g->generation || !g->retained || g->closing)status=-3;
    else if(g->failed)status=-6;
    else if(g->dirty)status=-4;
    if(status){rsx_owner_lock_release(&g->lock);return status;}
    t->gate=g;t->native=1;return 0;
}
static inline void rsx_owner_native_end(rsx_native_owner_gate *g,rsx_native_owner_thread *t) {
    if(t->gate!=g || !t->native)return;
    t->gate=NULL;t->native=0;rsx_owner_lock_release(&g->lock);
}
#endif
