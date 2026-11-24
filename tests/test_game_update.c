/* cellGameUpdate through the real HLE registration table, cellNetCtl and the
 * cellSysutil pump: every check answers "no update" from a later poll, once;
 * Finish answers "finished"; Abort and Term succeed; Term drops what is not
 * yet delivered. Synthetic guest addresses only; no game files. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../libs/network/cellNetCtl.h"
#include "../libs/system/cellSysutil.h"
#include "../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"

static unsigned char memory[4096];
uint8_t* vm_base = memory;
uint32_t ppu_vm_size = sizeof memory;
/* This single-threaded fixture has no reservations, write watches, dialogs or NP. */
int g_resv_store_active; uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
int spu_coh_is_reserved(uint32_t ea) {(void)ea; return 0;}
void spu_lockline_lock(void) {} void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) {(void)a; (void)v; (void)w;}
void cellOskDialog_tick(void) {}
void cellMsgDialog_tick(void) {}
void np_basic_social_tick(void) {}
void np_psnr_pump(void) {}
void ps3_np_eula_tick(void) {}
void ps3_np_ticket_tick(void) {}
const char* np_psnr_identity(void) { return NULL; }

/* The registration table generated from the sources (gen_hle_nids.py). */
void ppu_hle_register_all(void);
static const struct { unsigned nid; const char* name; void* fn; } k_expected[] = {
    {0x99AB1A26u, "cellGameUpdateInit",                         (void*)cellGameUpdateInit},
    {0x10DAE56Du, "cellGameUpdateTerm",                         (void*)cellGameUpdateTerm},
    {0xD0A5D727u, "cellGameUpdateCheckStartAsync",              (void*)cellGameUpdateCheckStartAsync},
    {0xFFA3D791u, "cellGameUpdateCheckFinishAsync",             (void*)cellGameUpdateCheckFinishAsync},
    {0xA5E1FA60u, "cellGameUpdateCheckStartWithoutDialogAsync", (void*)cellGameUpdateCheckStartWithoutDialogAsync},
    {0x3E359AB6u, "cellGameUpdateCheckAbort",                   (void*)cellGameUpdateCheckAbort},
};
#define N_EXPECTED (sizeof k_expected / sizeof k_expected[0])
static unsigned registered[N_EXPECTED];
void ps3_hle_register(unsigned nid, const char* name, void* fn)
{
    for (size_t i = 0; i < N_EXPECTED; i++) {
        if (k_expected[i].nid != nid) continue;
        assert(!strcmp(name, k_expected[i].name));
        assert(fn == k_expected[i].fn);
        registered[i]++;
    }
}

#define CB_OPD   0x1000u
#define CB2_OPD  0x1100u
#define USERDATA 0x2000u
#define PARAM_EA 0x3000u

typedef struct { u32 opd; s32 status; s32 error; u32 userdata; } Delivery;
static Delivery got[16];
static unsigned n_got;
static enum { REACT_NONE, REACT_FINISH, REACT_TERM, REACT_NESTED_POLL } react;

static void guest_caller(u32 opd, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h)
{
    assert(!(d | e | f | g | h));
    assert(n_got < sizeof got / sizeof got[0]);
    got[n_got++] = (Delivery){opd, (s32)a, (s32)b, (u32)c};
    switch (react) {
    case REACT_FINISH:
        react = REACT_NONE;
        assert(cellGameUpdateCheckFinishAsync((CellGameUpdateCallback)(uintptr_t)CB2_OPD,
                                              (void*)(uintptr_t)USERDATA) == CELL_OK);
        {   /* Not delivered from inside this callback, even by a nested poll. */
            unsigned before = n_got;
            cellSysutilCheckCallback();
            assert(n_got == before);
        }
        break;
    case REACT_TERM:
        react = REACT_NONE;
        assert(cellGameUpdateTerm() == CELL_OK);
        break;
    case REACT_NESTED_POLL:
        react = REACT_NONE;
        cellSysutilCheckCallback();   /* must not deliver this answer again */
        break;
    default:
        break;
    }
}

static void expect(unsigned index, u32 opd, s32 status)
{
    assert(index < n_got);
    assert(got[index].opd == opd);
    assert(got[index].status == status);
    assert(got[index].error == 0);
    assert(got[index].userdata == USERDATA);
}

static CellGameUpdateCallback cb(u32 opd) { return (CellGameUpdateCallback)(uintptr_t)opd; }
static void* user(void) { return (void*)(uintptr_t)USERDATA; }
static const CellGameUpdateParam* param(void) { return (const CellGameUpdateParam*)(uintptr_t)PARAM_EA; }

int main(void)
{
    /* NID registration: each name the title imports resolves to its handler. */
    ppu_hle_register_all();
    for (size_t i = 0; i < N_EXPECTED; i++) {
        if (registered[i] != 1) {
            fprintf(stderr, "FAIL: %s (0x%08X) registered %u times\n",
                    k_expected[i].name, k_expected[i].nid, registered[i]);
            return 1;
        }
    }

    /* Before Init everything but Init is refused. */
    assert((u32)cellGameUpdateTerm() == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);
    assert((u32)cellGameUpdateCheckAbort() == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);
    assert((u32)cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);
    assert((u32)cellGameUpdateCheckFinishAsync(cb(CB_OPD), user()) == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);
    assert(cellGameUpdateInit() == CELL_OK);
    assert((u32)cellGameUpdateInit() == CELL_GAMEUPDATE_ERROR_ALREADY_INITIALIZED);
    assert((u32)cellGameUpdateCheckStartAsync(NULL, cb(CB_OPD), user()) == CELL_GAMEUPDATE_ERROR_INVALID_ADDR);
    assert((u32)cellGameUpdateCheckStartAsync(param(), NULL, user()) == CELL_GAMEUPDATE_ERROR_INVALID_ADDR);
    assert((u32)cellGameUpdateCheckStartWithoutDialogAsync(NULL, user()) == CELL_GAMEUPDATE_ERROR_INVALID_ADDR);

    /* No guest caller yet: the answer waits, it is not lost. */
    assert(cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_OK);
    cellSysutilCheckCallback();
    assert(n_got == 0);
    g_ps3_guest_caller = guest_caller;

    /* A second Start while the first answer waits is BUSY. */
    assert((u32)cellGameUpdateCheckStartWithoutDialogAsync(cb(CB_OPD), user()) == CELL_GAMEUPDATE_ERROR_BUSY);

    /* The pump delivers "no update" once; a nested poll does not repeat it. */
    react = REACT_NESTED_POLL;
    cellSysutilCheckCallback();
    assert(n_got == 1);
    expect(0, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    cellSysutilCheckCallback();
    assert(n_got == 1);

    /* Dialog flow: Start, then Finish from inside the Start callback. */
    assert(cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_OK);
    assert(n_got == 1);   /* never synchronous */
    react = REACT_FINISH;
    cellSysutilCheckCallback();
    assert(n_got == 2);
    expect(1, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    assert((u32)cellGameUpdateCheckFinishAsync(cb(CB2_OPD), user()) == CELL_GAMEUPDATE_ERROR_BUSY);
    cellSysutilCheckCallback();
    assert(n_got == 3);
    expect(2, CB2_OPD, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED);

    /* Finish issued before the Start answer arrives is delivered after it. */
    assert(cellGameUpdateCheckStartWithoutDialogAsync(cb(CB_OPD), user()) == CELL_OK);
    assert(cellGameUpdateCheckFinishAsync(cb(CB2_OPD), user()) == CELL_OK);
    cellSysutilCheckCallback();
    assert(n_got == 5);
    expect(3, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    expect(4, CB2_OPD, CELL_GAMEUPDATE_RESULT_STATUS_FINISHED);

    /* Abort with nothing pending succeeds and delivers nothing. */
    assert(cellGameUpdateCheckAbort() == CELL_OK);
    cellSysutilCheckCallback();
    assert(n_got == 5);
    /* Abort of a pending check succeeds; the check answers ABORTED, once. */
    assert(cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_OK);
    assert(cellGameUpdateCheckAbort() == CELL_OK);
    cellSysutilCheckCallback();
    cellSysutilCheckCallback();
    assert(n_got == 6);
    expect(5, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_ABORTED);

    /* Term drops an undelivered answer; nothing calls the title afterwards. */
    assert(cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_OK);
    assert(cellGameUpdateTerm() == CELL_OK);
    cellSysutilCheckCallback();
    assert(n_got == 6);
    assert((u32)cellGameUpdateTerm() == CELL_GAMEUPDATE_ERROR_NOT_INITIALIZED);

    /* Term from inside a callback drops the rest of that poll's answers, and a
     * fresh Init starts clean. */
    assert(cellGameUpdateInit() == CELL_OK);
    assert(cellGameUpdateCheckStartAsync(param(), cb(CB_OPD), user()) == CELL_OK);
    assert(cellGameUpdateCheckFinishAsync(cb(CB2_OPD), user()) == CELL_OK);
    react = REACT_TERM;
    cellSysutilCheckCallback();
    assert(n_got == 7);
    expect(6, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    cellSysutilCheckCallback();
    assert(n_got == 7);
    assert(cellGameUpdateInit() == CELL_OK);
    assert(cellGameUpdateCheckStartWithoutDialogAsync(cb(CB_OPD), user()) == CELL_OK);
    cellSysutilCheckCallback();
    assert(n_got == 8);
    expect(7, CB_OPD, CELL_GAMEUPDATE_RESULT_STATUS_NO_UPDATE);
    assert(cellGameUpdateTerm() == CELL_OK);

    puts("PASS: cellGameUpdate NIDs registered; no-update answer, finish, abort and term through the sysutil pump");
    return 0;
}
