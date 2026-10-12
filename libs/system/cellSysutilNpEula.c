/*
 * ps3recomp - cellSysutilNpEula HLE implementation
 *
 * EULA check: returns "already accepted" so games skip the EULA dialog.
 */

#include "cellSysutilNpEula.h"
#include <stdio.h>

s32 cellSysutilNpEulaCheck(void* cb, void* arg)
{
    (void)cb; (void)arg;
    printf("[cellSysutilNpEula] EulaCheck() — returning accepted\n");
    /* Return 1 = EULA already accepted */
    return 1;
}

#include "ps3emu/guest_call.h"
#ifdef _WIN32
#include <windows.h>
static SRWLOCK eula_lock = SRWLOCK_INIT;
#define EULA_LOCK() AcquireSRWLockExclusive(&eula_lock)
#define EULA_UNLOCK() ReleaseSRWLockExclusive(&eula_lock)
#else
#include <pthread.h>
static pthread_mutex_t eula_lock = PTHREAD_MUTEX_INITIALIZER;
#define EULA_LOCK() pthread_mutex_lock(&eula_lock)
#define EULA_UNLOCK() pthread_mutex_unlock(&eula_lock)
#endif

#define EULA_INVALID_ARGUMENT ((s32)0x8002e501u)
#define EULA_NOT_INITIALIZED ((s32)0x8002e502u)
#define EULA_ALREADY_INITIALIZED ((s32)0x8002e503u)
static u32 eula_opd, eula_arg, eula_status;
static int eula_delivering;

s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg2, u64 arg3,
    u32 callbackOpd, u32 callbackArg)
{
    (void)arg2; (void)arg3;
    if (!communicationId || !callbackOpd) return EULA_INVALID_ARGUMENT;
    EULA_LOCK();
    if (eula_opd) { EULA_UNLOCK(); return EULA_ALREADY_INITIALIZED; }
    eula_opd = callbackOpd; eula_arg = callbackArg;
    /* Same offline policy as RPCS3: already accepted, delivered asynchronously.
     * CELL_OK is the request result; status 2 is a later callback argument. */
    eula_status = 2;
    EULA_UNLOCK();
    printf("[cellSysutilNpEula] sceNpEulaCheckEulaStatus: completion pending\n");
    return CELL_OK;
}

s32 sceNpEulaAbort(void)
{
    EULA_LOCK();
    if (!eula_opd) { EULA_UNLOCK(); return EULA_NOT_INITIALIZED; }
    eula_status = 4;
    EULA_UNLOCK();
    return CELL_OK;
}

s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg2,
    u32 callbackOpd, u32 callbackArg)
{
    (void)arg2; (void)callbackArg;
    if (!communicationId || !callbackOpd) return EULA_INVALID_ARGUMENT;
    EULA_LOCK();
    int busy = eula_opd != 0;
    EULA_UNLOCK();
    if (busy) return EULA_ALREADY_INITIALIZED;
    /* A real EULA dialog is not implemented. Preserve visible failure rather
     * than creating a request that can never complete. */
    return (s32)0x8002e500u;
}

void ps3_np_eula_tick(void)
{
    EULA_LOCK();
    ps3_guest_caller_fn caller = g_ps3_guest_caller;
    if (!eula_opd || eula_delivering || !caller) { EULA_UNLOCK(); return; }
    eula_delivering = 1;
    u32 opd = eula_opd, arg = eula_arg, status = eula_status;
    EULA_UNLOCK();
    /* Do not hold the lock across guest execution. The in-flight marker keeps
     * reentrant callback polls from delivering this request twice. */
    caller(opd, status, 0, 1, arg, 0, 0, 0, 0);
    EULA_LOCK();
    eula_opd = eula_arg = eula_status = 0; eula_delivering = 0;
    EULA_UNLOCK();
}
