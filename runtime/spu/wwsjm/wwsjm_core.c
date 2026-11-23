/* Native WWS job manager: the pure core. See wwsjm_core.h and
 * docs/SPURS_NATIVE_WWSJM.md. Written from the behavioural specification. */
#include "wwsjm_core.h"

#include <string.h>

/* Field offsets of the job list line (interface facts). */
enum {
    LINE_IMAGE_EA = 0x00, LINE_IMAGE_SIZE = 0x04, LINE_CURRENT = 0x0A, LINE_SWITCHES = 0x0C,
    LINE_MASK = 0x20, LINE_ARRAY_EA = 0x30, LINE_MARKERS = 0x40
};

static unsigned marker_offset(unsigned a) { return LINE_MARKERS + 16u * a; }
static unsigned lane_offset(unsigned a, unsigned lane) { return marker_offset(a) + 4u + 2u * lane; }

WWSJM_API const char* wwsjm_status_name(wwsjm_status s)
{
    static const char* const names[] = {
        "ok", "argument", "detached array", "entry type", "header", "command", "flags", "set",
        "range", "overlap", "length", "no run-job command", "gather", "service", "too many"
    };
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}

WWSJM_API void wwsjm_line_decode(const uint8_t raw[WWSJM_LINE_BYTES], wwsjm_line* out)
{
    out->image_ea = wwsjm_be32(raw + LINE_IMAGE_EA);
    out->image_size = wwsjm_be32(raw + LINE_IMAGE_SIZE);
    out->current = wwsjm_be16(raw + LINE_CURRENT);
    out->switch_count = wwsjm_be32(raw + LINE_SWITCHES);
    for (unsigned a = 0; a < WWSJM_ARRAYS; a++) {
        out->mask[a] = wwsjm_be32(raw + LINE_MASK + 4u * a);
        out->array_ea[a] = wwsjm_be32(raw + LINE_ARRAY_EA + 4u * a);
        const uint8_t* m = raw + marker_offset(a);
        out->m[a].submitted = wwsjm_be16(m);
        out->m[a].next = wwsjm_be16(m + 2);
        for (unsigned s = 0; s < WWSJM_LANES; s++)
            out->m[a].lane[s] = wwsjm_be16(m + 4 + 2 * s);
    }
}

WWSJM_API uint32_t wwsjm_header_ea(const wwsjm_line* l, unsigned a, uint16_t index)
{
    /* The line holds capacity - 1; capacities need not be powers of two. */
    uint32_t capacity = l->mask[a] + 1u;
    if (!capacity || !l->array_ea[a])          /* malformed: wwsjm_cursor_of refuses it */
        return 0;
    return l->array_ea[a] + 8u * ((uint32_t)index % capacity);
}

WWSJM_API wwsjm_status wwsjm_cursor_of(const wwsjm_line* l, wwsjm_cursor* out)
{
    unsigned a = l->current;
    if (a >= WWSJM_ARRAYS)
        return WWSJM_E_ARG;
    if (!l->array_ea[a] || l->mask[a] == 0xFFFFFFFFu)
        return WWSJM_E_DETACHED;               /* no array, or a capacity of 2^32 */
    out->array = a;
    out->index = l->m[a].next;
    out->header_ea = wwsjm_header_ea(l, a, out->index);
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_header_decode(const uint8_t raw[WWSJM_HEADER_BYTES], wwsjm_header* out)
{
    uint16_t size_word = wwsjm_be16(raw);
    out->size = size_word & 0x0FFF;
    out->type = wwsjm_be16(raw + 2);
    out->ea = wwsjm_be32(raw + 4);
    if (out->type > WWSJM_ENTRY_REPORT)
        return WWSJM_E_ENTRY_TYPE;
    if (size_word & 0xF000)
        return WWSJM_E_HEADER;
    if (out->type == WWSJM_ENTRY_JOB &&
        (out->ea & 15 || out->size < 16 || out->size > WWSJM_MAX_PORTION || out->size & 15))
        return WWSJM_E_HEADER;
    if (wwsjm_header_is_gate(out) && (out->ea & 1))
        return WWSJM_E_HEADER;
    return WWSJM_OK;
}

WWSJM_API wwsjm_verdict wwsjm_judge(const wwsjm_header* h, uint16_t gate_value)
{
    if (h->type == WWSJM_ENTRY_EMPTY)
        return WWSJM_STOP_EMPTY;
    if (wwsjm_header_is_gate(h) && gate_value != 0)
        return WWSJM_STOP_BLOCKED;
    return WWSJM_TAKE;
}

/* a precedes b in the 16-bit modular order */
static int before16(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b) < 0; }

WWSJM_API wwsjm_status wwsjm_claim(const uint8_t old[WWSJM_LINE_BYTES], unsigned lane,
                         const wwsjm_header* h, uint8_t out[WWSJM_LINE_BYTES])
{
    if (lane >= WWSJM_LANES || h->type == WWSJM_ENTRY_EMPTY || h->type > WWSJM_ENTRY_REPORT)
        return WWSJM_E_ARG;
    unsigned a = wwsjm_be16(old + LINE_CURRENT);
    if (a >= WWSJM_ARRAYS)
        return WWSJM_E_ARG;
    memcpy(out, old, WWSJM_LINE_BYTES);
    uint8_t* m = out + marker_offset(a);
    uint16_t taken = wwsjm_be16(m + 2);
    uint16_t next = (uint16_t)(taken + 1);
    wwsjm_put16(m + 2, next);
    if (before16(wwsjm_be16(m), next))
        wwsjm_put16(m, next);
    if (h->type == WWSJM_ENTRY_JOB) {
        uint8_t* mine = out + lane_offset(a, lane);
        if (wwsjm_be16(mine) == WWSJM_NO_MARKER)
            wwsjm_put16(mine, taken);
    } else if (h->type == WWSJM_ENTRY_SWITCH) {
        uint16_t cur = wwsjm_be16(out + LINE_CURRENT);
        wwsjm_put16(out + LINE_CURRENT, (uint16_t)((cur + 1u) % WWSJM_ARRAYS));
        /* a u32 counter: its low halfword borrows from the high one */
        wwsjm_put32(out + LINE_SWITCHES, wwsjm_be32(out + LINE_SWITCHES) - 1u);
    }
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_retire(const uint8_t old[WWSJM_LINE_BYTES], unsigned array, unsigned lane,
                          const wwsjm_held* next, uint8_t out[WWSJM_LINE_BYTES])
{
    if (lane >= WWSJM_LANES || array >= WWSJM_ARRAYS)
        return WWSJM_E_ARG;
    memcpy(out, old, WWSJM_LINE_BYTES);
    uint16_t v = (next && next->valid && next->array == array) ? next->index : WWSJM_NO_MARKER;
    wwsjm_put16(out + lane_offset(array, lane), v);
    return WWSJM_OK;
}

WWSJM_API uint8_t wwsjm_ready_value(const wwsjm_line* l, wwsjm_verdict at_next, uint8_t busy)
{
    unsigned a = l->current;
    if (a >= WWSJM_ARRAYS || at_next != WWSJM_TAKE)
        return 0;
    return before16(l->m[a].next, l->m[a].submitted) ? busy : 0;
}

WWSJM_API wwsjm_status wwsjm_ready_apply(const uint8_t old[WWSJM_LINE_BYTES], unsigned wid, uint8_t value,
                               uint8_t out[WWSJM_LINE_BYTES])
{
    if (wid >= 16)
        return WWSJM_E_ARG;
    memcpy(out, old, WWSJM_LINE_BYTES);
    out[wid] = value;
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_decrement(const uint8_t old[WWSJM_LINE_BYTES], unsigned offset,
                             uint8_t out[WWSJM_LINE_BYTES], uint16_t* value)
{
    if ((offset & 1) || offset > WWSJM_LINE_BYTES - 2)
        return WWSJM_E_ARG;
    memcpy(out, old, WWSJM_LINE_BYTES);
    uint16_t v = (uint16_t)(wwsjm_be16(old + offset) - 1u);
    wwsjm_put16(out + offset, v);
    *value = v;
    return WWSJM_OK;
}

/* ---- command portion ------------------------------------------------------ */

/* Command word 0, most significant first: length in quadwords (14), pad (1),
 * continue-after-discard (1), read (1), gather (1), shareable (1),
 * write-if-discarded (1), command (3), buffer set (4), buffer (5). */
typedef struct {
    uint32_t quads;
    unsigned pad, cont, read, gather, share, wid_flag, cmd, set, buf;
} cmd_word;

static cmd_word split_word(uint32_t w)
{
    cmd_word c;
    c.quads = w >> 18;
    c.pad = (w >> 17) & 1;
    c.cont = (w >> 16) & 1;
    c.read = (w >> 15) & 1;
    c.gather = (w >> 14) & 1;
    c.share = (w >> 13) & 1;
    c.wid_flag = (w >> 12) & 1;
    c.cmd = (w >> 9) & 7;
    c.set = (w >> 5) & 15;
    c.buf = w & 31;
    return c;
}

static uint32_t buffer_bytes(const wwsjm_set* s) { return (uint32_t)s->pages * WWSJM_PAGE_BYTES; }
static uint32_t set_lo(const wwsjm_set* s) { return (uint32_t)s->first_page * WWSJM_PAGE_BYTES; }
static uint32_t set_hi(const wwsjm_set* s) { return set_lo(s) + s->count * buffer_bytes(s); }

/* LS of physical buffer `phys` (0-based) of a set. */
static uint32_t phys_lo(const wwsjm_set* s, unsigned phys) { return set_lo(s) + phys * buffer_bytes(s); }

/* Physical buffer (0-based) of logical buffer `buf`. */
static unsigned phys_of(const wwsjm_set* s, unsigned buf)
{
    return (buf - s->first_buffer + s->start) % s->count;
}

static int buffer_ok(const wwsjm_set* s, unsigned buf)
{
    return s->reserved && buf >= s->first_buffer && buf < (unsigned)s->first_buffer + s->count;
}

static wwsjm_status fail(wwsjm_plan* out, unsigned* bad, unsigned slot, wwsjm_status st)
{
    memset(out, 0, sizeof *out);
    if (bad)
        *bad = slot;
    return st;
}

static int add_load(wwsjm_plan* p, unsigned use, unsigned fill, unsigned share, uint32_t offset,
                    uint32_t ls, uint32_t ea, uint32_t bytes, uint32_t limit)
{
    if (p->load_count >= WWSJM_MAX_LOADS)
        return 0;
    wwsjm_load* l = &p->load[p->load_count++];
    l->use = (uint8_t)use;
    l->fill = (uint8_t)fill;
    l->shareable = (uint8_t)share;
    l->offset = offset;
    l->ls = ls;
    l->ea = ea;
    l->bytes = bytes;
    l->limit = limit;
    return 1;
}

WWSJM_API wwsjm_status wwsjm_plan_commands(const uint8_t* record, uint32_t size, uint32_t ls_low,
                                 uint32_t ls_high, wwsjm_plan* out, unsigned* bad_slot)
{
    memset(out, 0, sizeof *out);
    if (bad_slot)
        *bad_slot = 0;
    if (size > WWSJM_MAX_PORTION || size % 16)
        return fail(out, bad_slot, 0, WWSJM_E_HEADER);
    unsigned slots = size / 8u, run_at = slots;
    for (unsigned i = 0; i < slots && run_at == slots; i++) {
        const uint8_t* c = record + 8u * i;
        cmd_word w = split_word(wwsjm_be32(c));
        uint32_t arg = wwsjm_be32(c + 4);
        if (w.pad || w.cont || w.wid_flag)
            return fail(out, bad_slot, i, WWSJM_E_FLAGS);
        switch (w.cmd) {
        case WWSJM_CMD_RESERVE: {
            if (w.read || w.gather || w.share || w.quads || w.buf)
                return fail(out, bad_slot, i, WWSJM_E_FLAGS);
            wwsjm_set* s = &out->set[w.set];
            if (s->reserved)
                return fail(out, bad_slot, i, WWSJM_E_SET);
            if (arg & 0x8000u)
                return fail(out, bad_slot, i, WWSJM_E_COMMAND);
            s->reserved = 1;
            s->first_page = (uint8_t)(arg >> 24);
            s->pages = (uint8_t)(arg >> 16);
            s->count = (uint8_t)((arg >> 8) & 0x7F);
            s->first_buffer = (uint8_t)arg;
            if (w.set == 0 && s->first_page != WWSJM_MODULE_PAGE)
                out->set0_moved = 1;
            uint32_t lo = set_lo(s), hi = set_hi(s);
            if (!s->count || s->count > 32 || lo < ls_low ||
                hi > ls_high || (hi == lo && lo >= WWSJM_LS_BYTES))
                return fail(out, bad_slot, i, WWSJM_E_RANGE);
            for (unsigned k = 0; k < WWSJM_MAX_SETS; k++) {
                const wwsjm_set* o = &out->set[k];
                if (k == w.set || !o->reserved)
                    continue;
                if (lo < set_hi(o) && set_lo(o) < hi)
                    return fail(out, bad_slot, i, WWSJM_E_OVERLAP);
            }
            break;
        }
        case WWSJM_CMD_USE: {
            wwsjm_set* s = &out->set[w.set];
            if (!buffer_ok(s, w.buf) || (s->named >> (w.buf - s->first_buffer) & 1))
                return fail(out, bad_slot, i, WWSJM_E_SET);
            if (out->use_count >= WWSJM_MAX_USES)
                return fail(out, bad_slot, i, WWSJM_E_TOO_MANY);
            s->named |= 1u << (w.buf - s->first_buffer);
            unsigned ui = out->use_count++;
            wwsjm_use* u = &out->use[ui];
            u->set = (uint8_t)w.set;
            u->buffer = (uint8_t)w.buf;
            u->shareable = (uint8_t)w.share;
            uint32_t room = buffer_bytes(s);
            uint32_t ls = phys_lo(s, phys_of(s, w.buf));
            if (!w.read) {
                if (w.gather || w.share)
                    return fail(out, bad_slot, i, WWSJM_E_FLAGS);
                u->fill = WWSJM_FILL_NONE;
                break;
            }
            if (w.gather && w.share)          /* not part of the contract */
                return fail(out, bad_slot, i, WWSJM_E_FLAGS);
            uint32_t bytes = w.quads * 16u;
            if (!bytes || arg & 15)
                return fail(out, bad_slot, i, WWSJM_E_LENGTH);
            u->ea = arg;
            u->bytes = bytes;
            if (w.gather) {
                u->fill = WWSJM_FILL_GATHER;
                if (!add_load(out, ui, WWSJM_FILL_GATHER, 0, 0, ls, arg, bytes, room))
                    return fail(out, bad_slot, i, WWSJM_E_TOO_MANY);
                break;
            }
            if (bytes > room)
                return fail(out, bad_slot, i, WWSJM_E_LENGTH);
            u->fill = WWSJM_FILL_COPY;
            for (uint32_t done = 0; done < bytes; done += WWSJM_CHUNK_BYTES) {
                uint32_t n = bytes - done < WWSJM_CHUNK_BYTES ? bytes - done : WWSJM_CHUNK_BYTES;
                if (!add_load(out, ui, WWSJM_FILL_COPY, w.share, done, ls + done, arg + done, n,
                              room - done))
                    return fail(out, bad_slot, i, WWSJM_E_TOO_MANY);
            }
            break;
        }
        case WWSJM_CMD_UNRESERVE:
            if (w.read || w.gather || w.share || w.quads)
                return fail(out, bad_slot, i, WWSJM_E_FLAGS);
            out->unreserve_mask |= arg;
            break;
        case WWSJM_CMD_DEPEND:
            if (w.read || w.gather || w.share || w.quads || (arg & 1))
                return fail(out, bad_slot, i, WWSJM_E_FLAGS);
            if (out->dep_count >= WWSJM_MAX_DEPS)
                return fail(out, bad_slot, i, WWSJM_E_TOO_MANY);
            out->dep_ea[out->dep_count++] = arg;
            break;
        case WWSJM_CMD_NOP:                    /* never observed: skipped, counted */
            out->nop_count++;
            break;
        case WWSJM_CMD_RUN:
            if (w.read || w.gather || w.share || w.quads)
                return fail(out, bad_slot, i, WWSJM_E_FLAGS);
            run_at = i;
            break;
        default:                               /* end, invalid */
            return fail(out, bad_slot, i, WWSJM_E_COMMAND);
        }
    }
    if (run_at == slots)
        return fail(out, bad_slot, slots, WWSJM_E_NO_RUN);
    out->command_bytes = 8u * (run_at + 1);
    out->userdata_offset = (out->command_bytes + 15u) & ~15u;
    out->userdata_bytes = size > out->userdata_offset ? size - out->userdata_offset : 0;
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_buffer_span(const wwsjm_plan* p, unsigned set, unsigned buf, uint32_t* ls,
                               uint32_t* bytes)
{
    if (set >= WWSJM_MAX_SETS)
        return WWSJM_E_ARG;
    const wwsjm_set* s = &p->set[set];
    if (!buffer_ok(s, buf))
        return WWSJM_E_SET;
    *bytes = buffer_bytes(s);
    *ls = phys_lo(s, phys_of(s, buf));
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_plan_held(const wwsjm_plan* p, uint32_t released, wwsjm_span* out,
                             unsigned max, unsigned* out_n)
{
    unsigned n = 0;
    *out_n = 0;
    for (unsigned k = 0; k < WWSJM_MAX_SETS; k++) {
        const wwsjm_set* s = &p->set[k];
        if (!s->reserved || !s->pages || (released >> k & 1))
            continue;
        uint32_t named = s->named ? s->named : 1u;
        for (unsigned b = 0; b < s->count; b++) {
            if (!(named >> b & 1))
                continue;
            if (n >= max)
                return WWSJM_E_TOO_MANY;
            uint32_t lo = phys_lo(s, phys_of(s, s->first_buffer + b));
            out[n].lo = lo;
            out[n].hi = lo + buffer_bytes(s);
            n++;
        }
    }
    *out_n = n;
    return WWSJM_OK;
}

static int overlaps_any(uint32_t lo, uint32_t hi, const wwsjm_span* busy, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (lo < busy[i].hi && busy[i].lo < hi)
            return 1;
    return 0;
}

/* Set 0 holds the position-dependent module at its first page, and a set with
 * a shareable copy may be reused in place by a later job: neither rotates. */
static int pinned(const wwsjm_plan* p, unsigned set)
{
    if (set == 0)
        return 1;
    for (unsigned i = 0; i < p->use_count; i++)
        if (p->use[i].set == set && p->use[i].fill == WWSJM_FILL_COPY && p->use[i].shareable)
            return 1;
    return 0;
}

WWSJM_API unsigned wwsjm_plan_place(wwsjm_plan* p, const wwsjm_span* busy, unsigned n_busy)
{
    unsigned conflicts = 0;
    for (unsigned k = 0; k < WWSJM_MAX_SETS; k++) {
        wwsjm_set* s = &p->set[k];
        if (!s->reserved)
            continue;
        uint32_t bytes = buffer_bytes(s);
        unsigned start = 0;
        for (unsigned b = 0; b < s->count && !pinned(p, k); b++)
            if (!overlaps_any(phys_lo(s, b), phys_lo(s, b) + bytes, busy, n_busy)) {
                start = b;
                break;
            }
        s->start = (uint8_t)start;
        uint32_t named = s->named ? s->named : 1u;
        for (unsigned b = 0; b < s->count; b++) {
            uint32_t lo = phys_lo(s, phys_of(s, s->first_buffer + b));
            if ((named >> b & 1) && bytes && overlaps_any(lo, lo + bytes, busy, n_busy)) {
                conflicts++;
                break;
            }
        }
    }
    wwsjm_plan_relocate(p);
    return conflicts;
}

WWSJM_API void wwsjm_plan_relocate(wwsjm_plan* p)
{
    for (unsigned i = 0; i < p->load_count; i++) {
        wwsjm_load* l = &p->load[i];
        const wwsjm_use* u = &p->use[l->use];
        wwsjm_set* s = &p->set[u->set];
        s->start = (uint8_t)(s->start % s->count);
        l->ls = phys_lo(s, phys_of(s, u->buffer)) + l->offset;
    }
}

WWSJM_API wwsjm_status wwsjm_buffer_descriptor(const wwsjm_plan* p, unsigned set, unsigned buf,
                                     uint8_t tag, uint8_t out[16])
{
    uint32_t ls, bytes;
    wwsjm_status st = wwsjm_buffer_span(p, set, buf, &ls, &bytes);
    if (st != WWSJM_OK)
        return st;
    memset(out, 0, 16);
    out[0] = (uint8_t)set;
    out[1] = (uint8_t)buf;
    out[3] = tag;
    wwsjm_put16(out + 4, (uint16_t)(ls / 4));
    wwsjm_put16(out + 6, (uint16_t)(bytes / 4));
    for (unsigned i = 0; i < p->use_count; i++) {
        const wwsjm_use* u = &p->use[i];
        if (u->set == set && u->buffer == buf && u->fill != WWSJM_FILL_NONE) {
            wwsjm_put32(out + 8, u->ea);
            wwsjm_put32(out + 12, u->bytes);
        }
    }
    return WWSJM_OK;
}

WWSJM_API wwsjm_status wwsjm_gather_layout(const uint8_t* list, unsigned n, uint32_t ls, uint32_t limit,
                                 wwsjm_piece* out, unsigned max_out, unsigned* out_n)
{
    uint32_t at = 0;                           /* quadword-aligned offset of the next element */
    unsigned k = 0;
    *out_n = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t word = wwsjm_be32(list + 8u * i);
        uint32_t ea = wwsjm_be32(list + 8u * i + 4);
        if (word & 0xFFFF8000u)                /* stall-and-notify or reserved bits */
            return WWSJM_E_GATHER;
        if (!word)
            continue;
        if (word > WWSJM_CHUNK_BYTES)
            return WWSJM_E_GATHER;
        uint32_t end = at + (ea & 15u) + word; /* the element keeps its EA's quadword offset */
        if (end > limit)
            return WWSJM_E_GATHER;
        if (k >= max_out)
            return WWSJM_E_TOO_MANY;
        out[k].ls = ls + at + (ea & 15u);
        out[k].ea = ea;
        out[k].bytes = word;
        k++;
        at = (end + 15u) & ~15u;
    }
    *out_n = k;
    return WWSJM_OK;
}

/* ---- residency ------------------------------------------------------------- */

WWSJM_API void wwsjm_residency_reset(wwsjm_residency* r) { r->n = 0; }

WWSJM_API int wwsjm_residency_holds(const wwsjm_residency* r, uint32_t ls, uint32_t bytes, uint32_t ea)
{
    for (unsigned i = 0; i < r->n; i++)
        if (r->e[i].ls == ls && r->e[i].bytes == bytes && r->e[i].ea == ea)
            return 1;
    return 0;
}

WWSJM_API void wwsjm_residency_clobber(wwsjm_residency* r, uint32_t ls, uint32_t bytes)
{
    unsigned keep = 0;
    for (unsigned i = 0; i < r->n; i++) {
        int hit = ls < r->e[i].ls + r->e[i].bytes && r->e[i].ls < ls + bytes;
        if (!hit)
            r->e[keep++] = r->e[i];
    }
    r->n = keep;
}

WWSJM_API void wwsjm_residency_note(wwsjm_residency* r, uint32_t ls, uint32_t bytes, uint32_t ea)
{
    wwsjm_residency_clobber(r, ls, bytes);
    if (r->n == WWSJM_RESIDENT_MAX) {          /* forget the oldest */
        memmove(&r->e[0], &r->e[1], (WWSJM_RESIDENT_MAX - 1) * sizeof r->e[0]);
        r->n--;
    }
    r->e[r->n].ls = ls;
    r->e[r->n].bytes = bytes;
    r->e[r->n].ea = ea;
    r->n++;
}

WWSJM_API void wwsjm_plan_residency(wwsjm_plan* p, wwsjm_residency* r)
{
    /* A set is resident when every use of it is a shareable copy that was the
     * last thing loaded at the same place from the same source. */
    for (unsigned k = 0; k < WWSJM_MAX_SETS; k++) {
        const wwsjm_set* s = &p->set[k];
        if (!s->reserved)
            continue;
        int uses = 0, resident = 1;
        for (unsigned i = 0; i < p->use_count; i++) {
            const wwsjm_use* u = &p->use[i];
            if (u->set != k)
                continue;
            uses++;
            uint32_t ls, bytes;
            wwsjm_buffer_span(p, k, u->buffer, &ls, &bytes);
            if (u->fill != WWSJM_FILL_COPY || !u->shareable ||
                !wwsjm_residency_holds(r, ls, u->bytes, u->ea))
                resident = 0;
        }
        resident = resident && uses;
        for (unsigned i = 0; i < p->load_count; i++)
            if (p->use[p->load[i].use].set == k)
                p->load[i].skip = (uint8_t)resident;
        if (resident)
            continue;
        /* Anything else placed in the set's pages (a copy, a gather, or a buffer
         * the job writes) replaces what was resident there. */
        wwsjm_residency_clobber(r, set_lo(s), set_hi(s) - set_lo(s));
        for (unsigned i = 0; i < p->use_count; i++) {
            const wwsjm_use* u = &p->use[i];
            uint32_t ls, bytes;
            if (u->set == k && u->fill == WWSJM_FILL_COPY && u->shareable &&
                wwsjm_buffer_span(p, k, u->buffer, &ls, &bytes) == WWSJM_OK)
                wwsjm_residency_note(r, ls, u->bytes, u->ea);
        }
    }
}

/* ---- context ----------------------------------------------------------------- */

WWSJM_API void wwsjm_context_build(const wwsjm_context_args* a, uint8_t out[WWSJM_CONTEXT_BYTES])
{
    memset(out, 0, WWSJM_CONTEXT_BYTES);
    wwsjm_put32(out + 0x00, a->lane);
    wwsjm_put32(out + 0x04, a->dma_tag);
    wwsjm_put32(out + 0x0C, a->spurs_ea);
    wwsjm_put32(out + 0x10, (uint32_t)a->wid << 16 | a->index);
    wwsjm_put32(out + 0x14, a->dep_count);
    wwsjm_put32(out + 0x18, (a->list_ea + a->list_tag) | a->array);
    wwsjm_put32(out + 0x1C, (uint32_t)a->wid << 16 | 0xFFFFu);
    for (unsigned i = 0; i < 4; i++)
        wwsjm_put32(out + 0x20 + 4 * i, a->userdata_ls);
    for (unsigned i = 0; i < WWSJM_SLOTS; i++)
        wwsjm_put32(out + 0x30 + 4 * i, a->slot[i]);
}

WWSJM_API void wwsjm_finish_result(const wwsjm_header* claimed, uint32_t out[2])
{
    if (!claimed) {
        out[0] = 0;
        out[1] = 0xFFFFFFFFu;
        return;
    }
    out[0] = (uint32_t)claimed->size << 16 | claimed->type;
    out[1] = claimed->ea;
}
