#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_default(const rsx_dispatch* rsx, u32 attr, float out[4])
{
    rsx_dsp_vertex_default(rsx, attr, out);
    if (attr != 3)
        return;
    const u32 base = (0x1C00u + attr * 0x10u) >> 2;
    const u32 written = rsx->seen[base] | rsx->seen[base + 1] |
        rsx->seen[base + 2] | rsx->seen[base + 3];
    /* Preserve the legacy white color fallback only for an unwritten
     * attribute. An explicit (0,0,0,1) is shader input, even when it equals
     * the hardware default; a shader may swizzle x into output alpha. */
    if (!written && out[0] == 0.0f && out[1] == 0.0f &&
        out[2] == 0.0f && out[3] == 1.0f)
        out[0] = out[1] = out[2] = 1.0f;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_vertex_fetch_default(rsx, attr, fetch->default_value);
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (frequency 0/1) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
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

        u32 first = UINT_MAX;
        u32 last = 0;
        if (fetch->desc.frequency <= 1) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix =
            (u64)plan->base_offset + (u64)fetch->desc.offset;
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            plan->base_offset + fetch->desc.offset +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

/* ---- PS3RECOMP_RSX_VERTEX_SIMD -------------------------------------------
 *
 * PS3RECOMP_RSX_VERTEX_SIMD=1 (default off, read once): the stable-range loop
 * below decodes one vertex's attribute with one 128-bit operation instead of
 * one scalar conversion per component. The bytes written are the same:
 *   - a half is decoded with the same shift/mask/add arithmetic as
 *     rsx_compact_be_f16, in four lanes, so a denormal half is still flushed
 *     to its signed zero and a NaN keeps its payload (vcvtph2ps/vcvt_f32_f16
 *     would do neither);
 *   - a float is a byte swap;
 *   - UNORM8 and SNORM16 are the same int-to-float conversion and the same
 *     IEEE division, which the vector divide rounds identically;
 *   - components the attribute does not have are (0, 0, 0, 1) as before.
 * Loads read exactly the element's bytes, never past it. x86 with SSE4.1 and
 * arm64 with NEON; elsewhere the switch is accepted and the scalar loop runs.
 * rsx_vertex_fetch_simd_override is for the differential fixture. */
#if (defined(__SSE4_1__) || defined(__AVX__)) && (defined(__x86_64__) || defined(_M_X64))
#  include <immintrin.h>
#  define RSX_VERTEX_SIMD 1
typedef __m128i rsx_v4;
static inline rsx_v4 rsx_v4_load(const u8* p, u32 bytes)
{
    switch (bytes) {
    case 16: return _mm_loadu_si128((const __m128i*)p);
    case 12: { u32 w; memcpy(&w, p + 8, 4);
               return _mm_insert_epi32(_mm_loadl_epi64((const __m128i*)p), (int)w, 2); }
    case 8:  return _mm_loadl_epi64((const __m128i*)p);
    case 6:  { u32 w; u16 h; memcpy(&w, p, 4); memcpy(&h, p + 4, 2);
               return _mm_insert_epi16(_mm_cvtsi32_si128((int)w), h, 2); }
    case 4:  { u32 w; memcpy(&w, p, 4); return _mm_cvtsi32_si128((int)w); }
    case 3:  return _mm_cvtsi32_si128((int)((u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16));
    case 2:  { u16 h; memcpy(&h, p, 2); return _mm_cvtsi32_si128(h); }
    default: return _mm_cvtsi32_si128(p[0]);
    }
}
/* Big-endian 32-bit words to host order. */
static inline rsx_v4 rsx_v4_be32(rsx_v4 v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
/* Four big-endian 16-bit values to four zero-extended 32-bit lanes. */
static inline rsx_v4 rsx_v4_be16(rsx_v4 v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, 7, 6, -1, -1));
}
static inline rsx_v4 rsx_v4_sext16(rsx_v4 v) { return _mm_srai_epi32(_mm_slli_epi32(v, 16), 16); }
static inline rsx_v4 rsx_v4_u8(rsx_v4 v) { return _mm_cvtepu8_epi32(v); }
static inline rsx_v4 rsx_v4_f32(rsx_v4 v) { return _mm_castps_si128(_mm_cvtepi32_ps(v)); }
static inline rsx_v4 rsx_v4_div(rsx_v4 v, float d)
{
    return _mm_castps_si128(_mm_div_ps(_mm_castsi128_ps(v), _mm_set1_ps(d)));
}
static inline rsx_v4 rsx_v4_or(rsx_v4 a, rsx_v4 b) { return _mm_or_si128(a, b); }
static inline rsx_v4 rsx_v4_fill(u32 w) { return _mm_set_epi32((int)w, 0, 0, 0); }
static inline void rsx_v4_store(float* v, rsx_v4 x) { _mm_storeu_si128((__m128i*)v, x); }
/* One CMP32 word (big-endian in lane 0): the 11/11/10-bit signed fields to
 * (x / 1023, y / 1023, z / 511, 0), as rsx_vertex_decode_element computes
 * them (sign-extend by shifts, int to float, the same IEEE divisions). Each
 * field is moved to the top of its lane by a power-of-two multiply (no
 * per-lane shift below AVX2), then shifted down arithmetically: 21 for the
 * 11-bit fields, 22 for the 10-bit one. */
static inline rsx_v4 rsx_v4_cmp32(rsx_v4 raw)
{
    const __m128i w = _mm_shuffle_epi32(rsx_v4_be32(raw), 0);
    const __m128i top = _mm_mullo_epi32(w, _mm_setr_epi32(1 << 21, 1 << 10, 1, 0));
    const __m128i s = _mm_blend_epi16(_mm_srai_epi32(top, 21), _mm_srai_epi32(top, 22), 0x30);
    return _mm_castps_si128(_mm_div_ps(_mm_cvtepi32_ps(s), _mm_setr_ps(1023.0f, 1023.0f, 511.0f, 1.0f)));
}
/* Four halves in 32-bit lanes, decoded as rsx_compact_be_f16 does. */
static inline rsx_v4 rsx_v4_half(rsx_v4 h)
{
    const __m128i sign  = _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x8000)), 16);
    const __m128i exp   = _mm_and_si128(_mm_srli_epi32(h, 10), _mm_set1_epi32(0x1F));
    const __m128i man13 = _mm_slli_epi32(_mm_and_si128(h, _mm_set1_epi32(0x3FF)), 13);
    const __m128i norm  = _mm_or_si128(sign, _mm_or_si128(
        _mm_slli_epi32(_mm_add_epi32(exp, _mm_set1_epi32(112)), 23), man13));
    const __m128i inf   = _mm_or_si128(sign, _mm_or_si128(_mm_set1_epi32(0x7F800000), man13));
    const __m128i is0   = _mm_cmpeq_epi32(exp, _mm_setzero_si128());
    const __m128i is31  = _mm_cmpeq_epi32(exp, _mm_set1_epi32(31));
    const __m128i r     = _mm_blendv_epi8(norm, inf, is31);
    return _mm_blendv_epi8(r, sign, is0);   /* exponent 0: the signed zero */
}
#elif defined(__aarch64__) || defined(_M_ARM64)
#  include <arm_neon.h>
#  define RSX_VERTEX_SIMD 1
typedef uint32x4_t rsx_v4;
static inline rsx_v4 rsx_v4_load(const u8* p, u32 bytes)
{
    u8 tmp[16] = {0};
    switch (bytes) {
    case 16: return vreinterpretq_u32_u8(vld1q_u8(p));
    case 12: memcpy(tmp, p, 12); break;
    case 8:  memcpy(tmp, p, 8); break;
    case 6:  memcpy(tmp, p, 6); break;
    case 4:  memcpy(tmp, p, 4); break;
    case 3:  memcpy(tmp, p, 3); break;
    case 2:  memcpy(tmp, p, 2); break;
    default: tmp[0] = p[0]; break;
    }
    return vreinterpretq_u32_u8(vld1q_u8(tmp));
}
static inline rsx_v4 rsx_v4_be32(rsx_v4 v) { return vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(v))); }
static inline rsx_v4 rsx_v4_be16(rsx_v4 v)
{
    static const u8 index[16] = {1, 0, 255, 255, 3, 2, 255, 255, 5, 4, 255, 255, 7, 6, 255, 255};
    return vreinterpretq_u32_u8(vqtbl1q_u8(vreinterpretq_u8_u32(v), vld1q_u8(index)));
}
static inline rsx_v4 rsx_v4_sext16(rsx_v4 v)
{
    return vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32(vshlq_n_u32(v, 16)), 16));
}
static inline rsx_v4 rsx_v4_u8(rsx_v4 v)
{
    return vmovl_u16(vget_low_u16(vmovl_u8(vget_low_u8(vreinterpretq_u8_u32(v)))));
}
static inline rsx_v4 rsx_v4_f32(rsx_v4 v) { return vreinterpretq_u32_f32(vcvtq_f32_s32(vreinterpretq_s32_u32(v))); }
static inline rsx_v4 rsx_v4_div(rsx_v4 v, float d)
{
    return vreinterpretq_u32_f32(vdivq_f32(vreinterpretq_f32_u32(v), vdupq_n_f32(d)));
}
static inline rsx_v4 rsx_v4_or(rsx_v4 a, rsx_v4 b) { return vorrq_u32(a, b); }
static inline rsx_v4 rsx_v4_fill(u32 w) { return vsetq_lane_u32(w, vdupq_n_u32(0), 3); }
static inline void rsx_v4_store(float* v, rsx_v4 x) { vst1q_u32((u32*)v, x); }
/* One CMP32 word (big-endian in lane 0): see the x86 form. */
static inline rsx_v4 rsx_v4_cmp32(rsx_v4 raw)
{
    static const u32 scale[4] = { 1u << 21, 1u << 10, 1u, 0u };
    static const u32 lane_z[4] = { 0u, 0u, 0xFFFFFFFFu, 0u };
    static const float div[4] = { 1023.0f, 1023.0f, 511.0f, 1.0f };
    const uint32x4_t w = vdupq_laneq_u32(rsx_v4_be32(raw), 0);
    const int32x4_t top = vreinterpretq_s32_u32(vmulq_u32(w, vld1q_u32(scale)));
    const int32x4_t s = vbslq_s32(vld1q_u32(lane_z), vshrq_n_s32(top, 22), vshrq_n_s32(top, 21));
    return vreinterpretq_u32_f32(vdivq_f32(vcvtq_f32_s32(s), vld1q_f32(div)));
}
static inline rsx_v4 rsx_v4_half(rsx_v4 h)
{
    const uint32x4_t sign  = vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x8000)), 16);
    const uint32x4_t exp   = vandq_u32(vshrq_n_u32(h, 10), vdupq_n_u32(0x1F));
    const uint32x4_t man13 = vshlq_n_u32(vandq_u32(h, vdupq_n_u32(0x3FF)), 13);
    const uint32x4_t norm  = vorrq_u32(sign, vorrq_u32(vshlq_n_u32(vaddq_u32(exp, vdupq_n_u32(112)), 23), man13));
    const uint32x4_t inf   = vorrq_u32(sign, vorrq_u32(vdupq_n_u32(0x7F800000), man13));
    const uint32x4_t is0   = vceqq_u32(exp, vdupq_n_u32(0));
    const uint32x4_t is31  = vceqq_u32(exp, vdupq_n_u32(31));
    const uint32x4_t r     = vbslq_u32(is31, inf, norm);
    return vbslq_u32(is0, sign, r);   /* exponent 0: the signed zero */
}
#else
#  define RSX_VERTEX_SIMD 0
#endif

static int s_vertex_simd = -1;
static int rsx_vertex_simd_on(void)
{
    if (s_vertex_simd < 0) {
        const char* e = getenv("PS3RECOMP_RSX_VERTEX_SIMD");
        s_vertex_simd = e && !strcmp(e, "1");
    }
    return s_vertex_simd;
}
void rsx_vertex_fetch_simd_override(int on) { s_vertex_simd = on ? 1 : 0; }
int rsx_vertex_fetch_simd_available(void) { return RSX_VERTEX_SIMD; }

/* The stable-range loop for one attribute and one vertex type: `decode`
 * reads `src` into `v` (pre-set to 0,0,0,1) for components c < n. Elements
 * outside the stable range take the one-vertex path. */
#define RSX_FETCH_LOOP(decode)                                                  \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                          \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;      \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    const int simd = rsx_vertex_simd_on();
    (void)simd;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        if (!type || !fetch->desc.size || !base ||
            type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const int plain = fetch->desc.frequency <= 1;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
#if RSX_VERTEX_SIMD
        if (simd) {
            /* Lanes past n hold zero after the load, and every decode maps a
             * zero lane to +0.0f, so (0, 0, 0, 1) is the one OR of w. A CMP32
             * element is one packed word whatever the size, and its w is
             * always 1. */
            const int cmp32 = type == RSX_VTX_TYPE_CMP32;
            const rsx_v4 fill = n < 4 || cmp32 ? rsx_v4_fill(0x3F800000u) : rsx_v4_fill(0);
            const u32 bytes = cmp32 ? 4u :
                              n * (type == RSX_VTX_TYPE_FLOAT ? 4u :
                                   type == RSX_VTX_TYPE_UNORM8 || type == RSX_VTX_TYPE_UINT8 ? 1u : 2u);
#define RSX_FETCH_LOOP_SIMD(decode)                                                 \
            for (u32 i = 0; i < count; i++) {                                       \
                float* v = (float*)(out + (size_t)i * stride + slot * 16u);         \
                const u32 element = plain                                           \
                    ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)      \
                    : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index, \
                                               fetch->desc.frequency, modulo);      \
                if (element - first > span) {                                      \
                    if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0; \
                    continue;                                                       \
                }                                                                   \
                const rsx_v4 raw = rsx_v4_load(base + (u64)(element - first) * fstride, bytes); \
                rsx_v4_store(v, rsx_v4_or(decode, fill));                           \
            }
            switch (type) {
            case RSX_VTX_TYPE_FLOAT:
                RSX_FETCH_LOOP_SIMD(rsx_v4_be32(raw))
                break;
            case RSX_VTX_TYPE_HALF:
                RSX_FETCH_LOOP_SIMD(rsx_v4_half(rsx_v4_be16(raw)))
                break;
            case RSX_VTX_TYPE_UNORM8:
                RSX_FETCH_LOOP_SIMD(rsx_v4_div(rsx_v4_f32(rsx_v4_u8(raw)), 255.0f))
                break;
            case RSX_VTX_TYPE_UINT8:
                RSX_FETCH_LOOP_SIMD(rsx_v4_f32(rsx_v4_u8(raw)))
                break;
            case RSX_VTX_TYPE_SNORM16:
                RSX_FETCH_LOOP_SIMD(rsx_v4_div(rsx_v4_f32(rsx_v4_sext16(rsx_v4_be16(raw))), 32767.0f))
                break;
            case RSX_VTX_TYPE_CMP32:
                RSX_FETCH_LOOP_SIMD(rsx_v4_cmp32(raw))
                break;
            default:   /* RSX_VTX_TYPE_SINT16 */
                RSX_FETCH_LOOP_SIMD(rsx_v4_f32(rsx_v4_sext16(rsx_v4_be16(raw))))
                break;
            }
#undef RSX_FETCH_LOOP_SIMD
            continue;
        }
#endif
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
            break;
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
            break;
        default:   /* RSX_VTX_TYPE_CMP32: one packed word, whatever the size */
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    /* A draw whose references share one base index and cover a range of
     * vertex ids not much wider than the reference count -- the common
     * indexed mesh -- is collapsed through a direct table over that range:
     * the same first-use order and occurrence map the hash gives, without
     * hashing or probing. */
    if (count >= 8u) {
        u32 lo = refs[0].vertex_id, hi = lo;
        const u32 base = refs[0].base_index;
        int uniform = 1;
        for (u32 i = 1; i < count; i++) {
            const u32 id = refs[i].vertex_id;
            if (id < lo) lo = id;
            if (id > hi) hi = id;
            uniform &= refs[i].base_index == base;
        }
        const u64 range = (u64)hi - lo + 1u;
        if (uniform && range <= (u64)count * 4u + 64u && range <= (1u << 20)) {
            if (range > remap->dense_capacity) {
                u32 cap = remap->dense_capacity ? remap->dense_capacity : 256u;
                while (cap < range) cap *= 2u;
                u32* stamp = (u32*)realloc(remap->dense_stamp, (size_t)cap * sizeof(u32));
                if (stamp) remap->dense_stamp = stamp;
                u32* value = stamp ? (u32*)realloc(remap->dense_value, (size_t)cap * sizeof(u32)) : NULL;
                if (value) remap->dense_value = value;
                if (!stamp || !value)
                    goto hashed;
                memset(remap->dense_stamp, 0, (size_t)cap * sizeof(u32));
                remap->dense_capacity = cap;
                remap->dense_generation = 0;
            }
            if (++remap->dense_generation == 0u) {
                memset(remap->dense_stamp, 0, (size_t)remap->dense_capacity * sizeof(u32));
                remap->dense_generation = 1u;
            }
            const u32 gen = remap->dense_generation;
            u32* stamp = remap->dense_stamp;
            u32* value = remap->dense_value;
            u32* map = remap->occurrence_to_unique;
            u32 unique = 0;
            for (u32 occurrence = 0; occurrence < count; occurrence++) {
                const u32 at = refs[occurrence].vertex_id - lo;
                if (stamp[at] == gen) {
                    map[occurrence] = value[at];
                } else {
                    stamp[at] = gen;
                    value[at] = unique;
                    refs[unique].vertex_id = refs[occurrence].vertex_id;
                    refs[unique].base_index = base;
                    map[occurrence] = unique++;
                }
            }
            *unique_count = unique;
            return 1;
        }
    }
hashed:;
    u32 unique = 0;
    /* Only as many slots as this draw needs (at least twice its count): a
     * small draw after a large one probes a table that stays in cache. The
     * generation marks what is live, so the untouched rest does not matter. */
    u32 used_slots = 16u;
    while (used_slots < required_slots) used_slots *= 2u;
    const u32 mask = used_slots - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    free(remap->dense_stamp);
    free(remap->dense_value);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}

static int rsx_vertex_host_format_for(u32 type, u32 size, u8* format)
{
    if (!size || size > 4) return 0;
    switch (type) {
    case RSX_VTX_TYPE_FLOAT:   *format = (u8)(RSX_HOST_VF_F32_1 + size - 1u); return 1;
    case RSX_VTX_TYPE_HALF:    *format = RSX_HOST_VF_F16_4; return 1;
    case RSX_VTX_TYPE_UNORM8:  *format = RSX_HOST_VF_UN8_4; return 1;
    case RSX_VTX_TYPE_SNORM16: *format = RSX_HOST_VF_SN16_4; return 1;
    default: return 0;
    }
}

int rsx_vertex_host_streams_build(
    const rsx_vertex_fetch_plan* plan, u32 first, u32 n,
    u8* out, u32 out_cap, rsx_vertex_host_streams* streams)
{
    if (!plan || !streams || !n || plan->inline_data)
        return 0;
    memset(streams, 0, sizeof(*streams));
    u32 at = 0;
    /* Formats and sizes first, so a refusal writes nothing. */
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* fetch = &plan->attr[plan->layout.attrs[slot]];
        u8 format = 0;
        if (!fetch->desc.type || !fetch->desc.size || fetch->desc.frequency > 1 ||
            !fetch->stable_base || !rsx_vertex_host_format_for(fetch->desc.type, fetch->desc.size, &format) ||
            first < fetch->stable_first || first + n - 1u > fetch->stable_last || first + n - 1u < first)
            return 0;
        const u32 esize = rsx_vertex_host_format_bytes(format);
        if ((u64)at + (u64)esize * n > out_cap) return 0;
        streams->format[slot] = format;
        streams->offset[slot] = at;
        at += esize * n;
    }
    streams->count = plan->layout.count;
    streams->bytes = at;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const rsx_vertex_fetch_attr* fetch = &plan->attr[plan->layout.attrs[slot]];
        const u8* src = fetch->stable_base + (u64)(first - fetch->stable_first) * fetch->stride;
        u8* dst = out + streams->offset[slot];
        const u32 size = fetch->desc.size, stride = fetch->stride;
        switch (fetch->desc.type) {
        case RSX_VTX_TYPE_FLOAT:
            for (u32 i = 0; i < n; i++, src += stride)
                for (u32 c = 0; c < size; c++, dst += 4) {
                    const u8* p = src + c * 4u;
                    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
                    memcpy(dst, &w, 4);
                }
            break;
        case RSX_VTX_TYPE_HALF:
            for (u32 i = 0; i < n; i++, src += stride) {
                u16 h[4] = { 0, 0, 0, 0x3C00u };
                for (u32 c = 0; c < size; c++) {
                    u16 v = (u16)((src[c * 2u] << 8) | src[c * 2u + 1u]);
                    if (!(v & 0x7C00u)) v &= 0x8000u;       /* zero or denormal: signed zero */
                    h[c] = v;
                }
                memcpy(dst, h, 8); dst += 8;
            }
            break;
        case RSX_VTX_TYPE_UNORM8:
            for (u32 i = 0; i < n; i++, src += stride) {
                u8 b[4] = { 0, 0, 0, 255 };
                for (u32 c = 0; c < size; c++) b[c] = src[c];
                memcpy(dst, b, 4); dst += 4;
            }
            break;
        case RSX_VTX_TYPE_SNORM16:
            for (u32 i = 0; i < n; i++, src += stride) {
                s16 v[4] = { 0, 0, 0, 32767 };
                for (u32 c = 0; c < size; c++) {
                    v[c] = (s16)((src[c * 2u] << 8) | src[c * 2u + 1u]);
                    if (v[c] == -32768) return 0;
                }
                memcpy(dst, v, 8); dst += 8;
            }
            break;
        default:
            return 0;
        }
    }
    return 1;
}
