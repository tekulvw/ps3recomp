/* Synthetic draw-engine test of pipeline identity across many guest program
 * pairs: each distinct transform program gets its own pipeline, revisiting
 * any of them reuses it, and a transform program rewritten in place is a new
 * pipeline. Also the inline-constant collector's slot contract. No title
 * data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_test_programs.h"
#include "rsx_fp_decompiler.h"

#define N_PROGRAMS 256u

/* The host-format half of the stub backend. */
static struct {
    int creates, draws;
    u8 formats[16];
    u32 streams, vertex_count, index_count, index_size;
    u8 data[256];
    u16 indices[8];
} g_host;
static u32 stub_pipeline_create_host(void* u, const char* vs, const char* ps,
                                     const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                     const u8* formats, rsx_be_format rt, u32 rt_count)
{
    g_host.creates++;
    memcpy(g_host.formats, formats, layout->count);
    return stub_pipeline_create(u, vs, ps, rs, layout, 0, rt, rt_count);
}
static void stub_draw_host(void* u, rsx_topology topology, const void* data, u32 data_bytes,
                           const u32* offsets, u32 streams, u32 vertex_count,
                           const void* indices, u32 index_count, u32 index_size)
{
    (void)u; (void)topology; (void)offsets;
    g_host.draws++;
    g_host.streams = streams; g_host.vertex_count = vertex_count;
    g_host.index_count = index_count; g_host.index_size = index_size;
    memcpy(g_host.data, data, data_bytes < sizeof g_host.data ? data_bytes : sizeof g_host.data);
    if (index_size == 2) memcpy(g_host.indices, indices, (index_count < 8 ? index_count : 8) * 2u);
}

static void upload_vp(u32 swz)
{
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 0, swz, 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    m(0x1EA0, 0);
}

static u32 swizzle(u32 i)
{
    return RSX_TEST_VP_SWZ(i & 3u, (i >> 2) & 3u, (i >> 4) & 3u, (i >> 6) & 3u);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    engine_up();
    rsx_test_fp_instr(vm_base + TEX_OFFSET,
        RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
        RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    m(0x8E4, TEX_OFFSET | 2);
    m(0x1D60, 64);

    u32 handle[N_PROGRAMS];
    for (u32 i = 0; i < N_PROGRAMS; i++) {
        upload_vp(swizzle(i));
        draw_triangle();
        handle[i] = stub.bound_pipeline;
    }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS,
          "each distinct transform program builds one pipeline (%d)", stub.n_pipeline_create);
    int distinct = 1;
    for (u32 i = 0; i < N_PROGRAMS; i++)
        for (u32 j = i + 1; j < N_PROGRAMS; j++)
            if (handle[i] == handle[j]) distinct = 0;
    CHECK(distinct, "and the pipelines are distinct");

    int same = 1;
    for (u32 r = 0; r < 2; r++)
        for (u32 k = 0; k < N_PROGRAMS; k++) {
            const u32 i = (k * 97u + r) % N_PROGRAMS;
            upload_vp(swizzle(i));
            draw_triangle();
            if (stub.bound_pipeline != handle[i]) same = 0;
        }
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS && same,
          "revisiting every program reuses its own pipeline (%d)", stub.n_pipeline_create);

    /* The same slot, the same length, different bytes: a new program. */
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.bound_pipeline == handle[5], "program 5 is bound");
    u8 vp[16];
    rsx_test_vp_mov_out(vp, 1, swizzle(5), 0, 1);
    m(0x1E9C, 0);
    for (u32 i = 0; i < 4; i++) { u32 w; memcpy(&w, vp + i * 4, 4); m(0xB80 + i * 4, w); }
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline != handle[5],
          "a program rewritten in place is not taken for the old one (%d)", stub.n_pipeline_create);
    upload_vp(swizzle(5));
    draw_triangle();
    CHECK(stub.n_pipeline_create == (int)N_PROGRAMS + 1 && stub.bound_pipeline == handle[5],
          "and restoring it finds the old pipeline again");
    engine_down();

    /* Only the slots a program has are written. */
    static rsx_fp_constant_block block;
    memset(&block, 0xAB, sizeof block);
    u8 fp[64] = {0};
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    CHECK(rsx_fp_collect_constants(fp, sizeof fp, &block) == 0 && block.count == 0,
          "a program without constants has no slots");
    CHECK(block.values[0][0] == 0xABABABABu, "and the unused slots are left alone");

    /* Host-format vertex streams (PS3RECOMP_RSX_HOST_VERTEX_FORMATS=1): a
     * triangle list over a FLOAT4 array reaches draw_host as byte-swapped
     * floats over its vertex range with u16 indices, through a pipeline made
     * by pipeline_create_host; a CMP32 array is refused and takes the float4
     * path; with the switch off nothing changes. */
    {
        g_stub_backend.pipeline_create_host = stub_pipeline_create_host;
        g_stub_backend.draw_host = stub_draw_host;
        setenv("PS3RECOMP_RSX_HOST_VERTEX_FORMATS", "1", 1);
        engine_up();
        upload_vp(swizzle(0));
        rsx_test_fp_instr(vm_base + TEX_OFFSET,
            RSX_TEST_FP_OPC(0x01) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
            RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
        m(0x8E4, TEX_OFFSET | 2);
        m(0x1D60, 64);
        /* Six vertices at VTX_OFFSET: x = 10 + i. */
        for (u32 v = 0; v < 6; v++) for (u32 c = 0; c < 4; c++) {
            const float f = c ? 0.5f : 10.0f + (float)v;
            u32 w; memcpy(&w, &f, 4);
            u8* p = vm_base + VTX_OFFSET + v * 16u + c * 4u;
            p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
        }
        const int draws0 = stub.n_draw, hosts0 = g_host.draws;
        /* Elements 2, 3, 4 (a range that does not start at 0). */
        m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        m(M_DRAW_ARRAYS, 2u | ((3u - 1u) << 24));
        m(M_BEGIN_END, 0u);
        CHECK(g_host.draws == hosts0 + 1 && stub.n_draw == draws0, "a plain triangle list is drawn with host streams");
        CHECK(g_host.creates == 1 && g_host.formats[0] == RSX_HOST_VF_F32_4 && g_host.streams == 1,
              "through a host-format pipeline with one FLOAT4 stream");
        float x0 = 0, y0 = 0; memcpy(&x0, g_host.data, 4); memcpy(&y0, g_host.data + 4, 4);
        CHECK(g_host.vertex_count == 3 && g_host.index_count == 3 && g_host.index_size == 2 &&
              g_host.indices[0] == 0 && g_host.indices[2] == 2 && x0 == 12.0f && y0 == 0.5f,
              "three vertices from element 2, byte-swapped, indices relative to the range (x0 %g)", x0);
        m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
        m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
        m(M_BEGIN_END, 0u);
        CHECK(g_host.creates == 1 && g_host.draws == hosts0 + 2, "the next draw reuses the pipeline");
        /* CMP32 has no host format: refused, drawn as float4. */
        m(M_VTXFMT, 6u | (1u << 4) | (16u << 8));
        unsigned long long hd = 0, hr = 0;
        rsx_draw_engine_host_vertex_counts(&hd, &hr);
        draw_triangle();
        unsigned long long hd2 = 0, hr2 = 0;
        rsx_draw_engine_host_vertex_counts(&hd2, &hr2);
        CHECK(hr2 == hr + 1 && stub.n_draw == draws0 + 1 && g_host.draws == hosts0 + 2,
              "an attribute without a host format takes the float4 path");
        m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
        engine_down();
        unsetenv("PS3RECOMP_RSX_HOST_VERTEX_FORMATS");
        engine_up();
        upload_vp(swizzle(0));
        m(0x8E4, TEX_OFFSET | 2);
        m(0x1D60, 64);
        const int before = stub.n_draw;
        draw_triangle();
        CHECK(stub.n_draw == before + 1 && g_host.draws == hosts0 + 2, "with the switch off every draw is float4");
        engine_down();
        g_stub_backend.pipeline_create_host = NULL;
        g_stub_backend.draw_host = NULL;
    }

    free(vm_base);
    printf("program identity failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
