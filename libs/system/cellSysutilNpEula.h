/*
 * ps3recomp - cellSysutilNpEula HLE
 *
 * NP EULA (End User License Agreement) acceptance dialog.
 * Status checks complete through the guest sysutil callback pump.
 * The legacy cellSysutilNpEulaCheck stub remains for compatibility.
 */

#ifndef PS3RECOMP_CELL_SYSUTIL_NP_EULA_H
#define PS3RECOMP_CELL_SYSUTIL_NP_EULA_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

s32 cellSysutilNpEulaCheck(void* cb, void* arg);

/* Guest effective addresses remain opaque; callbacks are guest OPDs. */
s32 sceNpEulaCheckEulaStatus(u32 communicationId, u32 arg2, u64 arg3,
    u32 callbackOpd, u32 callbackArg);
s32 sceNpEulaAbort(void);
s32 sceNpEulaShowCurrentEula(u32 communicationId, u64 arg2,
    u32 callbackOpd, u32 callbackArg);
/* Internal sysutil pump; dispatches at most one pending status check. */
void ps3_np_eula_tick(void);
#ifdef __cplusplus
}
#endif

#endif
