/*
 * ps3recomp - cellSail HLE implementation
 *
 * Stub. Player lifecycle and state management work.
 * No actual media playback occurs — games typically show
 * a black screen for cutscenes, which is acceptable for
 * initial recompilation testing.
 */

#include "ps3emu/guest_call.h"
#include "cellSail.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_read32 / vm_write32 */
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>   /* getenv -- Clang errors on the implicit declaration MSVC allows */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

/* Callback-completed futures. The 24-byte public record layout is known, but
 * SDK mutex IDs and private flag values are not. Keep completion/lifecycle
 * state on the host; only the documented result field is published to guest
 * memory after a real Set. No decoder or player readiness is inferred. */
#define SAIL_FUTURE_BYTES 24u
#define SAIL_FUTURE_MAX 64
#define SAIL_FUTURE_INVALID_ARG   ((s32)0x80610701u)
#define SAIL_FUTURE_INVALID_STATE ((s32)0x80610702u)
#define SAIL_FUTURE_NOT_AVAILABLE ((s32)0x80610708u)
#define SAIL_FUTURE_MEMORY        ((s32)0x806107F0u)
static struct { u32 ea; s32 result; int done; } s_futures[SAIL_FUTURE_MAX];
static atomic_flag s_future_lock = ATOMIC_FLAG_INIT;
extern u32 ppu_vm_size; /* zero denotes a full 4-GiB guest address space */
static void sail_future_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_future_lock, memory_order_acquire)) {}
}
static void sail_future_unlock(void)
{
    atomic_flag_clear_explicit(&s_future_lock, memory_order_release);
}
static int sail_future_range(u32 ea, u32 bytes, u32 alignment)
{
    const u64 limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    return vm_base && ea && !(ea & (alignment - 1u)) && (u64)ea + bytes <= limit;
}
static int sail_future_find(u32 ea)
{
    for (int i = 0; i < SAIL_FUTURE_MAX; ++i)
        if (s_futures[i].ea == ea) return i;
    return -1;
}
static void sail_future_clear(void)
{
    sail_future_lock();
    memset(s_futures, 0, sizeof(s_futures));
    sail_future_unlock();
}

/* NID 0x4CC54F8E */
s32 cellSailFutureInitialize(u32 pSelf)
{
    if (!sail_future_range(pSelf, SAIL_FUTURE_BYTES, 8)) return SAIL_FUTURE_INVALID_ARG;
    sail_future_lock();
    if (sail_future_find(pSelf) >= 0) {
        sail_future_unlock(); return SAIL_FUTURE_INVALID_STATE;
    }
    int i = sail_future_find(0);
    if (i < 0) { sail_future_unlock(); return SAIL_FUTURE_MEMORY; }
    for (u32 off = 0; off < SAIL_FUTURE_BYTES; off += 4) vm_write32(pSelf + off, 0);
    s_futures[i].ea = pSelf;
    s_futures[i].result = 0;
    s_futures[i].done = 0;
    sail_future_unlock();
    return CELL_OK;
}

/* NID 0xA37FED15 */
s32 cellSailFutureSet(u32 pSelf, s32 result)
{
    if (!sail_future_range(pSelf, SAIL_FUTURE_BYTES, 8)) return SAIL_FUTURE_INVALID_ARG;
    sail_future_lock();
    int i = sail_future_find(pSelf);
    if (i < 0 || s_futures[i].done) {
        sail_future_unlock(); return SAIL_FUTURE_INVALID_STATE;
    }
    vm_write32(pSelf + 12, (u32)result);
    s_futures[i].result = result;
    s_futures[i].done = 1;
    sail_future_unlock();
    return CELL_OK;
}

/* NID 0x3A2D806C */
s32 cellSailFutureGet(u32 pSelf, u64 timeout, u32 pResult)
{
    (void)timeout; /* Pending waits are not implemented; never fabricate completion. */
    if (!sail_future_range(pSelf, SAIL_FUTURE_BYTES, 8) ||
        !sail_future_range(pResult, 4, 4)) return SAIL_FUTURE_INVALID_ARG;
    sail_future_lock();
    int i = sail_future_find(pSelf);
    if (i < 0) { sail_future_unlock(); return SAIL_FUTURE_INVALID_STATE; }
    if (!s_futures[i].done) { sail_future_unlock(); return SAIL_FUTURE_NOT_AVAILABLE; }
    vm_write32(pResult, (u32)s_futures[i].result);
    sail_future_unlock();
    return CELL_OK;
}

/* NID 0x9553AF65 */
s32 cellSailFutureFinalize(u32 pSelf)
{
    if (!sail_future_range(pSelf, SAIL_FUTURE_BYTES, 8)) return SAIL_FUTURE_INVALID_ARG;
    sail_future_lock();
    int i = sail_future_find(pSelf);
    if (i < 0) { sail_future_unlock(); return SAIL_FUTURE_INVALID_STATE; }
    memset(&s_futures[i], 0, sizeof(s_futures[i]));
    sail_future_unlock();
    return CELL_OK;
}

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    s32 state;
    CellSailPlayerCallback callback;
    void* callbackArg;
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];

/* Players registered by cellSailPlayerInitialize2, keyed by the guest struct EA
 * the title passed as `pSelf`. Declared here because the adapter setters below
 * need it -- they take that same pointer, not an index into s_players[]. */
static struct { u32 self_ea, allocator_ea, callback_ea, arg_ea; int in_use; int state; }
    s_sail_players[CELL_SAIL_PLAYER_MAX];

/* The graphics backend has no decoder and therefore owns no frame buffers.
 * Track initialization separately from the opaque guest adapter bytes. These
 * ABI errors deliberately do not reuse this file's older, misnumbered aliases.
 */
#define SAIL_GRAPHICS_INVALID_ARG   ((s32)0x80610701u)
#define SAIL_GRAPHICS_INVALID_STATE ((s32)0x80610702u)
#define SAIL_GRAPHICS_NOT_AVAILABLE ((s32)0x80610708u)
#define SAIL_GRAPHICS_MEMORY        ((s32)0x806107F0u)
#define SAIL_GRAPHICS_MAX 16
static u32 s_graphics_adapters[SAIL_GRAPHICS_MAX];
static atomic_flag s_graphics_lock = ATOMIC_FLAG_INIT;
extern u32 ppu_vm_size; /* zero denotes the full 4 GiB guest address space */

static void sail_graphics_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_graphics_lock, memory_order_acquire)) {}
}
static void sail_graphics_unlock(void)
{
    atomic_flag_clear_explicit(&s_graphics_lock, memory_order_release);
}
static int sail_graphics_range(u32 ea, u32 bytes)
{
    const u64 limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    return vm_base && ea && (u64)ea + bytes <= limit;
}
static int sail_graphics_slot(u32 ea)
{
    for (int i = 0; i < SAIL_GRAPHICS_MAX; ++i)
        if (s_graphics_adapters[i] == ea) return i;
    return -1;
}
static void sail_graphics_reset(void)
{
    sail_graphics_lock();
    memset(s_graphics_adapters, 0, sizeof(s_graphics_adapters));
    sail_graphics_unlock();
}

/* The sound backend has no decoder and therefore owns no frame buffers.
 * Track initialization separately from the opaque guest adapter bytes. These
 * ABI errors deliberately do not reuse this file's older, misnumbered aliases.
 */
#define SAIL_SOUND_INVALID_ARG   ((s32)0x80610701u)
#define SAIL_SOUND_INVALID_STATE ((s32)0x80610702u)
#define SAIL_SOUND_MEMORY        ((s32)0x806107F0u)
#define SAIL_SOUND_MAX 16
static u32 s_sound_adapters[SAIL_SOUND_MAX];
static atomic_flag s_sound_lock = ATOMIC_FLAG_INIT;
extern u32 ppu_vm_size; /* zero denotes the full 4 GiB guest address space */

static void sail_sound_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_sound_lock, memory_order_acquire)) {}
}
static void sail_sound_unlock(void)
{
    atomic_flag_clear_explicit(&s_sound_lock, memory_order_release);
}
static int sail_sound_range(u32 ea, u32 bytes)
{
    const u64 limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    return vm_base && ea && (u64)ea + bytes <= limit;
}
static int sail_sound_slot(u32 ea)
{
    for (int i = 0; i < SAIL_SOUND_MAX; ++i)
        if (s_sound_adapters[i] == ea) return i;
    return -1;
}
static void sail_sound_reset(void)
{
    sail_sound_lock();
    memset(s_sound_adapters, 0, sizeof(s_sound_adapters));
    sail_sound_unlock();
}

/* A CellSailPlayer is a GUEST POINTER, not a small integer handle.
 *
 * Every player entry point below used to validate its first argument as an
 * index (`handle >= CELL_SAIL_PLAYER_MAX`) into the handle-keyed s_players[]
 * table, which rejects every real call: a guest pointer is always larger than
 * the table. cellSailPlayerInitialize2 has always registered the player by its
 * `pSelf` EA, so the two halves of this file disagreed about what a handle is.
 *
 * That is not a harmless error return. Tokyo Jungle sets its sound adapter
 * during audio init; on failure it abandons the path, closes a resource it
 * never opened ("sgxResClose unknown ID (0)"), terminates its SGX sound system
 * and then blocks forever joining an audio thread that will not exit.
 *
 * One lookup, used by all of them, so the disagreement cannot come back. */
static int sail_player_slot(u32 self_ea)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return i;
    return -1;
}

/* Only the observed five-register form with both trailing arguments zero is
 * supported. Their semantics are unknown: never invent output writes. */
s32 cellSailPlayerReplaceEventHandler(u32 pSelf, u32 pCallback, u32 pArg,
                                     u64 unknownArg3, u64 unknownArg4)
{
    enum { REPLACE_INVALID_ARG = 0x80610701u, REPLACE_NOT_AVAILABLE = 0x80610708u };
    extern u32 ppu_vm_size;
    const u64 limit = ppu_vm_size ? (u64)ppu_vm_size : (1ull << 32);
    const int slot = sail_player_slot(pSelf);
    /* Local conservative policy: require a live player and an aligned guest
     * OPD covering its entry and TOC words. The opaque argument is not read. */
    if (slot < 0 || !vm_base || !pCallback || (pCallback & 3u) ||
        (u64)pCallback + 8u > limit)
        return (s32)REPLACE_INVALID_ARG;
    if (unknownArg3 || unknownArg4)
        return (s32)REPLACE_NOT_AVAILABLE;
    s_sail_players[slot].callback_ea = pCallback;
    s_sail_players[slot].arg_ea = pArg;
    return CELL_OK;
}

/* Lifecycle */

s32 cellSailInit(void)
{
    printf("[cellSail] Init()\n");
    if (s_initialized)
        return (s32)CELL_SAIL_ERROR_ALREADY_INITIALIZED;
    memset(s_players, 0, sizeof(s_players));
    s_initialized = 1;
    return CELL_OK;
}

s32 cellSailTerm(void)
{
    printf("[cellSail] Term()\n");
    s_initialized = 0;
    sail_graphics_reset();
    sail_future_clear();
    sail_sound_reset();
    return CELL_OK;
}

/* Player management */

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle)
{
    (void)attr; (void)resource;
    printf("[cellSail] PlayerCreate()\n");

    if (!s_initialized) return (s32)CELL_SAIL_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_players[i].in_use) {
            memset(&s_players[i], 0, sizeof(SailPlayer));
            s_players[i].in_use = 1;
            s_players[i].state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
            s_players[i].callback = callback;
            s_players[i].callbackArg = callbackArg;
            vm_write32((u32)(uintptr_t)handle, (u32)i);   /* guest out-param */
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

static s32 sail_release_player_descriptors(u32 player);

s32 cellSailPlayerDestroy(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerDestroy(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    const s32 release = sail_release_player_descriptors((u32)handle);
    if (release) return release;
    s_sail_players[_sp].in_use = 0;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_CLOSED;
    return CELL_OK;
}

/* CellSailEvent is {be32 major; be32 minor} -- one 64-bit register, major in
 * the high half. Async player calls report completion as major=2
 * (CALL_COMPLETED) with minor = the CellSailPlayerCall id that finished; a
 * title that drives SAIL asynchronously blocks until its handler sees that.
 * We run every call synchronously, so notify right after each one. */
#define SAIL_EV_CALL_COMPLETED 2
enum { SAIL_CALL_BOOT = 1, SAIL_CALL_OPEN_STREAM = 2, SAIL_CALL_CLOSE_STREAM = 3,
       SAIL_CALL_START = 10, SAIL_CALL_STOP = 11 };

static void sail_notify(int slot, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (slot < 0 || !g_ps3_guest_caller) return;
    const u32 cb = s_sail_players[slot].callback_ea;
    if (!cb) return;
    g_ps3_guest_caller(cb, s_sail_players[slot].arg_ea,
                       ((u64)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
}

/* The player also reports every state it passes through: major=3
 * (PLAYER_STATE_CHANGED), minor 0, arg0 = the new state in the SDK's
 * numbering -- not the CELL_SAIL_PLAYER_STATE_* values in cellSail.h, which
 * are this file's own bookkeeping. Midway Arcade Origins maps arg0 through a
 * nine-entry table (CLOSED=2, OPENED=4, RUNNING=6, ...) and opens nothing until
 * boot has reported CLOSED; without these events it boots the player and then
 * waits on a black screen for good. */
#define SAIL_EV_STATE_CHANGED 3
enum { SAIL_ST_BOOT_TRANSITION = 1, SAIL_ST_CLOSED = 2, SAIL_ST_OPEN_TRANSITION = 3,
       SAIL_ST_OPENED = 4, SAIL_ST_START_TRANSITION = 5, SAIL_ST_RUNNING = 6,
       SAIL_ST_STOP_TRANSITION = 7, SAIL_ST_CLOSE_TRANSITION = 8 };

/* One synchronous call: transition state, completion, settled state.
 * The call token belongs to completion arg1, not either state notification. */
static void sail_call(int slot, u32 call, u32 via, u32 to, u64 userParam)
{
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, via, 0);
    sail_notify(slot, SAIL_EV_CALL_COMPLETED, call, 0, userParam);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, to, 0);
}

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam)
{
    printf("[cellSail] PlayerBoot(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(_sp, SAIL_CALL_BOOT, SAIL_ST_BOOT_TRANSITION, SAIL_ST_CLOSED, userParam);
    return CELL_OK;
}

s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, const char* path)
{
    /* path is a GUEST address; printing it as a host char* faulted here the
     * same way it did in CreateDescriptor. Copy it out of guest memory. */
    char path_host[512];
    path_host[0] = '\0';
    if (path) {
        u32 ea = (u32)(uintptr_t)path;
        size_t n = 0;
        while (n < sizeof(path_host) - 1) {
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            path_host[n++] = c;
        }
        path_host[n] = '\0';
    }
    printf("[cellSail] PlayerOpenStream(%u, \"%s\") - stub\n", handle,
           path ? path_host : "null");
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* Stub: don't actually open anything */
    sail_call(_sp, SAIL_CALL_OPEN_STREAM, SAIL_ST_OPEN_TRANSITION, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_call(_sp, SAIL_CALL_CLOSE_STREAM, SAIL_ST_CLOSE_TRANSITION, SAIL_ST_CLOSED, 0);
    return CELL_OK;
}

s32 cellSailPlayerStart(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStart(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    /* Immediately signal finished since we don't play anything */
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_call(_sp, SAIL_CALL_START, SAIL_ST_START_TRANSITION, SAIL_ST_RUNNING, 0);
    /* Nothing decodes the stream, so it has already ended: a finished stream
     * drops the player back to OPENED, which is what a title waiting out a
     * movie (Midway's logo AVIs) watches for. */
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION, 0);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerStop(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStop(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_call(_sp, SAIL_CALL_STOP, SAIL_ST_STOP_TRANSITION, SAIL_ST_OPENED, 0);
    return CELL_OK;
}

s32 cellSailPlayerPause(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_PAUSE;
    return CELL_OK;
}

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!state) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)state, (u32)s_sail_players[_sp].state);
    return CELL_OK;
}

s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!streamNum) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)streamNum, 0);   /* no streams in stub */
    return CELL_OK;
}

s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info)
{
    (void)streamIndex;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_SAIL_ERROR_NOT_FOUND;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    (void)index; (void)adapter;
    u32 self_ea = (u32)handle;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return CELL_OK;
    return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    (void)index; (void)adapter;
    u32 self_ea = (u32)handle;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return CELL_OK;
    return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
}

s32 cellSailPlayerCancel(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return 0;
    return (s_sail_players[_sp].state == CELL_SAIL_PLAYER_STATE_PAUSE) ? 1 : 0;
}

s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode, void* command)
{
    (void)command;
    printf("[cellSail] SetRepeatMode(%u, %d)\n", handle, repeatMode);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* Real ABI returns the (now-set) repeat mode in r3, not an error code. */
    return repeatMode;
}

/* ---------------------------------------------------------------------------
 * Descriptor management
 * -----------------------------------------------------------------------*/

#define MAX_DESCRIPTORS 16

typedef struct {
    int in_use;
    s32 streamType;
    int registered;
    CellSailPlayerHandle player;
    char uri[512];
} SailDescriptor;

static SailDescriptor s_descs[MAX_DESCRIPTORS];

/* Accurate errors for descriptor registration; legacy aliases are unchanged. */
#define SAIL_DESC_INVALID_ARG ((s32)0x80610701u)
#define SAIL_DESC_INVALID_STATE ((s32)0x80610702u)
#define SAIL_DESC_NOT_AVAILABLE ((s32)0x80610708u)
#define SAIL_DESC_MEMORY ((s32)0x806107F0u)

static int sail_desc_range(u64 ea, u64 size)
{
    extern u32 ppu_vm_size;
    const u64 limit = ppu_vm_size ? (u64)ppu_vm_size : (1ull << 32);
    return vm_base && ea && ea <= limit && size <= limit - ea;
}

static int sail_owned_descriptor(u32 player, u32 desc)
{
    return sail_player_slot(player) >= 0 && desc < MAX_DESCRIPTORS &&
           s_descs[desc].in_use && s_descs[desc].player == player;
}

/* Local HLE cleanup policy: never discard registered descriptors, and release
 * unregistered host records when their player is successfully destroyed. */
static s32 sail_release_player_descriptors(u32 player)
{
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == player && s_descs[i].registered)
            return SAIL_DESC_INVALID_STATE;
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == player)
            memset(&s_descs[i], 0, sizeof(s_descs[i]));
    return CELL_OK;
}

s32 cellSailPlayerGetDescriptorCount(CellSailPlayerHandle handle)
{
    if (sail_player_slot((u32)handle) < 0) return SAIL_DESC_INVALID_ARG;
    s32 count = 0;
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == handle && s_descs[i].registered)
            ++count;
    return count;
}


s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc)
{
    (void)mediaInfo;
    const u64 output = (u64)(uintptr_t)desc;
    if (sail_player_slot((u32)handle) < 0 || (output & 3u) || !sail_desc_range(output, 4))
        return SAIL_DESC_INVALID_ARG;

    /* uri is a GUEST address. Printing it as a host char* faulted the moment
     * the title opened a movie -- the log line stopped mid-token at
     * 'uri="' and the process died with 0xC0000005. Copy it out of guest
     * memory first. The descriptor has always had a uri[512] field; nothing
     * ever filled it, so the URI was discarded even when it did not crash. */
    char uri_host[512];
    uri_host[0] = '\0';
    if (uri) {
        u32 ea = (u32)(uintptr_t)uri;
        size_t n = 0;
        while (n < sizeof(uri_host) - 1) {
            if (!sail_desc_range((u64)ea + n, 1)) return SAIL_DESC_INVALID_ARG;
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            uri_host[n++] = c;
        }
        uri_host[n] = '\0';
    }

    printf("[cellSail] CreateDescriptor(player=%u, type=%d, uri=\"%s\")\n", handle,
           streamType, uri ? uri_host : "null");

    for (int i = 0; i < MAX_DESCRIPTORS; i++) {
        if (!s_descs[i].in_use) {
            memset(&s_descs[i], 0, sizeof(SailDescriptor));
            s_descs[i].in_use = 1;
            s_descs[i].streamType = streamType;
            s_descs[i].player = handle;
            snprintf(s_descs[i].uri, sizeof(s_descs[i].uri), "%s", uri_host);
            vm_write32((u32)(uintptr_t)desc, (u32)i);   /* guest out-param */
            return CELL_OK;
        }
    }
    return SAIL_DESC_MEMORY;
}

s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc)
{
    if (!sail_owned_descriptor(handle, desc)) return SAIL_DESC_INVALID_ARG;
    if (s_descs[desc].registered) return SAIL_DESC_INVALID_STATE;
    memset(&s_descs[desc], 0, sizeof(s_descs[desc]));
    return CELL_OK;
}

s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc)
{
    if (!sail_owned_descriptor(handle, desc)) return SAIL_DESC_INVALID_ARG;
    if (s_descs[desc].registered) return SAIL_DESC_INVALID_STATE;
    s_descs[desc].registered = 1;
    return CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle unknownOutput)
{
    /* The second argument's output contract is unverified. Do not interpret
     * it as a descriptor index or report a removal that never occurred. */
    (void)unknownOutput;
    if (sail_player_slot((u32)handle) < 0) return SAIL_DESC_INVALID_ARG;
    return SAIL_DESC_NOT_AVAILABLE;
}

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize, u64 arg)
{
    (void)dbAddr; (void)dbSize; (void)arg;
    printf("[cellSail] DescriptorCreateDatabase(desc=%u)\n", desc);
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc, s32* type)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use || !type)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)type, (u32)s_descs[desc].streamType);
    return CELL_OK;
}

s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable)
{
    (void)enable;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen)
{
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use || !uri)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    strncpy(GUEST_PTR(uri, char*), s_descs[desc].uri, maxLen - 1);
    uri[maxLen - 1] = '\0';
    return CELL_OK;
}

/* Memory allocator */

/* cellSailPlayerInitialize2(pSelf, pAllocator, pCallback, pUserParam,
 *                          pAttribute, pResource)  -- NID 0x23654375
 *
 * This was unimplemented, so it took the generic "return success and touch
 * nothing" path. That is the worst answer for an initialiser: the guest
 * believes its player struct is set up and reads whatever happened to be in
 * that memory. Tokyo Jungle then dereferenced one of those stale words and
 * faulted reading guest 0x10F000B4, well past the end of main memory.
 *
 * The struct fields are not documented here, and guessing offsets to write
 * would just trade one wrong value for another. What IS safe and sufficient is
 * to ZERO it: every pointer the guest reads out is then NULL, which callers
 * check, instead of a garbage address that faults. The handles we were given
 * are kept on our side, keyed by the struct EA, so no guest layout is assumed.
 */
#define SAIL_PLAYER_CLEAR   0x200u   /* well clear of the caller's next object */
#define SAIL_ADAPTER_CLEAR  0x100u


static void sail_zero_guest(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i += 4) vm_write32(ea + i, 0);
}

s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource)
{
    (void)pAttribute; (void)pResource;
    printf("[cellSail] PlayerInitialize2(self=0x%08X alloc=0x%08X cb=0x%08X)\n",
           pSelf, pAllocator, pCallback);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    /* PS3_NO_SAIL=1: report that the player cannot be created. On hardware
     * this call spawns the SAIL player's own worker threads, and a title that
     * then waits for one of them to signal readiness will wait forever against
     * a stub that spawns nothing. Failing cleanly lets a title take its
     * "no video" path instead of deadlocking during setup. */
    if (getenv("PS3_NO_SAIL")) {
        printf("[cellSail] PS3_NO_SAIL -- reporting player creation failure\n");
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    }

    sail_zero_guest(pSelf, SAIL_PLAYER_CLEAR);

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_sail_players[i].in_use) {
            s_sail_players[i].in_use       = 1;
            s_sail_players[i].self_ea      = pSelf;
            s_sail_players[i].allocator_ea = pAllocator;
            s_sail_players[i].callback_ea  = pCallback;
            s_sail_players[i].arg_ea       = pUserParam;
            break;
        }
    }
    return CELL_OK;
}

/* Opaque sound adapters are keyed by guest EA; no decoded buffers exist. */
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    (void)pArg;
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!sail_sound_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_SOUND_INVALID_ARG;
    sail_sound_lock();
    if (sail_sound_slot(pSelf) >= 0) {
        sail_sound_unlock();
        return SAIL_SOUND_INVALID_STATE;
    }
    const int slot = sail_sound_slot(0);
    if (slot < 0) {
        sail_sound_unlock();
        return SAIL_SOUND_MEMORY;
    }
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    s_sound_adapters[slot] = pSelf;
    sail_sound_unlock();
    return CELL_OK;
}

s32 cellSailSoundAdapterFinalize(u32 pSelf)
{
    if (!sail_sound_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_SOUND_INVALID_ARG;
    sail_sound_lock();
    const int slot = sail_sound_slot(pSelf);
    if (slot >= 0) s_sound_adapters[slot] = 0;
    sail_sound_unlock();
    return slot >= 0 ? CELL_OK : SAIL_SOUND_INVALID_STATE;
}

/* GetFrame returns synchronization flags, not a CELL_OK/frame-ready boolean.
 * CELL_SAIL_SYNC_NO_FRAME is zero; frame-bearing statuses begin at 0x100.
 * The 24-byte guest record is {u32 buffer, session, tag; s32 status; u64 pts}.
 * With no audio decoder, every valid poll explicitly reports no frame and no buffer.
 */
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo)
{
    if (!sail_sound_range(pSelf, SAIL_ADAPTER_CLEAR) ||
        !sail_sound_range(pInfo, 24) || samples > 2048)
        return SAIL_SOUND_INVALID_ARG;
    sail_sound_lock();
    const int slot = sail_sound_slot(pSelf);
    if (slot >= 0) sail_zero_guest(pInfo, 24);
    /* Bounded stderr evidence survives a later strict-HLE halt. No per-frame
     * log flood and no change to player events, playback state, or callbacks. */
    static unsigned logged;
    if (logged < 6) {
        ++logged;
        fprintf(stderr, "[cellSail] SoundAdapterGetFrame(self=%08X samples=%u info=%08X) -> %s\n",
                pSelf, samples, pInfo, slot >= 0 ? "NO_FRAME (decoder unavailable)" : "INVALID_STATE");
    }
    sail_sound_unlock();
    return slot >= 0 ? CELL_SAIL_SYNC_NO_FRAME : SAIL_SOUND_INVALID_STATE;
}

/* cellSailGraphicsAdapterInitialize(pSelf, pCallbacks, pArg)
 *
 * The video-side twin of cellSailSoundAdapterInitialize, and it was missing --
 * so the import returned whatever was in r3 and the guest read an adapter it
 * believed was initialised. Same treatment as the sound adapter: zero the guest
 * struct so every pointer the title reads back is NULL (which callers check)
 * rather than a stale address that faults or is silently followed. */
s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    (void)pArg;
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    if (sail_graphics_slot(pSelf) >= 0) {
        sail_graphics_unlock();
        return SAIL_GRAPHICS_INVALID_STATE;
    }
    const int slot = sail_graphics_slot(0);
    if (slot < 0) {
        sail_graphics_unlock();
        return SAIL_GRAPHICS_MEMORY;
    }
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    s_graphics_adapters[slot] = pSelf;
    sail_graphics_unlock();
    return CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(u32 pSelf)
{
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    if (slot >= 0) s_graphics_adapters[slot] = 0;
    sail_graphics_unlock();
    return slot >= 0 ? CELL_OK : SAIL_GRAPHICS_INVALID_STATE;
}

/* GetFrame returns synchronization flags, not a CELL_OK/frame-ready boolean.
 * CELL_SAIL_SYNC_NO_FRAME is zero; frame-bearing statuses begin at 0x100.
 * The 24-byte guest record is {u32 buffer, session, tag; s32 status; u64 pts}.
 * With no decoder, every valid poll explicitly reports no frame and no buffer.
 */
s32 cellSailGraphicsAdapterGetFrame(u32 pSelf, u32 pInfo)
{
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR) ||
        !sail_graphics_range(pInfo, 24))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    if (slot >= 0) sail_zero_guest(pInfo, 24);
    /* Bounded stderr evidence survives a later strict-HLE halt. No per-frame
     * log flood and no change to player events, playback state, or callbacks. */
    static unsigned logged;
    if (logged < 6) {
        ++logged;
        fprintf(stderr, "[cellSail] GraphicsAdapterGetFrame(self=%08X info=%08X) -> %s\n",
                pSelf, pInfo, slot >= 0 ? "NO_FRAME (decoder unavailable)" : "INVALID_STATE");
    }
    sail_graphics_unlock();
    return slot >= 0 ? CELL_SAIL_SYNC_NO_FRAME : SAIL_GRAPHICS_INVALID_STATE;
}

/* No frame has been decoded or presented, so the backend cannot acknowledge
 * this tag's presentation time. NOT_AVAILABLE is an explicit runtime policy
 * for the unsupported synchronization backend, not simulated playback.
 */
s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u32 tag, u64 time)
{
    (void)tag; (void)time;
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    sail_graphics_unlock();
    return slot >= 0 ? SAIL_GRAPHICS_NOT_AVAILABLE : SAIL_GRAPHICS_INVALID_STATE;
}

/* cellSailGraphicsAdapterSetPreferredFormat(pSelf, pFormat)
 *
 * Records nothing: we do not decode video, so there is no format to honour.
 * It must still answer CELL_OK for a valid adapter, because a title that reads
 * an error here concludes its graphics adapter is unusable. */
s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    printf("[cellSail] GraphicsAdapterSetPreferredFormat(self=0x%08X fmt=0x%08X)\n",
           pSelf, pFormat);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs)
{
    /* The public guest record is {u32 callbacks, u32 pArg}. Initialization
     * stores the callback-table EA; pArg remains caller-owned. Copying the two
     * callback OPDs here corrupts pArg and loses the table indirection. */
    const u64 alloc_ea = (u64)(uintptr_t)allocator;
    const u64 funcs_ea = (u64)(uintptr_t)pFuncs;
    extern u32 ppu_vm_size;
    const u64 limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    if (!vm_base || !alloc_ea || !funcs_ea || (alloc_ea & 3) || (funcs_ea & 3) ||
        alloc_ea > limit || funcs_ea > limit ||
        8 > limit - alloc_ea || 8 > limit - funcs_ea)
        return (s32)0x80610701u;
    vm_write32((u32)alloc_ea, (u32)funcs_ea);
    return CELL_OK;
}
