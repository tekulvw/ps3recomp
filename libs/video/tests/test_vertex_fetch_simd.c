/*
 * PS3RECOMP_RSX_VERTEX_SIMD against the scalar stable-range loop.
 *
 * Random fetch plans -- every vertex type, every component count, strides at
 * and above the element size, frequency dividers in both modes, INLINE_ARRAY
 * streams, guest memory that is partly unmapped -- over random references,
 * fetched once with the switch off and once with it on: the same result and
 * the same bytes, bit for bit, and the same as rsx_vertex_fetch_one. The
 * guest memory is seeded with every half-float special (signed zeros,
 * denormals, infinities, NaNs with payloads) and SNORM16 -32768, and each
 * guest location ends at a PROT_NONE page: an element whose last byte is the
 * last mapped byte must still fetch, so a vector load that reads past the
 * element faults the fixture.
 *
 * Build (from the repo root):
 *   cc -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch_simd libs/video/tests/test_vertex_fetch_simd.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 * tools/test_vertex_fetch_simd.py builds and runs it, and its mutants.
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 16 };
static u8* guest[2];                    /* two locations, each followed by a guard page */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

static u8* guarded(void)
{
    const long page = sysconf(_SC_PAGESIZE);
    const size_t bytes = ((GUEST + page - 1) / page) * page;
    u8* p = mmap(NULL, bytes + (size_t)page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    if (mprotect(p + bytes, (size_t)page, PROT_NONE)) { perror("mprotect"); exit(2); }
    /* GUEST is a multiple of the page size on every supported host; keep the
     * guard right after the last guest byte regardless. */
    return p + (bytes - GUEST);
}

/* Big-endian 16-bit specials planted through the guest memory: every half
 * class, and the SNORM16/SINT16 minimum. */
static const u16 specials[] = {
    0x0000, 0x8000,                     /* signed zeros */
    0x0001, 0x8001, 0x03FF, 0x83FF,     /* denormals */
    0x0400, 0x8400,                     /* smallest normals */
    0x3C00, 0xBC00, 0x7BFF, 0xFBFF,     /* one, largest normals */
    0x7C00, 0xFC00,                     /* infinities */
    0x7C01, 0xFC01, 0x7E00, 0x7FFF, 0xFFFF, /* NaNs with payloads */
};

static void seed_guest(void)
{
    for (int l = 0; l < 2; l++) {
        for (u32 i = 0; i < GUEST; i++) guest[l][i] = (u8)rnd();
        for (u32 i = 0; i + 1 < GUEST; i += 2)
            if (rnd() % 6 == 0) {
                const u16 s = specials[rnd() % (sizeof specials / sizeof specials[0])];
                guest[l][i] = (u8)(s >> 8); guest[l][i + 1] = (u8)s;
            }
    }
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline, const u8* inl, u32 inl_bytes)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 4 == 0) ? rnd() % 256u : 0;
    plan->divider_mask = rnd() & 0xFFFFu;
    plan->guest_ptr = guest_ptr;
    u32 inline_packed = 0;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        const u32 r = rnd() % 16;
        f->desc.type = r < 1 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 12 == 0) ? rnd() % 6 : 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 4 == 0) ? 0 : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride ? f->desc.stride : f->elem_size;
        f->desc.frequency = (rnd() % 4 == 0) ? rnd() % 4 : 0;
        f->desc.offset = rnd() % 4096u;
        f->desc.location = rnd() & 1u;
    }
    if (use_inline) {
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
            plan->inline_off[a] = inline_packed;
            if (plan->attr[a].desc.type && plan->attr[a].desc.size)
                inline_packed += plan->attr[a].elem_size;
        }
        plan->inline_stride = inline_packed ? inline_packed : 16u;
        plan->inline_data = inl;
        plan->inline_bytes = inl_bytes;
    }
}

/* A plan whose every enabled attribute has its last referenced element end on
 * the last mapped byte of its location: refs 0..count-1 at base 0, frequency
 * 0, so the element span is exactly [0, count-1]. */
static void edge_plan(rsx_vertex_fetch_plan* plan, u32 count)
{
    memset(plan, 0, sizeof *plan);
    rsx_vertex_layout_plan_init(&plan->layout, rnd() & 0xFFFFu);
    plan->base_offset = (rnd() % 2) ? rnd() % 64u : 0;
    plan->guest_ptr = guest_ptr;
    for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {
        rsx_vertex_fetch_attr* f = &plan->attr[a];
        f->desc.type = 1 + rnd() % 7;
        f->desc.size = 1 + rnd() % 4;
        f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
        f->desc.stride = (rnd() % 2) ? f->elem_size : f->elem_size + (rnd() % 3) * 4u;
        f->stride = f->desc.stride;
        f->desc.frequency = 0;
        f->desc.location = rnd() & 1u;
        const u64 span = (u64)(count - 1) * f->stride + f->elem_size;
        f->desc.offset = (u32)(GUEST - span) - plan->base_offset;
    }
}

enum { MAXV = 200 };
static rsx_vertex_ref refs[MAXV];
static u8 one[MAXV * 256], scalar[MAXV * 256], simd[MAXV * 256];

static int simd_paths, by_type[8], by_n[5], specials_in_path[3];

/* Count what the vector loop will see, and which planted halves it decodes. */
static void note_coverage(const rsx_vertex_fetch_plan* plan, u32 count)
{
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
        if (!f->stable_base || !f->desc.type || !f->desc.size || f->desc.type == RSX_VTX_TYPE_CMP32) continue;
        simd_paths++;
        by_type[f->desc.type]++;
        by_n[f->desc.size < 4 ? f->desc.size : 4]++;
        if (f->desc.type != RSX_VTX_TYPE_HALF) continue;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,
                                                         f->desc.frequency, (plan->divider_mask >> plan->layout.attrs[slot]) & 1u);
            if (element < f->stable_first || element > f->stable_last) continue;
            const u8* p = f->stable_base + (u64)(element - f->stable_first) * f->stride;
            for (u32 c = 0; c < f->desc.size && c < 4; c++) {
                const u16 h = (u16)((p[2 * c] << 8) | p[2 * c + 1]);
                const u32 exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
                if (exp == 0 && man) specials_in_path[0]++;
                else if (exp == 31 && !man) specials_in_path[1]++;
                else if (exp == 31) specials_in_path[2]++;
            }
        }
    }
}

static void compare(const rsx_vertex_fetch_plan* plan, u32 count, int n, const char* kind)
{
    const u32 stride = plan->layout.stride;
    memset(one, 0xCD, sizeof one); memset(scalar, 0xCD, sizeof scalar); memset(simd, 0xCD, sizeof simd);
    int ok_one = 1;
    for (u32 i = 0; i < count && ok_one; i++)
        ok_one = rsx_vertex_fetch_one(plan, &refs[i], one + (size_t)i * stride);
    rsx_vertex_fetch_simd_override(0);
    const int ok_scalar = rsx_vertex_fetch_many(plan, refs, count, scalar);
    rsx_vertex_fetch_simd_override(1);
    const int ok_simd = rsx_vertex_fetch_many(plan, refs, count, simd);
    CHECK(ok_scalar == ok_one, "%s plan %d: scalar fetch_many %d, fetch_one %d", kind, n, ok_scalar, ok_one);
    CHECK(ok_simd == ok_scalar, "%s plan %d: simd fetch_many %d, scalar %d", kind, n, ok_simd, ok_scalar);
    if (!ok_scalar || !ok_simd) return;
    CHECK(!memcmp(one, scalar, (size_t)count * stride), "%s plan %d: scalar vertices differ from fetch_one", kind, n);
    if (memcmp(scalar, simd, (size_t)count * stride)) {
        /* Name the first differing component. */
        for (u32 i = 0; i < count; i++)
            for (u32 slot = 0; slot < plan->layout.count; slot++) {
                const u8* a = scalar + (size_t)i * stride + slot * 16u;
                const u8* b = simd + (size_t)i * stride + slot * 16u;
                if (!memcmp(a, b, 16)) continue;
                const rsx_vertex_fetch_attr* f = &plan->attr[plan->layout.attrs[slot]];
                u32 wa[4], wb[4]; memcpy(wa, a, 16); memcpy(wb, b, 16);
                CHECK(0, "%s plan %d vertex %u slot %u (type %u size %u stride %u): scalar %08X %08X %08X %08X, simd %08X %08X %08X %08X",
                      kind, n, i, slot, f->desc.type, f->desc.size, f->stride,
                      wa[0], wa[1], wa[2], wa[3], wb[0], wb[1], wb[2], wb[3]);
                return;
            }
    }
}

int main(void)
{
    guest[0] = guarded(); guest[1] = guarded();
    seed_guest();
    static u8 inl[8192];
    for (u32 i = 0; i < sizeof inl; i++) inl[i] = (u8)rnd();
    for (u32 i = 0; i + 1 < sizeof inl; i += 2)
        if (rnd() % 6 == 0) { const u16 s = specials[rnd() % (sizeof specials / sizeof specials[0])];
                              inl[i] = (u8)(s >> 8); inl[i + 1] = (u8)s; }

    const int available = rsx_vertex_fetch_simd_available();
    printf("test_vertex_fetch_simd: vector path %s in this build\n", available ? "present" : "absent");

    enum { PLANS = 8000, EDGES = 2000 };
    int fetched = 0, edge_fetched = 0;
    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan plan;
        random_plan(&plan, rnd() % 5 == 0, inl, (u32)(rnd() % sizeof inl));
        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        const u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = start + ((rnd() % 8 == 0) ? rnd() : rnd() % 300u);
            refs[i].base_index = base;
        }
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        note_coverage(&plan, count);
        compare(&plan, count, n, "random");
        rsx_vertex_fetch_simd_override(0);
        if (rsx_vertex_fetch_many(&plan, refs, count, scalar)) fetched++;
    }
    /* Elements ending on the last mapped byte, in every order of reference. */
    for (int n = 0; n < EDGES; n++) {
        mapped_end[0] = mapped_end[1] = GUEST;
        const u32 count = 1 + rnd() % 64;
        rsx_vertex_fetch_plan plan;
        edge_plan(&plan, count);
        for (u32 i = 0; i < count; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }
        for (u32 i = count; i > 1; i--) {               /* shuffle */
            const u32 j = rnd() % i; const rsx_vertex_ref t = refs[i - 1]; refs[i - 1] = refs[j]; refs[j] = t; }
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        for (u32 slot = 0; slot < plan.layout.count; slot++) {
            const rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[slot]];
            CHECK(f->stable_base && f->stable_last == count - 1, "edge plan %d slot %u: no stable range", n, slot);
        }
        note_coverage(&plan, count);
        compare(&plan, count, n, "edge");
        edge_fetched++;
    }
    printf("test_vertex_fetch_simd: %d plans (%d fetched), %d edge plans, %d vector attribute passes "
           "(types s16n %d f32 %d f16 %d u8n %d s16 %d u8 %d; sizes 1:%d 2:%d 3:%d 4:%d), "
           "halves seen: %d denormal, %d inf, %d nan, %d failure(s)\n",
           PLANS, fetched, edge_fetched, simd_paths, by_type[1], by_type[2], by_type[3], by_type[4], by_type[5], by_type[7],
           by_n[1], by_n[2], by_n[3], by_n[4], specials_in_path[0], specials_in_path[1], specials_in_path[2], fails);
    int enough = fetched > PLANS / 2 && simd_paths > PLANS;
    for (int t = 1; t <= 7; t++) if (t != RSX_VTX_TYPE_CMP32 && by_type[t] < 500) enough = 0;
    for (int k = 1; k <= 4; k++) if (by_n[k] < 500) enough = 0;
    for (int k = 0; k < 3; k++) if (specials_in_path[k] < 100) enough = 0;
    if (!enough) { printf("FAIL: too few cases exercised a path\n"); return 1; }
    return fails ? 1 : 0;
}
