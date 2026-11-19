/* Native WWS job manager: the pure core.
 *
 * Written from the behavioural specification of the job manager's guest-visible
 * contract (job list line, entry headers, job records, the job API context and
 * the SPURS ready counts); see docs/SPURS_NATIVE_WWSJM.md. It is not derived
 * from the original policy module's code.
 *
 * Everything here is a pure function over byte buffers: no spu_context, no MFC,
 * no guest memory, no globals. The run loop (a later phase) reads guest memory,
 * asks the core what to do and what the new bytes are, and performs the atomic
 * or the transfer itself. Guest data is big-endian; the core has its own
 * accessors. All transactions copy the old line and change only the fields the
 * contract names, so every other byte is preserved by construction.
 */
#ifndef WWSJM_CORE_H
#define WWSJM_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linkage of the core's functions: external by default; a user that compiles
 * the core into its own translation unit (the run loop does) defines it as
 * `static inline` first, so a separately built core object never clashes. */
#ifndef WWSJM_API
#define WWSJM_API
#endif

#define WWSJM_LINE_BYTES     128u
#define WWSJM_ARRAYS         4u
#define WWSJM_LANES          6u
#define WWSJM_NO_MARKER      0xFFFFu
#define WWSJM_HEADER_BYTES   8u
#define WWSJM_PAGE_BYTES     1024u
#define WWSJM_MAX_COMMANDS   32u      /* command slots in a record */
#define WWSJM_MAX_PORTION    0x100u   /* bytes of commands + parameter data */
#define WWSJM_MAX_SETS       16u
#define WWSJM_MAX_DEPS       8u
#define WWSJM_MAX_LOADS      64u
#define WWSJM_MAX_USES       32u
#define WWSJM_CHUNK_BYTES    0x4000u  /* largest single GET of a staged copy */
#define WWSJM_CONTEXT_BYTES  0x80u
#define WWSJM_RESIDENT_MAX   8u
#define WWSJM_MODULE_PAGE    16u      /* modules are linked for LS 0x4000 */
#define WWSJM_LS_BYTES       0x40000u /* local store */
/* N is a u16; capacities need not be powers of two, so the slot jumps when N
 * wraps. The wrap was never observed; a check mode reports N at or above this. */
#define WWSJM_INDEX_WRAP_WATCH 0xF000u

typedef enum {
    WWSJM_OK = 0,
    WWSJM_E_ARG,            /* caller error: lane, array, wid out of range */
    WWSJM_E_DETACHED,       /* the current array has no guest address or no capacity */
    WWSJM_E_ENTRY_TYPE,     /* header entry type > 5 */
    WWSJM_E_HEADER,         /* header size nibble, size or EA alignment */
    WWSJM_E_COMMAND,        /* command 6/7 in a command portion, or a reserve pad bit */
    WWSJM_E_FLAGS,          /* flag combination outside the contract */
    WWSJM_E_SET,            /* use of an unreserved set/buffer, double reserve */
    WWSJM_E_RANGE,          /* reserve outside the allowed LS window */
    WWSJM_E_OVERLAP,        /* two reserved sets of one job overlap */
    WWSJM_E_LENGTH,         /* a load longer than its buffer */
    WWSJM_E_NO_RUN,         /* no run-job command inside the portion */
    WWSJM_E_GATHER,         /* gather element flagged or overflowing the set */
    WWSJM_E_SERVICE,        /* get-buffer for a set with no defined tag */
    WWSJM_E_TOO_MANY        /* more dependencies/loads than the plan holds */
} wwsjm_status;

WWSJM_API const char* wwsjm_status_name(wwsjm_status s);

/* ---- big-endian accessors ---------------------------------------------- */
static inline uint16_t wwsjm_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t wwsjm_be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void wwsjm_put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wwsjm_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* ---- the job list line -------------------------------------------------- */
typedef struct {
    uint16_t submitted;              /* S */
    uint16_t next;                   /* N: the next index to claim */
    uint16_t lane[WWSJM_LANES];      /* oldest held type-1 index per lane */
} wwsjm_markers;

typedef struct {
    uint32_t image_ea, image_size;
    uint16_t current;                /* the array the manager services */
    uint32_t switch_count;           /* +0x0C: decremented (u32) by every array switch */
    uint32_t mask[WWSJM_ARRAYS];     /* capacity - 1 */
    uint32_t array_ea[WWSJM_ARRAYS];
    wwsjm_markers m[WWSJM_ARRAYS];
} wwsjm_line;

WWSJM_API void wwsjm_line_decode(const uint8_t raw[WWSJM_LINE_BYTES], wwsjm_line* out);

/* Where the header of `index` on array `a` lives: the u16 index mod capacity
 * (capacity - 1 is the line's field), with no special case at the u16 wrap.
 * 0 for a malformed array (no EA, or capacity 2^32). */
WWSJM_API uint32_t wwsjm_header_ea(const wwsjm_line* l, unsigned a, uint16_t index);

/* The entry the manager would examine next: array `current`, index N. */
typedef struct {
    unsigned array;
    uint16_t index;
    uint32_t header_ea;
} wwsjm_cursor;

WWSJM_API wwsjm_status wwsjm_cursor_of(const wwsjm_line* l, wwsjm_cursor* out);

/* ---- headers and the claim decision ------------------------------------ */
enum {
    WWSJM_ENTRY_EMPTY = 0,     /* nothing published yet */
    WWSJM_ENTRY_JOB = 1,       /* EA = job record */
    WWSJM_ENTRY_GATE = 2,      /* EA = u16 counter */
    WWSJM_ENTRY_NOP = 3,
    WWSJM_ENTRY_SWITCH = 4,    /* advance to the next array */
    WWSJM_ENTRY_REPORT = 5     /* EA = u16 inside a report; a gate */
};

typedef struct {
    uint16_t size;             /* bytes of commands + parameter data */
    uint16_t type;
    uint32_t ea;
} wwsjm_header;

WWSJM_API wwsjm_status wwsjm_header_decode(const uint8_t raw[WWSJM_HEADER_BYTES], wwsjm_header* out);

/* Types 2 and 5 depend on the u16 at their EA. */
static inline int wwsjm_header_is_gate(const wwsjm_header* h)
{
    return h->type == WWSJM_ENTRY_GATE || h->type == WWSJM_ENTRY_REPORT;
}

typedef enum {
    WWSJM_STOP_EMPTY = 0,      /* type 0: stop, nothing to claim */
    WWSJM_STOP_BLOCKED,        /* a gate whose counter is non-zero */
    WWSJM_TAKE                 /* claimable now */
} wwsjm_verdict;

/* `gate_value` is read only for gates. */
WWSJM_API wwsjm_verdict wwsjm_judge(const wwsjm_header* h, uint16_t gate_value);

/* Claim the entry at the cursor of `old` for lane `lane`. Publishes N+1, raises
 * S to N+1 when it was behind, records the lane's oldest held job for a type-1
 * entry, and advances the current array for a type-4 entry. */
WWSJM_API wwsjm_status wwsjm_claim(const uint8_t old[WWSJM_LINE_BYTES], unsigned lane,
                         const wwsjm_header* h, uint8_t out[WWSJM_LINE_BYTES]);

/* A job the lane still holds after the retiring one (the one claimed during
 * finish), or none. */
typedef struct {
    int valid;
    unsigned array;
    uint16_t index;
} wwsjm_held;

/* Publish the completion of the job claimed on array `array` by lane `lane`:
 * the lane's marker on that array becomes the next held job there, or none. */
WWSJM_API wwsjm_status wwsjm_retire(const uint8_t old[WWSJM_LINE_BYTES], unsigned array, unsigned lane,
                          const wwsjm_held* next, uint8_t out[WWSJM_LINE_BYTES]);

/* ---- SPURS ready counts -------------------------------------------------- */
/* The value the manager publishes for its own workload: `busy` while entries
 * are counted beyond N and the entry at N is claimable, else 0. */
WWSJM_API uint8_t wwsjm_ready_value(const wwsjm_line* l, wwsjm_verdict at_next, uint8_t busy);

/* Instance-line write: byte `wid` of the first 16 bytes becomes `value`. */
WWSJM_API wwsjm_status wwsjm_ready_apply(const uint8_t old[WWSJM_LINE_BYTES], unsigned wid, uint8_t value,
                               uint8_t out[WWSJM_LINE_BYTES]);

#define WWSJM_READY_ON_FLAG 8u

/* ---- dependency counters ------------------------------------------------- */
/* Decrement the u16 at byte `offset` of a 128-byte line (wrapping) into `out`
 * and `*value`. An odd offset or one past the line is refused. */
WWSJM_API wwsjm_status wwsjm_decrement(const uint8_t old[WWSJM_LINE_BYTES], unsigned offset,
                             uint8_t out[WWSJM_LINE_BYTES], uint16_t* value);

/* The u16 after a counter names the workload to wake and its ready count. */
static inline void wwsjm_wake_target(uint16_t follower, unsigned* wid, uint8_t* value)
{
    *wid = follower >> 8;
    *value = (uint8_t)follower;
}

/* ---- the command portion of a job record --------------------------------- */
enum {
    WWSJM_CMD_NOP = 0, WWSJM_CMD_RESERVE = 1, WWSJM_CMD_USE = 2, WWSJM_CMD_UNRESERVE = 3,
    WWSJM_CMD_DEPEND = 4, WWSJM_CMD_RUN = 5, WWSJM_CMD_END = 6, WWSJM_CMD_INVALID = 7
};

typedef enum {
    WWSJM_FILL_NONE = 0,       /* uninitialised buffer */
    WWSJM_FILL_COPY,           /* one contiguous copy from an EA */
    WWSJM_FILL_GATHER          /* a (size, EA) list in guest memory */
} wwsjm_fill;

/* A reserved buffer set: `count` buffers of `pages` KiB from `first_page`,
 * numbered from `first_buffer`. Its buffers form a ring: logical buffer b
 * occupies physical buffer (b - first_buffer + start) mod count. */
typedef struct {
    uint8_t reserved;
    uint8_t first_page, pages, count, first_buffer;
    uint8_t start;             /* ring rotation chosen by wwsjm_plan_place */
    uint32_t named;            /* logical buffers named by use commands (bit b - first_buffer) */
} wwsjm_set;

/* One use command: fills logical buffer `buffer` of set `set`. */
typedef struct {
    uint8_t set, buffer;
    uint8_t fill;              /* wwsjm_fill */
    uint8_t shareable;
    uint32_t ea;               /* copy source or gather list EA */
    uint32_t bytes;            /* copy length or gather list length */
} wwsjm_use;

/* One staged GET (a chunk of a copy) or one gather to perform. */
typedef struct {
    uint8_t use;               /* index into wwsjm_plan.use */
    uint8_t fill;              /* COPY or GATHER */
    uint8_t skip;              /* resident: no transfer needed */
    uint8_t shareable;
    uint32_t offset;           /* within the buffer */
    uint32_t ls;               /* follows the placement */
    uint32_t ea;               /* copy source or gather list EA */
    uint32_t bytes;            /* copy length or gather list bytes */
    uint32_t limit;            /* bytes available at ls */
} wwsjm_load;

typedef struct {
    wwsjm_set set[WWSJM_MAX_SETS];
    unsigned use_count;
    wwsjm_use use[WWSJM_MAX_USES];
    uint32_t unreserve_mask;
    unsigned dep_count;
    uint32_t dep_ea[WWSJM_MAX_DEPS];
    unsigned load_count;
    wwsjm_load load[WWSJM_MAX_LOADS];
    unsigned nop_count;        /* no-op commands skipped (never observed; check reports) */
    uint8_t set0_moved;        /* set 0 not at WWSJM_MODULE_PAGE (allowed; check reports) */
    uint32_t command_bytes;    /* through the run-job command */
    uint32_t userdata_offset;  /* parameter data: next 16-byte boundary */
    uint32_t userdata_bytes;
} wwsjm_plan;

/* Decode the first `size` bytes of a record into a staging plan. No-op
 * commands are skipped and counted; end and invalid commands are refused. Every
 * reserved set must lie inside [ls_low, ls_high); a set of zero pages only
 * needs its start inside local store. Nothing partial is returned: on error the plan is
 * cleared and `*bad_slot` names the offending command. */
WWSJM_API wwsjm_status wwsjm_plan_commands(const uint8_t* record, uint32_t size, uint32_t ls_low,
                                 uint32_t ls_high, wwsjm_plan* out, unsigned* bad_slot);

/* An LS range [lo, hi) that is not free. */
typedef struct {
    uint32_t lo, hi;
} wwsjm_span;

/* The LS a job still holds: every named buffer (logical buffer `first_buffer`
 * when none is named) of every set it reserved and has not released (bit s of
 * `released` = set s released). Zero-length sets hold nothing. */
WWSJM_API wwsjm_status wwsjm_plan_held(const wwsjm_plan* p, uint32_t released, wwsjm_span* out,
                             unsigned max, unsigned* out_n);

/* Place a job's sets beside LS that is not free (the buffers an earlier job
 * still holds): each set's ring starts at its lowest-numbered physical buffer
 * that overlaps none of `busy`, or at its first buffer when every buffer does.
 * Returns the number of sets whose placed buffers still overlap `busy`: those
 * must not be filled while the earlier job runs (0 = stage now). Loads and
 * descriptors follow the placement. With no busy spans every ring starts at
 * its first buffer. Set 0 (the module) and any set holding a shareable copy
 * never rotate: their buffers stay put and only count as conflicts. */
WWSJM_API unsigned wwsjm_plan_place(wwsjm_plan* p, const wwsjm_span* busy, unsigned n_busy);

/* Recompute every load's LS from the sets' `start` (for a caller that chooses
 * the ring starts itself). */
WWSJM_API void wwsjm_plan_relocate(wwsjm_plan* p);

/* LS address and length of logical buffer `buf` of a reserved set, after
 * placement. */
WWSJM_API wwsjm_status wwsjm_buffer_span(const wwsjm_plan* p, unsigned set, unsigned buf, uint32_t* ls,
                               uint32_t* bytes);

/* The get-buffer result for logical buffer `buf` of `set`: set, buffer, DMA
 * `tag` (0 when the job did not ask for one), LS/4, length/4, and for a buffer
 * filled by a read (copy or gather) the use command's EA and length in bytes. */
WWSJM_API wwsjm_status wwsjm_buffer_descriptor(const wwsjm_plan* p, unsigned set, unsigned buf,
                                     uint8_t tag, uint8_t out[16]);

/* DMA tags handed to one job: each get-buffer that asks for a tag receives the
 * next one, from 31 downwards. Reset per job. */
#define WWSJM_JOB_TAG_FIRST 31u
#define WWSJM_JOB_TAG_COUNT 16u
static inline wwsjm_status wwsjm_job_tag(unsigned* given, uint8_t* tag)
{
    if (*given >= WWSJM_JOB_TAG_COUNT)
        return WWSJM_E_SERVICE;
    *tag = (uint8_t)(WWSJM_JOB_TAG_FIRST - (*given)++);
    return WWSJM_OK;
}

/* Where each element of a gather list lands (the MFC list rule: each element
 * starts on the next quadword after the previous element's last byte, keeping
 * its EA's offset within the quadword). `list` holds `n` big-endian (size, EA)
 * pairs; an element is at most 16 KiB; every byte must fit in `limit`; at most
 * `max_out` pieces are written. */
typedef struct {
    uint32_t ls, ea, bytes;
} wwsjm_piece;

WWSJM_API wwsjm_status wwsjm_gather_layout(const uint8_t* list, unsigned n, uint32_t ls, uint32_t limit,
                                 wwsjm_piece* out, unsigned max_out, unsigned* out_n);

/* ---- module/buffer residency within one run ------------------------------ */
typedef struct {
    unsigned n;
    struct { uint32_t ls, bytes, ea; } e[WWSJM_RESIDENT_MAX];
} wwsjm_residency;

WWSJM_API void wwsjm_residency_reset(wwsjm_residency* r);
WWSJM_API int  wwsjm_residency_holds(const wwsjm_residency* r, uint32_t ls, uint32_t bytes, uint32_t ea);
/* LS [ls, ls+bytes) now holds something else. */
WWSJM_API void wwsjm_residency_clobber(wwsjm_residency* r, uint32_t ls, uint32_t bytes);
WWSJM_API void wwsjm_residency_note(wwsjm_residency* r, uint32_t ls, uint32_t bytes, uint32_t ea);

/* Call after placement. Marks each shareable copy that is already resident
 * (same LS, same source, same length) as skipped; everything else the job
 * reserves replaces what was resident there. */
WWSJM_API void wwsjm_plan_residency(wwsjm_plan* p, wwsjm_residency* r);

/* ---- the job API context --------------------------------------------------- */
enum {
    WWSJM_SLOT_30 = 0, WWSJM_SLOT_GET_BUFFER, WWSJM_SLOT_38, WWSJM_SLOT_3C, WWSJM_SLOT_40,
    WWSJM_SLOT_FINISH, WWSJM_SLOT_RELEASE, WWSJM_SLOTS
};

typedef struct {
    unsigned lane;
    uint32_t dma_tag;
    uint32_t spurs_ea;
    unsigned wid;
    uint16_t index;
    unsigned array;
    uint32_t dep_count;
    uint32_t list_ea;
    uint32_t list_tag;         /* the per-instance constant added to the list EA */
    uint32_t userdata_ls;
    uint32_t slot[WWSJM_SLOTS];
} wwsjm_context_args;

WWSJM_API void wwsjm_context_build(const wwsjm_context_args* a, uint8_t out[WWSJM_CONTEXT_BYTES]);

/* Finish's result: the claimed job's header words, or {0, 0xFFFFFFFF}. */
WWSJM_API void wwsjm_finish_result(const wwsjm_header* claimed, uint32_t out[2]);

#ifdef __cplusplus
}
#endif
#endif
