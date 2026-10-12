/* Included by cellSail.c: a bounded opt-in one-picture producer. */
#if defined(PS3RECOMP_SAIL_FFMPEG) && PS3RECOMP_SAIL_FFMPEG
#include "sail_video_decoder.h"
extern s32 cellGcmGetConfiguration(void* output);
extern s32 cellGcmGetOffsetTable(void* output);
typedef struct SailHeldFrame {
    int active, uncertain, made_up, allocated;
    u32 scratch, buffer, session, player;
    u32 alloc_opd, free_opd, allocator_arg;
    SailGraphicsConfig graphics;
} SailHeldFrame;
static SailHeldFrame s_held[CELL_SAIL_PLAYER_MAX];

/* An interrupted callback leaves uncertain set and the descriptor mutation
 * guard held. Its ownership is quarantined, never automatically re-dispatched. */
static s32 sail_held_invoke(SailHeldFrame* h, u32 opd, u64 a, u64 b,
                           u64 c, u64 d, u64* result)
{
    if ((opd & 3) || !sail_desc_range(opd, 12) || !vm_read32(opd))
        return SAIL_DESC_NOT_AVAILABLE;
    h->uncertain = 1;
    const int dispatched = ppu_guest_call_ct_checked(vm_read32(opd), vm_read32(opd + 4),
        a, b, c, d, 0, 0, 0, 0, result);
    h->uncertain = 0;
    return dispatched ? CELL_OK : SAIL_DESC_NOT_AVAILABLE;
}

/* Caller owns descriptor mutation guard, including all guest callbacks. */
static s32 sail_held_release_locked(u32 player)
{
    const int slot = sail_player_slot(player);
    if (slot < 0) return SAIL_DESC_INVALID_ARG;
    SailHeldFrame* h = &s_held[slot];
    if (!h->active) return CELL_OK;
    (void)sail_graphics_unpublish_frame(player);
    if (h->uncertain) return SAIL_DESC_INVALID_STATE;
    u64 result = 0;
    if (h->allocated) {
        s32 rc = sail_held_invoke(h, h->graphics.callbacks[4], h->graphics.arg_ea,
            1, h->scratch + 32, 0, &result);
        if (rc) return rc; /* never dispatched: retain the live pointer array */
        if ((s32)result < 0) { h->uncertain = 1; return SAIL_DESC_INVALID_STATE; }
        h->allocated = 0;
        fprintf(stderr, "[cellSail] HeldFrame freeFrame completed player=%08X buffer=%08X\n", player, h->buffer);
    }
    if (h->made_up) {
        s32 rc = sail_held_invoke(h, h->graphics.callbacks[1], h->graphics.arg_ea,
            0, 0, 0, &result);
        if (rc) return rc;
        if ((s32)result < 0) { h->uncertain = 1; return SAIL_DESC_INVALID_STATE; }
        h->made_up = 0;
    }
    if (h->scratch) {
        s32 rc = sail_held_invoke(h, h->free_opd, h->allocator_arg,
            128, h->scratch, 0, &result);
        if (rc) return rc;
        h->scratch = 0;
    }
    (void)sail_graphics_withdraw_frame(player);
    memset(h, 0, sizeof(*h));
    return CELL_OK;
}

/* Public configuration/tables inspect all pages without AddressToOffset's
 * implicit auto-mapping. The game's resulting VRAM texture still needs native
 * verification; this proves only the existing contiguous GCM translation. */
static s32 sail_held_mapping(SailHeldFrame* h, u32 bytes)
{
    const u32 tmp = h->scratch + 64;
    if (cellGcmGetConfiguration((void*)(uintptr_t)tmp)) return SAIL_DESC_NOT_AVAILABLE;
    const u64 base = vm_read32(tmp), size = vm_read32(tmp + 8);
    if (base && h->buffer >= base && (u64)h->buffer + bytes <= base + size) {
        vm_write32(h->scratch + 36, (u32)(h->buffer - base));
        return CELL_OK;
    }
    if (cellGcmGetOffsetTable((void*)(uintptr_t)tmp)) return SAIL_DESC_NOT_AVAILABLE;
    const u32 table = vm_read32(tmp);
    if (!sail_desc_range(table, 8192)) return SAIL_DESC_NOT_AVAILABLE;
    const u32 first = h->buffer >> 20, last = (h->buffer + bytes - 1) >> 20;
    const u32 first_io = ((u32)vm_base[table + 2 * first] << 8) | vm_base[table + 2 * first + 1];
    if (first_io == 0xFFFF) return SAIL_DESC_NOT_AVAILABLE;
    for (u32 page = first; page <= last; ++page) {
        const u32 io = ((u32)vm_base[table + 2 * page] << 8) | vm_base[table + 2 * page + 1];
        if (io == 0xFFFF || io != first_io + page - first || io >= 4096)
            return SAIL_DESC_NOT_AVAILABLE;
    }
    vm_write32(h->scratch + 36, (first_io << 20) | (h->buffer & 0xFFFFF));
    return CELL_OK;
}

/* Once an owned open source is found, reject setup through real close before
 * queueing an error that may destroy the original movie's event queue. */
static s32 sail_held_reject_open(int slot, u32 player, u64 token, s32 result)
{
    sail_descriptor_result(result);
    if (sail_stream_lifecycle(player, token, 1) == CELL_OK)
        sail_stream_notify(slot, 1, 0, (u64)(s64)result, 0);
    return result;
}

static s32 sail_held_start(u32 player, u32 command, u64 token)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    const int slot = sail_player_slot(player);
    if (slot < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_sail_players[slot].stream_event_busy || s_held[slot].active)
        return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if (!command || !sail_desc_range(command, 4))
        return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    SailDescriptor* d = NULL;
    for (int i = 0; i < MAX_DESCRIPTORS; ++i)
        if (s_descs[i].in_use && s_descs[i].player == player && s_descs[i].stream_open) {
            if (d) return sail_descriptor_result(SAIL_DESC_NOT_AVAILABLE);
            d = &s_descs[i];
        }
    if (!d) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    if (!d->media || !d->selections[1].valid)
        return sail_held_reject_open(slot, player, token, SAIL_DESC_INVALID_STATE);
    const char* base = strrchr(d->uri, '/');
    if (!base || strcmp(base + 1, "thx.pam") || d->selections[1].stream[0] != 2 ||
        d->selections[1].stream[4] < 0xE0 || d->selections[1].stream[4] > 0xEF)
        return sail_held_reject_open(slot, player, token, SAIL_DESC_NOT_AVAILABLE);
    if (!g_ps3_guest_caller || !s_sail_players[slot].callback_ea)
        return sail_held_reject_open(slot, player, token, SAIL_DESC_NOT_AVAILABLE);
    SailHeldFrame* h = &s_held[slot];
    memset(h, 0, sizeof(*h));
    s32 rc = sail_graphics_begin_session(player, &h->graphics, &h->session);
    if (rc) return sail_held_reject_open(slot, player, token, rc);
    h->active = 1; h->player = player;
    h->alloc_opd = s_sail_players[slot].allocator_alloc_opd;
    h->free_opd = s_sail_players[slot].allocator_free_opd;
    h->allocator_arg = s_sail_players[slot].allocator_arg;
    SailVideoFrame picture = {0};
    const u64 offset = (u64)vm_read32(d->header_ea + 8) * 2048;
    const u64 length = (u64)vm_read32(d->header_ea + 12) * 2048;
    const u64 origin = ((u64)vm_base[d->header_ea + 0x56] << 40) |
        ((u64)vm_base[d->header_ea + 0x57] << 32) | vm_read32(d->header_ea + 0x58);
    const unsigned stream_id = 0x100u | d->selections[1].stream[4];
    int decode = sail_video_decode_picture(d->media, offset, length, stream_id, 30, &picture);
    fprintf(stderr, "[cellSail] HeldFrame decode=%d stream=%03X ordinal=30 size=%ux%u pitch=%u pts=%lld\n",
        decode, stream_id, picture.width, picture.height, picture.pitch, (long long)picture.pts);
    rc = SAIL_DESC_NOT_AVAILABLE;
    if (decode || !picture.rgba || picture.width != 1280 || picture.height != 720 ||
        picture.pitch != 5120 || picture.colorspace != 1 || picture.color_range != 1 ||
        picture.interlaced || !picture.pts_valid || picture.time_base_num != 1 ||
        picture.time_base_den != 90000 || picture.rate_num != 30000 || picture.rate_den != 1001)
        goto failed;
    const u32 bytes = picture.pitch * picture.height;
    u64 result = 0;
    rc = sail_held_invoke(h, h->alloc_opd, h->allocator_arg, 128, 128, 0, &result);
    if (rc) goto failed;
    if (!result || result > UINT32_MAX || (result & 127) || !sail_desc_range(result, 128)) {
        /* A nonzero untrusted allocator result cannot be dereferenced or freed
         * safely without its exact contract: quarantine the returned ownership. */
        if (result) { h->uncertain = 1; rc = SAIL_DESC_INVALID_STATE; }
        else rc = SAIL_DESC_MEMORY;
        goto failed;
    }
    h->scratch = (u32)result;
    memset(vm_base + h->scratch, 0, 128);
    u8 format[32] = {1, 0, 8, 3, 5, 0, 2, 0xD0, 0, 0, 0x14, 0, 0, 0, 0, 255, 1, 1, 0};
    memcpy(vm_base + h->scratch, format, 32);
    rc = sail_held_invoke(h, h->graphics.callbacks[0], h->graphics.arg_ea, 0, 0, 0, &result);
    if (rc) goto failed;
    h->made_up = 1;
    if ((s32)result < 0) { rc = (s32)result; goto failed; }
    rc = sail_held_invoke(h, h->graphics.callbacks[3], h->graphics.arg_ea,
        bytes, 1, h->scratch + 32, &result);
    if (rc) goto failed;
    h->buffer = vm_read32(h->scratch + 32);
    /* Count one: zero is full success, positive one is actual partial count.
     * Retain a nonzero pointer even on failure for its paired original free. */
    h->allocated = h->buffer != 0;
    if (h->buffer && !sail_desc_range(h->buffer, bytes)) {
        h->uncertain = 1; rc = SAIL_DESC_INVALID_STATE; goto failed;
    }
    if ((s32)result < 0 || (s32)result > 1 || !h->buffer) {
        rc = SAIL_DESC_MEMORY; goto failed;
    }
    rc = sail_held_mapping(h, bytes);
    if (rc) goto failed;
    memcpy(vm_base + h->buffer, picture.rgba, bytes);
    u64 hash = 14695981039346656037ull;
    for (u32 i = 0; i < bytes; ++i) hash = (hash ^ vm_base[h->buffer + i]) * 1099511628211ull;
    fprintf(stderr, "[cellSail] HeldFrame allocated player=%08X session=%u scratch=%08X buffer=%08X gcmOffset=%08X bytes=%u fnv1a=%016llX\n",
        player, h->session, h->scratch, h->buffer, vm_read32(h->scratch + 36), bytes, (unsigned long long)hash);
    const char* dump = getenv("PS3RECOMP_SAIL_HELD_FRAME_DUMP");
    if (dump && *dump) { FILE* f = fopen(dump, "wb"); if (f) { fwrite(vm_base + h->buffer, 1, bytes, f); fclose(f); } }
    rc = sail_held_invoke(h, h->graphics.callbacks[2], h->graphics.arg_ea,
        h->scratch, h->session, 0, &result);
    if (rc) goto failed;
    /* Firmware's setup path ignores formatChanged's integer return. */
    rc = sail_graphics_publish_frame(player, h->buffer, format, (u64)picture.pts, origin, 1);
    if (rc) goto failed;
    sail_video_frame_release(&picture);
    s_sail_players[slot].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    s_sail_players[slot].stream_event_busy = 1;
    sail_descriptor_result(CELL_OK);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_START_TRANSITION, 0);
    sail_stream_notify(slot, SAIL_EV_CALL_COMPLETED, SAIL_CALL_START, 0, token);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_RUNNING, 0);
    s_sail_players[slot].stream_event_busy = 0;
    fprintf(stderr, "[cellSail] HeldFrame published: immutable diagnostic, no audio or EOS\n");
    return CELL_OK;
failed:
    sail_video_frame_release(&picture);
    { s32 release = sail_held_release_locked(player); if (release) rc = release; }
    fprintf(stderr, "[cellSail] HeldFrame setup failed=%08X quarantined=%d\n", (u32)rc, h->uncertain);
    sail_descriptor_result(rc);
    /* Real stream close queues descriptor cleanup before the movie error
     * destroys its event queue. Unresolved ownership must not be discarded. */
    if (!h->active && sail_stream_lifecycle(player, token, 1) == CELL_OK)
        sail_stream_notify(slot, 1, 0, (u64)(s64)rc, 0);
    return rc;
}

static s32 sail_held_stop(u32 player, u64 token)
{
    if (!sail_descriptor_enter()) return SAIL_DESC_INVALID_STATE;
    const int slot = sail_player_slot(player);
    if (slot < 0) return sail_descriptor_result(SAIL_DESC_INVALID_ARG);
    if (s_sail_players[slot].stream_event_busy) return sail_descriptor_result(SAIL_DESC_INVALID_STATE);
    const s32 rc = sail_held_release_locked(player);
    if (rc) return sail_descriptor_result(rc);
    s_sail_players[slot].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    s_sail_players[slot].stream_event_busy = 1;
    sail_descriptor_result(CELL_OK);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION, 0);
    sail_stream_notify(slot, SAIL_EV_CALL_COMPLETED, SAIL_CALL_STOP, 0, token);
    sail_stream_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED, 0);
    s_sail_players[slot].stream_event_busy = 0;
    return CELL_OK;
}
#else
static s32 sail_held_start(u32 player, u32 command, u64 token)
{ (void)player; (void)command; (void)token; return SAIL_GRAPHICS_NOT_AVAILABLE; }
static s32 sail_held_stop(u32 player, u64 token)
{ (void)player; (void)token; return SAIL_GRAPHICS_NOT_AVAILABLE; }
static s32 sail_held_release_locked(u32 player) { (void)player; return CELL_OK; }
#endif
