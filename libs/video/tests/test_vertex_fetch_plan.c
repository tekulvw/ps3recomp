/*
 * rsx_vertex_fetch_plan_prepare against the per-attribute walk it replaced.
 *
 * _prepare finds the span of ordinary per-vertex attributes (frequency 0/1)
 * once per draw instead of once per attribute. Random fetch plans -- every
 * vertex type, strides and offsets, frequency dividers in both modes, guest
 * memory that is partly unmapped, INLINE_ARRAY streams -- with random index
 * references, some past the 20-bit element wrap: every attribute's stable
 * range (base, first, last) must be what the old walk gave, and
 * rsx_vertex_fetch_many must produce exactly the bytes and the result of
 * rsx_vertex_fetch_one over the same references.
 *
 * Build (from the repo root):
 *   clang -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
 *     -o /tmp/test_vertex_fetch_plan libs/video/tests/test_vertex_fetch_plan.c \
 *     libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c
 */
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails <= 20) { \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)rng;
}

enum { GUEST = 1 << 16 };
static uint8_t guest[2][GUEST];         /* two locations */
static uint32_t mapped_end[2];          /* bytes past this are unmapped */

static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    const u32 loc = location & 1u;
    if ((u64)offset + min_bytes > mapped_end[loc]) return NULL;
    return guest[loc] + offset;
}

/* The walk _prepare used to do: every reference, once per attribute. */
static void ref_prepare(rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count || !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;
        if (plan->inline_data) {
            const u32 stride = plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }
        if (!plan->guest_ptr)
            continue;
        u32 first = UINT_MAX, last = 0;
        for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index, fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }
        const u64 prefix = (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end = prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start || end - start > UINT_MAX)
            continue;
        const u8* base = plan->guest_ptr(plan->guest_user, fetch->desc.location,
                                         (u32)start, (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

static void random_plan(rsx_vertex_fetch_plan* plan, int use_inline,
                        const u8* inl, u32 inl_bytes)
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
        f->desc.type = r < 6 ? RSX_VTX_TYPE_FLOAT : r < 7 ? 0 : 1 + rnd() % 7;
        f->desc.size = (rnd() % 8 == 0) ? rnd() % 6 : 1 + rnd() % 4;
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

/* A host GPU's conversion of each host format to float, written out: what
 * the vertex fetch would hand the program. */
static float host_half(u16 h)
{
    const u32 sign = (u32)(h >> 15) << 31, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    u32 bits;
    if (exp == 0) {
        if (!man) bits = sign;
        else { float f = (float)man / 16777216.0f; memcpy(&bits, &f, 4); bits |= sign; }
    } else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
    else bits = sign | ((exp + 112u) << 23) | (man << 13);
    float f; memcpy(&f, &bits, 4); return f;
}
static void host_decode(u8 format, const u8* p, float v[4])
{
    v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;
    switch (format) {
    case RSX_HOST_VF_F32_1: case RSX_HOST_VF_F32_2: case RSX_HOST_VF_F32_3: case RSX_HOST_VF_F32_4:
        memcpy(v, p, 4u * (u32)(format - RSX_HOST_VF_F32_1 + 1)); break;
    case RSX_HOST_VF_F16_4: for (int c = 0; c < 4; c++) { u16 h; memcpy(&h, p + 2 * c, 2); v[c] = host_half(h); } break;
    case RSX_HOST_VF_UN8_4: for (int c = 0; c < 4; c++) v[c] = p[c] / 255.0f; break;
    case RSX_HOST_VF_SN16_4: for (int c = 0; c < 4; c++) { s16 x; memcpy(&x, p + 2 * c, 2);
                                                          v[c] = x / 32767.0f; if (v[c] < -1.0f) v[c] = -1.0f; } break;
    }
}

/* rsx_vertex_host_streams_build, decoded the way a host GPU converts each
 * format, against rsx_vertex_fetch_many over the same elements: bit-equal
 * float4s, or a refusal. */
static int test_host_streams(void)
{
    enum { MAXN = 96 };
    static rsx_vertex_ref refs[MAXN];
    static u8 four[MAXN * 16 * 16], host[MAXN * 16 * 16];
    int built = 0, refused = 0, snorm_min_refused = 0;
    for (int n_plan = 0; n_plan < 4000; n_plan++) {
        mapped_end[0] = mapped_end[1] = GUEST;
        rsx_vertex_fetch_plan plan;
        random_plan(&plan, 0, NULL, 0);
        rsx_vertex_layout_plan_init(&plan.layout, (rnd() & 0x0F0Fu) | 1u);
        for (u32 a = 0; a < RSX_DSP_NUM_VERTEX_ATTR; a++) {          /* mostly host formats */
            rsx_vertex_fetch_attr* f = &plan.attr[a];
            static const u32 types[5] = { RSX_VTX_TYPE_FLOAT, RSX_VTX_TYPE_HALF, RSX_VTX_TYPE_UNORM8,
                                          RSX_VTX_TYPE_SNORM16, RSX_VTX_TYPE_CMP32 };
            f->desc.type = types[rnd() % (rnd() % 8 ? 4 : 5)];
            f->desc.size = 1 + rnd() % 4;
            if (rnd() % 16 == 0) f->desc.frequency = 2; else f->desc.frequency = 0;
            f->elem_size = rsx_vertex_attrib_size(f->desc.type, f->desc.size);
            f->desc.stride = f->elem_size + (rnd() % 3) * 4u;
            f->stride = f->desc.stride;
        }
        const u32 first = rnd() % 64, n = 1 + rnd() % (MAXN - 1);
        for (u32 i = 0; i < n; i++) { refs[i].vertex_id = first + i; refs[i].base_index = 0; }
        /* Now and then a SNORM16 component of -32768 in the range. */
        const int plant = rnd() % 8 == 0;
        if (plant)
            for (u32 slot = 0; slot < plan.layout.count; slot++) {
                rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[slot]];
                if (f->desc.type == RSX_VTX_TYPE_SNORM16) {
                    u8* p = guest[f->desc.location & 1u] + plan.base_offset + f->desc.offset + (first + n / 2) * f->stride;
                    p[0] = 0x80; p[1] = 0x00;
                }
            }
        rsx_vertex_fetch_plan_prepare(&plan, refs, n);
        rsx_vertex_host_streams hs;
        if (!rsx_vertex_host_streams_build(&plan, first, n, host, sizeof host, &hs)) {
            refused++;
            int has_snorm_min = 0;
            for (u32 slot = 0; slot < plan.layout.count; slot++) {
                const rsx_vertex_fetch_attr* f = &plan.attr[plan.layout.attrs[slot]];
                if (f->desc.type == RSX_VTX_TYPE_SNORM16 && f->stable_base && f->desc.frequency <= 1)
                    for (u32 i = 0; i < n; i++) for (u32 c = 0; c < f->desc.size; c++) {
                        const u8* p = f->stable_base + (u64)(first + i - f->stable_first) * f->stride + 2u * c;
                        has_snorm_min |= p[0] == 0x80 && p[1] == 0x00;
                    }
            }
            snorm_min_refused += has_snorm_min;
            continue;
        }
        built++;
        CHECK(hs.count == plan.layout.count, "plan %d: stream count", n_plan);
        CHECK(rsx_vertex_fetch_many(&plan, refs, n, four), "plan %d: float4 fetch", n_plan);
        for (u32 slot = 0; slot < hs.count; slot++) {
            const u32 esize = rsx_vertex_host_format_bytes(hs.format[slot]);
            for (u32 i = 0; i < n; i++) {
                float v[4];
                host_decode(hs.format[slot], host + hs.offset[slot] + i * esize, v);
                const u8* want = four + (size_t)i * plan.layout.stride + slot * 16u;
                if (memcmp(v, want, 16)) {
                    float w[4]; memcpy(w, want, 16);
                    CHECK(0, "plan %d slot %u vertex %u format %u: host %g %g %g %g, float4 %g %g %g %g", n_plan, slot, i,
                          hs.format[slot], v[0], v[1], v[2], v[3], w[0], w[1], w[2], w[3]);
                    break;
                }
            }
        }
    }
    printf("test_vertex_fetch_plan: host streams %d built, %d refused (%d for SNORM16 -32768), %d failure(s)\n",
           built, refused, snorm_min_refused, fails);
    return built > 1000 && snorm_min_refused > 10;
}

int main(void)
{
    if (!test_host_streams()) { printf("FAIL: too few host-stream cases\n"); return 1; }
    enum { PLANS = 6000, MAXV = 200 };
    static rsx_vertex_ref refs[MAXV];
    static u8 inl[8192];
    int stable = 0, divided = 0, fetched = 0;
    for (u32 i = 0; i < GUEST; i++) { guest[0][i] = (u8)rnd(); guest[1][i] = (u8)rnd(); }
    for (u32 i = 0; i < sizeof inl; i++) inl[i] = (u8)rnd();

    for (int n = 0; n < PLANS; n++) {
        for (int l = 0; l < 2; l++)
            mapped_end[l] = (rnd() % 3 == 0) ? rnd() % GUEST : GUEST;
        rsx_vertex_fetch_plan plan, ref;
        random_plan(&plan, rnd() % 5 == 0, inl, (u32)(rnd() % sizeof inl));

        const u32 count = 1 + rnd() % MAXV;
        const u32 base = (rnd() % 4 == 0) ? rnd() % 64u : 0;
        /* Some runs sit just below the 20-bit element wrap and cross it. */
        const u32 start = (rnd() % 4 == 0) ? 0x000FFFFFu - rnd() % 400u : 0;
        for (u32 i = 0; i < count; i++) {
            refs[i].vertex_id = start + ((rnd() % 8 == 0) ? rnd() : rnd() % 300u);
            refs[i].base_index = base;
        }
        ref = plan;
        rsx_vertex_fetch_plan_prepare(&plan, refs, count);
        ref_prepare(&ref, refs, count);
        /* The batched fetch against one vertex at a time. */
        {
            static u8 one[MAXV * 256], many[MAXV * 256];
            const u32 stride = plan.layout.stride;
            int ok_one = 1;
            memset(one, 0xCD, sizeof one);
            memset(many, 0xCD, sizeof many);
            for (u32 i = 0; i < count && ok_one; i++)
                ok_one = rsx_vertex_fetch_one(&plan, &refs[i], one + (size_t)i * stride);
            const int ok_many = rsx_vertex_fetch_many(&plan, refs, count, many);
            CHECK(ok_one == ok_many, "plan %d: fetch_many %d, fetch_one %d", n, ok_many, ok_one);
            CHECK(!ok_one || !memcmp(one, many, (size_t)count * stride),
                  "plan %d: fetch_many vertices differ from fetch_one", n);
            if (ok_one) fetched++;
        }
        for (u32 at = 0; at < RSX_DSP_NUM_VERTEX_ATTR; at++) {
            CHECK(plan.attr[at].stable_base == ref.attr[at].stable_base &&
                  plan.attr[at].stable_first == ref.attr[at].stable_first &&
                  plan.attr[at].stable_last == ref.attr[at].stable_last &&
                  plan.attr[at].stride == ref.attr[at].stride,
                  "plan %d attr %u: span %u..%u, walk %u..%u", n, at,
                  plan.attr[at].stable_first, plan.attr[at].stable_last,
                  ref.attr[at].stable_first, ref.attr[at].stable_last);
            if (ref.attr[at].stable_base) {
                stable++;
                if (plan.attr[at].desc.frequency > 1) divided++;
            }
        }
    }

    printf("test_vertex_fetch_plan: %d plans, %d stable ranges (%d divided), %d fetched, %d failure(s)\n",
           PLANS, stable, divided, fetched, fails);
    if (stable < PLANS || divided < 100 || fetched < PLANS / 2) {
        printf("FAIL: too few cases exercised a path\n");
        return 1;
    }
    return fails ? 1 : 0;
}
