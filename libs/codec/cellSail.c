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
#include "cellPamf.h"
#include "../filesystem/cellFs.h"
#include <stdatomic.h>
#include "sail_graphics_state.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_read32 / vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv -- Clang errors on the implicit declaration MSVC allows */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */

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
static struct {
    u32 self_ea, allocator_ea, callback_ea, arg_ea;
    /* The input callback table may live on the initialization caller's stack. */
    u32 allocator_callbacks_ea, allocator_alloc_opd, allocator_free_opd, allocator_arg;
    int in_use; int state;
    atomic_int stream_event_busy;
}
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
static SailGraphicsConfig s_graphics_configs[SAIL_GRAPHICS_MAX];
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
static void sail_graphics_detach_player(u32 player)
{
    sail_graphics_lock();
    for (int i = 0; i < SAIL_GRAPHICS_MAX; ++i)
        if (s_graphics_configs[i].player_ea == player)
            s_graphics_configs[i].player_ea = 0;
    sail_graphics_unlock();
}
static void sail_graphics_reset(void)
{
    sail_graphics_lock();
    memset(s_graphics_adapters, 0, sizeof(s_graphics_adapters));
    memset(s_graphics_configs, 0, sizeof(s_graphics_configs));
    sail_graphics_unlock();
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

static s32 sail_terminate_descriptors(void);

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
    const s32 release = sail_terminate_descriptors();
    if (release) return release;
    printf("[cellSail] Term()\n");
    s_initialized = 0;
    sail_graphics_reset();
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
    return sail_release_player_descriptors((u32)handle);
}

/* CellSailEvent is {be32 major; be32 minor} -- one 64-bit register, major in
 * the high half. Async player calls report completion as major=2
 * (CALL_COMPLETED) with minor = the CellSailPlayerCall id that finished; a
 * title that drives SAIL asynchronously blocks until its handler sees that.
 * We run every call synchronously, so notify right after each one. */
#define SAIL_EV_CALL_COMPLETED 2
enum { SAIL_CALL_BOOT = 1, SAIL_CALL_OPEN_STREAM = 2, SAIL_CALL_CLOSE_STREAM = 3,
       SAIL_CALL_START = 10, SAIL_CALL_STOP = 11 };

static void sail_notify(int slot, u32 major, u32 minor, u64 arg0)
{
    if (slot < 0 || !g_ps3_guest_caller) return;
    const u32 cb = s_sail_players[slot].callback_ea;
    if (!cb) return;
    g_ps3_guest_caller(cb, s_sail_players[slot].arg_ea,
                       ((u64)major << 32) | minor, arg0, 0, 0, 0, 0, 0);
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

/* One synchronous call: transition state, completion, settled state. */
static void sail_call(int slot, u32 call, u32 via, u32 to)
{
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, via);
    sail_notify(slot, SAIL_EV_CALL_COMPLETED, call, 0);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, to);
}

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam)
{
    (void)userParam;
    printf("[cellSail] PlayerBoot(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(_sp, SAIL_CALL_BOOT, SAIL_ST_BOOT_TRANSITION, SAIL_ST_CLOSED);
    return CELL_OK;
}

/* The URI belongs to an added descriptor. r4 is the asynchronous user token,
 * not a guest string. Only one already-opened local PAMF source is supported. */
static s32 sail_stream_lifecycle(u32 player, u64 userParam, int close_stream);
s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, u64 userParam)
{
    return sail_stream_lifecycle(handle, userParam, 0);
}

s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle, u64 userParam)
{
    return sail_stream_lifecycle(handle, userParam, 1);
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
    sail_call(_sp, SAIL_CALL_START, SAIL_ST_START_TRANSITION, SAIL_ST_RUNNING);
    /* Nothing decodes the stream, so it has already ended: a finished stream
     * drops the player back to OPENED, which is what a title waiting out a
     * movie (Midway's logo AVIs) watches for. */
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerStop(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerStop(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_call(_sp, SAIL_CALL_STOP, SAIL_ST_STOP_TRANSITION, SAIL_ST_OPENED);
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
    if (sail_player_slot((u32)handle) < 0) return SAIL_GRAPHICS_INVALID_ARG;
    if (index != 0) return SAIL_GRAPHICS_NOT_AVAILABLE;
    const u32 ea = (u32)(uintptr_t)adapter;
    if (!ea) return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(ea);
    s32 result = SAIL_GRAPHICS_INVALID_STATE;
    if (slot >= 0) {
        result = CELL_OK;
        for (int i = 0; i < SAIL_GRAPHICS_MAX; ++i)
            if ((i != slot && s_graphics_configs[i].player_ea == (u32)handle) ||
                (i == slot && s_graphics_configs[i].player_ea &&
                 s_graphics_configs[i].player_ea != (u32)handle))
                result = SAIL_GRAPHICS_INVALID_STATE;
        if (result == CELL_OK) s_graphics_configs[slot].player_ea = (u32)handle;
    }
    sail_graphics_unlock();
    return result;
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
    u32 uri_input_ea, uri_ea, owned_ea, header_ea, reader_ea;
    u32 free_opd, free_arg;
    u64 file_size;
    FILE* media;
    int opened;
    int stream_open, close_pending;
    atomic_int close_notified;
    /* Audio/video destination slot zero. Reader and stream bytes are snapshots;
     * the header pointers inside reader remain descriptor-owned until release. */
    struct { int valid; u8 reader[128]; u8 stream[48]; } selections[2];
} SailDescriptor;

static SailDescriptor s_descs[MAX_DESCRIPTORS];
/* The guest-call bridge uses a shared scratch stack. Fail reentrant descriptor
 * mutations rather than run another allocation callback on that stack. */
static atomic_int s_descriptor_callback_busy;
static int sail_descriptor_enter(void)
{
    int expected = 0;
    return atomic_compare_exchange_strong(&s_descriptor_callback_busy, &expected, 1);
}
static s32 sail_descriptor_result(s32 result)
{
    atomic_store(&s_descriptor_callback_busy, 0);
    return result;
}
extern u64 ppu_guest_call(u32, u64, u64, u64, u64, u64, u64, u64, u64);
static void sail_descriptor_release(SailDescriptor* d)
{
    if (d->media) fclose(d->media);
    if (d->owned_ea) {
        (void)ppu_guest_call(d->free_opd, d->free_arg, 128, d->owned_ea, 0, 0, 0, 0, 0);
    }
    memset(d, 0, sizeof(*d));
}

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

/* A metadata-backed stream is not a decoder. These events describe source
 * open/close only; Start remains a separate, currently stubbed playback API.
 * Close retains guest metadata until the queued STREAM_CLOSED consumer calls
 * DestroyDescriptor. No descriptor lock is held while invoking guest code. */
static void sail_stream_notify(int slot, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (g_ps3_guest_caller && s_sail_players[slot].callback_ea)
        g_ps3_guest_caller(s_sail_players[slot].callback_ea,
            s_sail_players[slot].arg_ea, ((u64)major << 32) | minor,
            arg0, arg1, 0, 0, 0, 0);
}

static s32 sail_stream_lifecycle(u32 player, u64 userParam, int close_stream)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    const int slot = sail_player_slot(player);
    if (slot < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_sail_players[slot].stream_event_busy)
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    int desc = -1;
    for (int i = 0; i < MAX_DESCRIPTORS; ++i) {
        SailDescriptor* d = &s_descs[i];
        if (!d->in_use || d->player != player) continue;
        if (d->close_pending) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
        if (close_stream ? d->stream_open : d->registered) {
            if (desc >= 0) return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
            desc = i;
        }
    }
    if (desc < 0) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    SailDescriptor* d = &s_descs[desc];
    if (!d->opened || (!close_stream && d->stream_open))
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if (!g_ps3_guest_caller || !s_sail_players[slot].callback_ea)
        return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
    if (close_stream) {
        if (d->media) { fclose(d->media); d->media = NULL; }
        d->stream_open = 0;
        d->registered = 0;
        d->close_pending = 1;
    } else {
        d->stream_open = 1;
    }
    s_sail_players[slot].stream_event_busy = 1;
    sail_descriptor_result(CELL_OK);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0,
        close_stream ? SAIL_ST_CLOSE_TRANSITION : SAIL_ST_OPEN_TRANSITION, 0);
    if (close_stream) d->close_notified = 1;
    sail_stream_notify(slot, close_stream ? 5 : 4, 0, (u32)desc, 0);
    sail_stream_notify(slot, SAIL_EV_CALL_COMPLETED,
        close_stream ? SAIL_CALL_CLOSE_STREAM : SAIL_CALL_OPEN_STREAM, 0, userParam);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0,
        close_stream ? SAIL_ST_CLOSED : SAIL_ST_OPENED, 0);
    s_sail_players[slot].stream_event_busy = 0;
    fprintf(stderr, "[cellSail] Player%sStream(self=%08X desc=%u) -> 00000000\n",
        close_stream ? "Close" : "Open", player, (u32)desc);
    return CELL_OK;
}

/* Local HLE cleanup policy: never discard registered descriptors, and release
 * unregistered host records when their player is successfully destroyed. */
static s32 sail_terminate_descriptors(void)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; ++i)
        if (s_sail_players[i].in_use && s_sail_players[i].stream_event_busy)
            return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && (s_descs[i].stream_open || s_descs[i].close_pending))
            return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use) sail_descriptor_release(&s_descs[i]);
    return sail_descriptor_result(CELL_OK);
}

static s32 sail_release_player_descriptors(u32 player)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    const int slot = sail_player_slot(player);
    if (slot < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_sail_players[slot].stream_event_busy) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == player &&
            (s_descs[i].registered || s_descs[i].close_pending))
            return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == player)
            sail_descriptor_release(&s_descs[i]);
    sail_graphics_detach_player(player);
    s_sail_players[slot].in_use = 0;
    s_sail_players[slot].state = CELL_SAIL_PLAYER_STATE_CLOSED;
    return sail_descriptor_result(CELL_OK);
}

/* NID 0x18B4629D. Bounded host-player teardown: preserve registered
 * descriptors and return INVALID_STATE until their lifecycle is resolved.
 * No private guest layout or playback completion is fabricated. */
s32 cellSailPlayerFinalize(u32 pSelf)
{
    const s32 result = sail_release_player_descriptors(pSelf);
    fprintf(stderr, "[cellSail] PlayerFinalize(self=%08X) -> %08X%s\n",
            pSelf, (u32)result, result == SAIL_DESC_INVALID_STATE ?
            " (registered descriptors or callback in progress)" : "");
    return result;
}

s32 cellSailPlayerGetDescriptorCount(CellSailPlayerHandle handle)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (sail_player_slot((u32)handle) < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    s32 count = 0;
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == handle && s_descs[i].registered)
            ++count;
    return sail_descriptor_result(count);
}


s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    (void)mediaInfo;
    const u64 output = (u64)(uintptr_t)desc;
    if (sail_player_slot((u32)handle) < 0 || (output & 3u) || !sail_desc_range(output, 4))
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);

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
            if (!sail_desc_range((u64)ea + n, 1)) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            uri_host[n++] = c;
        }
        if (n == sizeof(uri_host) - 1 && (!sail_desc_range((u64)ea + n, 1) || vm_read8(ea + (u32)n))) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
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
            s_descs[i].uri_input_ea = (u32)(uintptr_t)uri;
            snprintf(s_descs[i].uri, sizeof(s_descs[i].uri), "%s", uri_host);
            vm_write32((u32)(uintptr_t)desc, (u32)i);   /* guest out-param */
            return sail_descriptor_result(CELL_OK);
        }
    }
    return sail_descriptor_result(SAIL_DESC_MEMORY);
}

s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (!sail_owned_descriptor(handle, desc)) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_descs[desc].registered ||
        (s_descs[desc].close_pending && !s_descs[desc].close_notified))
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    sail_descriptor_release(&s_descs[desc]);
    return sail_descriptor_result(CELL_OK);
}

s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (!sail_owned_descriptor(handle, desc)) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_descs[desc].registered || s_descs[desc].close_pending)
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    s_descs[desc].registered = 1;
    return sail_descriptor_result(CELL_OK);
}

s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle unknownOutput)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    /* The second argument's output contract is unverified. Do not interpret
     * it as a descriptor index or report a removal that never occurred. */
    (void)unknownOutput;
    if (sail_player_slot((u32)handle) < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
}

/* Opens a local PAMF source; this does not start a decoder or emit playback
 * events. Header/URI storage comes from the title's registered allocator and
 * is held until descriptor destruction. Only x-cell-fs local sources exist. */
s32 cellSailDescriptorOpen(CellSailDescriptorHandle desc)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    SailDescriptor* d = &s_descs[desc];
    if (d->stream_open || d->close_pending) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    s32 result = SAIL_DESC_NOT_AVAILABLE;
    FILE* media = NULL;
    u8* header = NULL;
    u32 owned = 0, free_opd = 0, free_arg = 0;
    u32 allocator_seen = 0, callbacks_seen = 0, alloc_opd_seen = 0;
    const char* stage = "source";
    if (d->opened) { result = SAIL_DESC_INVALID_STATE; goto done; }
    if (d->streamType != 0 || strncmp(d->uri, "x-cell-fs://", 12)) goto done;
    const char* path = d->uri + 12;
    if (*path != '/') { result = SAIL_DESC_INVALID_ARG; goto done; }
    char host[CELL_FS_MAX_FS_PATH_LENGTH];
    if (cellfs_translate_path(path, host, sizeof(host))) goto done;
    media = fopen(host, "rb");
    if (!media || fseek(media, 0, SEEK_END)) goto done;
    const long length = ftell(media);
    if (length < 2048 || fseek(media, 0, SEEK_SET)) goto done;
    u8 prefix[16];
    if (fread(prefix, 1, sizeof(prefix), media) != sizeof(prefix) ||
        (memcmp(prefix, "PAMF0041", 8) && memcmp(prefix, "PAMF0040", 8))) { result = (s32)0x80610703u; goto done; }
    const u32 header_units = ((u32)prefix[8]<<24)|((u32)prefix[9]<<16)|((u32)prefix[10]<<8)|prefix[11];
    const u32 data_units = ((u32)prefix[12]<<24)|((u32)prefix[13]<<16)|((u32)prefix[14]<<8)|prefix[15];
    const u64 header_bytes = (u64)header_units * 2048;
    if (!header_units || header_bytes > (u64)length ||
        (u64)data_units * 2048 > (u64)length - header_bytes) {
        result = (s32)0x80610703u; goto done;
    }
    /* Bound metadata residency. Larger headers are unsupported, not truncated. */
    if (header_bytes > 2u*1024u*1024u) goto done;
    header = (u8*)malloc((size_t)header_bytes);
    if (!header) { result = SAIL_DESC_MEMORY; goto done; }
    if (fseek(media, 0, SEEK_SET) || fread(header, 1, (size_t)header_bytes, media) != header_bytes) goto done;
    stage = "allocator";
    const int player_slot = sail_player_slot(d->player);
    if (player_slot < 0) { result = SAIL_DESC_INVALID_STATE; goto done; }
    const u32 allocator = s_sail_players[player_slot].allocator_ea;
    allocator_seen = allocator;
    callbacks_seen = s_sail_players[player_slot].allocator_callbacks_ea;
    const u32 alloc_opd = s_sail_players[player_slot].allocator_alloc_opd;
    alloc_opd_seen = alloc_opd;
    stage = "allocator-opds";
    free_opd = s_sail_players[player_slot].allocator_free_opd;
    free_arg = s_sail_players[player_slot].allocator_arg;
    /* Retain callback identity, not the borrowed table; OPDs themselves remain
     * guest-owned executable descriptors and are revalidated before use. */
    if (!sail_desc_range(alloc_opd, 12) || !sail_desc_range(free_opd, 12) ||
        (alloc_opd & 3) || (free_opd & 3) || !vm_read32(alloc_opd) || !vm_read32(free_opd)) {
        result = SAIL_DESC_INVALID_ARG; goto done;
    }
    const u32 uri_offset = (u32)header_bytes + 128;
    const u32 bytes = (uri_offset + (u32)strlen(d->uri) + 1 + 127) & ~127u;
    stage = "allocation";
    const u64 address = ppu_guest_call(alloc_opd, free_arg, 128, bytes, 0, 0, 0, 0, 0);
    if (!address) { result = SAIL_DESC_MEMORY; goto done; }
    if (address > 0xFFFFFFFFu) { result = SAIL_DESC_MEMORY; goto done; }
    owned = (u32)address;
    if ((owned & 127) || !sail_desc_range(owned, bytes)) { result = SAIL_DESC_MEMORY; goto done; }
    memcpy(vm_base + owned, header, (size_t)header_bytes);
    const u32 reader = owned + (u32)header_bytes;
    stage = "pamf-reader";
    result = cellPamfReaderInitialize((CellPamfReader*)(uintptr_t)reader,
                                     (void*)(uintptr_t)owned, (u64)length, 0);
    if (result) goto done;
    memcpy(vm_base + owned + uri_offset, d->uri, strlen(d->uri) + 1);
    d->owned_ea = owned; d->header_ea = owned; d->reader_ea = reader;
    d->uri_ea = owned + uri_offset; d->free_opd = free_opd; d->free_arg = free_arg;
    d->file_size = (u64)length; d->media = media; d->opened = 1;
    owned = 0; media = NULL;
    fprintf(stderr, "[cellSail] DescriptorOpen(desc=%u bytes=%llu header=%llu guest=%08X) -> opened PAMF metadata\n",
            desc, (unsigned long long)d->file_size, (unsigned long long)header_bytes, d->header_ea);
    result = CELL_OK;
done:
    if (result) fprintf(stderr, "[cellSail] DescriptorOpen(desc=%u) -> %08X stage=%s allocator=%08X callbacks=%08X alloc=%08X free=%08X uri=%s\n", desc, (u32)result, stage, allocator_seen, callbacks_seen, alloc_opd_seen, free_opd, d->uri);
    if (owned) (void)ppu_guest_call(free_opd, free_arg, 128, owned, 0, 0, 0, 0, 0);
    free(header);
    if (media) fclose(media);
    atomic_store(&s_descriptor_callback_busy, 0);
    return result;
}

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize, u64 arg)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    const u64 output = (u64)(uintptr_t)dbAddr;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (!s_descs[desc].opened) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if (dbSize < 128 || (output & 3) || !sail_desc_range(output, 128) || arg)
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    return sail_descriptor_result(cellPamfReaderInitialize((CellPamfReader*)dbAddr,
        (void*)(uintptr_t)s_descs[desc].header_ea, s_descs[desc].file_size, 0));
}

/* Validate the public copied reader using PAMF's parser, then bind it to this
 * descriptor's owned header. All pointer arithmetic is checked before reads. */
static s32 sail_descriptor_reader(SailDescriptor* d, void* database, u32* stream)
{
    const u64 ea = (u64)(uintptr_t)database;
    if ((ea & 3) || !sail_desc_range(ea, 128)) return SAIL_DESC_INVALID_ARG;
    if (vm_read32(ea + 0x24) != d->header_ea) return SAIL_DESC_INVALID_ARG;
    const s32 index = cellPamfReaderGetStreamIndex((CellPamfReader*)database);
    if (index < 0) return SAIL_DESC_INVALID_ARG;
    const u64 selected = (u64)d->header_ea + 0x88 + 48ull * (u32)index;
    if (!sail_desc_range(selected, 48)) return SAIL_DESC_INVALID_ARG;
    *stream = (u32)selected;
    return CELL_OK;
}

/* Bounded observed PAMF form. unknownMode's firmware meaning is not established;
 * only the retail value one and destination slot zero are supported here. */
s32 cellSailDescriptorSetEs(CellSailDescriptorHandle desc, u32 category,
                            u32 destinationSlot, u32 unknownMode, void* database)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    SailDescriptor* d = &s_descs[desc];
    if (!d->opened) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if (category > 2) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (d->streamType != 0 || category == 2 || destinationSlot || unknownMode != 1)
        return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
    u32 stream;
    s32 rc = sail_descriptor_reader(d, database, &stream);
    if (rc) return sail_descriptor_result(rc);
    const u8 coding = vm_base[stream];
    const int is_audio = coding == 0xdc || coding == 0x80 || coding == 0x81;
    const int is_video = coding == 0x1b || coding == 0x02;
    if ((category == 0 && !is_audio) || (category == 1 && !is_video))
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    memcpy(d->selections[category].reader, vm_base + (uintptr_t)database, 128);
    memcpy(d->selections[category].stream, vm_base + stream, 48);
    d->selections[category].valid = 1;
    return sail_descriptor_result(CELL_OK);
}

s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc, void* database)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use)
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    SailDescriptor* d = &s_descs[desc];
    if (!d->opened) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    u32 stream;
    const s32 rc = sail_descriptor_reader(d, database, &stream);
    /* Caller owns the copied reader; no allocation to release or bytes to erase.
     * Previously selected ES snapshots survive reuse of the caller's stack. */
    return sail_descriptor_result(rc);
}

s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    return sail_descriptor_result(s_descs[desc].streamType);
}

s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    (void)enable;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    return sail_descriptor_result(CELL_OK);
}

u32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc)
{
    if (!sail_descriptor_enter()) return 0;
    if (desc >= MAX_DESCRIPTORS || !s_descs[desc].in_use) return sail_descriptor_result(0);
    /* A stable URI exists only after a successful local source open. */
    return sail_descriptor_result(s_descs[desc].uri_ea);
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
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    (void)pAttribute; (void)pResource;
    printf("[cellSail] PlayerInitialize2(self=0x%08X alloc=0x%08X cb=0x%08X)\n",
           pSelf, pAllocator, pCallback);
    if (!pSelf) return sail_descriptor_result((s32)CELL_SAIL_ERROR_INVALID_ARGUMENT);
    const int existing = sail_player_slot(pSelf);
    if (existing >= 0 && s_sail_players[existing].stream_event_busy)
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);

    /* PS3_NO_SAIL=1: report that the player cannot be created. On hardware
     * this call spawns the SAIL player's own worker threads, and a title that
     * then waits for one of them to signal readiness will wait forever against
     * a stub that spawns nothing. Failing cleanly lets a title take its
     * "no video" path instead of deadlocking during setup. */
    if (getenv("PS3_NO_SAIL")) {
        printf("[cellSail] PS3_NO_SAIL -- reporting player creation failure\n");
        return sail_descriptor_result((s32)CELL_SAIL_ERROR_INVALID_ARGUMENT);
    }

    u32 callbacks = 0, alloc_opd = 0, free_opd = 0, allocator_arg = 0;
    if (pAllocator) {
        if ((pAllocator & 3) || !sail_desc_range(pAllocator, 8))
            return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
        callbacks = vm_read32(pAllocator);
        allocator_arg = vm_read32(pAllocator + 4);
        if ((callbacks & 3) || !sail_desc_range(callbacks, 8))
            return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
        alloc_opd = vm_read32(callbacks); free_opd = vm_read32(callbacks + 4);
        if ((alloc_opd & 3) || (free_opd & 3) ||
            !sail_desc_range(alloc_opd, 12) || !sail_desc_range(free_opd, 12) ||
            !vm_read32(alloc_opd) || !vm_read32(free_opd))
            return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    }
    /* Null allocator preserves the existing registration-only player policy.
     * DescriptorOpen cannot allocate until a player has real callbacks. */
    sail_zero_guest(pSelf, SAIL_PLAYER_CLEAR);

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_sail_players[i].in_use) {
            s_sail_players[i].in_use       = 1;
            s_sail_players[i].self_ea      = pSelf;
            s_sail_players[i].allocator_ea = pAllocator;
            s_sail_players[i].allocator_callbacks_ea = callbacks;
            s_sail_players[i].allocator_alloc_opd = alloc_opd;
            s_sail_players[i].allocator_free_opd = free_opd;
            s_sail_players[i].allocator_arg = allocator_arg;
            s_sail_players[i].callback_ea  = pCallback;
            s_sail_players[i].arg_ea       = pUserParam;
            break;
        }
    }
    return sail_descriptor_result(CELL_OK);
}

/* cellSailSoundAdapterInitialize(pSelf, pCallbacks, pArg) -- NID 0x3D0D3B72.
 * Same reasoning: zero it rather than leave the guest reading stale memory. */
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    return CELL_OK;
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
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_GRAPHICS_INVALID_ARG;
    SailGraphicsConfig config = {0};
    config.self_ea = pSelf;
    config.arg_ea = pArg;
    /* Borrowed table bytes may overlap the object we clear. Snapshot first.
     * OPD records themselves remain guest-owned until Finalize. */
    if (pCallbacks) {
        if ((pCallbacks & 3) || !sail_graphics_range(pCallbacks, 20))
            return SAIL_GRAPHICS_INVALID_ARG;
        for (int i = 0; i < 5; ++i) {
            const u32 opd = vm_read32(pCallbacks + 4*i);
            if ((opd & 3) || !sail_graphics_range(opd, 12) ||
                ((u64)opd < (u64)pSelf + SAIL_ADAPTER_CLEAR && (u64)opd + 12 > pSelf) ||
                !vm_read32(opd))
                return SAIL_GRAPHICS_INVALID_ARG;
            config.callbacks[i] = opd;
        }
    }
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
    s_graphics_configs[slot] = config;
    sail_graphics_unlock();
    return CELL_OK;
}

s32 cellSailGraphicsAdapterFinalize(u32 pSelf)
{
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    if (slot >= 0) {
        s_graphics_adapters[slot] = 0;
        memset(&s_graphics_configs[slot], 0, sizeof(s_graphics_configs[slot]));
    }
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

/* Preferred bytes express a request, never an actual decoded format. */
s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR) ||
        (pFormat & 3) || !sail_graphics_range(pFormat, 32))
        return SAIL_GRAPHICS_INVALID_ARG;
    u8 format[32];
    memcpy(format, vm_base + pFormat, sizeof(format));
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    if (slot >= 0) {
        memcpy(s_graphics_configs[slot].preferred, format, sizeof(format));
        s_graphics_configs[slot].has_preferred = 1;
    }
    sail_graphics_unlock();
    return slot >= 0 ? CELL_OK : SAIL_GRAPHICS_INVALID_STATE;
}

/* No actual session exists until a decoder publishes a concrete format.
 * Leave output untouched on failure; preferred dimensions are not readiness. */
s32 cellSailGraphicsAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    if (!sail_graphics_range(pSelf, SAIL_ADAPTER_CLEAR) ||
        (pFormat & 3) || !sail_graphics_range(pFormat, 32))
        return SAIL_GRAPHICS_INVALID_ARG;
    sail_graphics_lock();
    const int slot = sail_graphics_slot(pSelf);
    sail_graphics_unlock();
    return slot >= 0 ? SAIL_GRAPHICS_NOT_AVAILABLE : SAIL_GRAPHICS_INVALID_STATE;
}

/* Internal decoder boundary: snapshot retained guest records under the lock,
 * then allow the consumer to invoke guest callbacks outside it. */
s32 sail_graphics_config_for_player(u32 player, SailGraphicsConfig* out)
{
    if (!out || sail_player_slot(player) < 0) return SAIL_GRAPHICS_INVALID_ARG;
    s32 result = SAIL_GRAPHICS_NOT_AVAILABLE;
    sail_graphics_lock();
    for (int i = 0; i < SAIL_GRAPHICS_MAX; ++i) {
        if (s_graphics_adapters[i] && s_graphics_configs[i].player_ea == player) {
            *out = s_graphics_configs[i];
            result = CELL_OK;
            break;
        }
    }
    sail_graphics_unlock();
    return result;
}

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs, void* pArg)
{
    /* Preserve the eight-byte public {callbacks, pArg} record. Retail explicitly
     * passes pArg in r5. PlayerInitialize2 captures callbacks before this input
     * table can leave scope; no borrowed callback-table reads happen at Open. */
    const u64 alloc_ea = (u64)(uintptr_t)allocator;
    const u64 funcs_ea = (u64)(uintptr_t)pFuncs;
    extern u32 ppu_vm_size;
    const u64 limit = ppu_vm_size ? ppu_vm_size : 0x100000000ull;
    if (!vm_base || !alloc_ea || !funcs_ea || (alloc_ea & 3) || (funcs_ea & 3) ||
        alloc_ea > limit || funcs_ea > limit ||
        8 > limit - alloc_ea || 8 > limit - funcs_ea)
        return (s32)0x80610701u;
    vm_write32((u32)alloc_ea, (u32)funcs_ea);
    vm_write32((u32)alloc_ea + 4, (u32)(uintptr_t)pArg);
    return CELL_OK;
}
