/* Synthetic requests only. Reuse the existing backend stub and compare
 * complete backend state, guest-memory reads, VM bytes and errno with the
 * same requests observed and unobserved. No window, GPU or title data. */
#define main existing_engine_test_main
#include "test_rsx_draw_engine.c"
#undef main
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static int fail_pipeline;
static int observed_reads;
static u32 test_pipeline(void* u, const char* vs, const char* ps,
    const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
    u32 stride, rsx_be_format rt, u32 rt_count)
{
    if (fail_pipeline) { stub.n_pipeline_create++; return 0; }
    return stub_pipeline_create(u, vs, ps, rs, layout, stride, rt, rt_count);
}
static const u8* test_read(void* u, u32 loc, u32 offset, u32 bytes)
{
    (void)u;
    observed_reads++;
    const u64 address = offset + (loc == RSX_LOCATION_LOCAL ? GUEST_LOCAL_EA : 0);
    return address + bytes <= GUEST_BYTES ? vm_base + address : NULL;
}
static void draw_count(u32 prim, u32 count)
{
    m(M_BEGIN_END, prim);
    m(M_DRAW_ARRAYS, ((count - 1) << 24));
    m(M_BEGIN_END, 0);
}
static StubState sequence(const char* output, const char* trigger,
                          int* reads, int errnos[8], int break_writer)
{
    if (output) setenv("RSX_ENGINE_DRAW_OBSERVER", output, 1);
    else unsetenv("RSX_ENGINE_DRAW_OBSERVER");
    setenv("RSX_ENGINE_DRAW_OBSERVER_TRIGGER", trigger, 1);
    setenv("RSX_ENGINE_DRAW_OBSERVER_TARGET", "0x40000", 1);
    setenv("RSX_ENGINE_DRAW_OBSERVER_MAX", "6", 1);
    unlink(trigger);
    observed_reads = 0;
    fail_pipeline = 0;
    engine_up();
    rsx_draw_backend backend = g_stub_backend;
    backend.pipeline_create = test_pipeline;
    rsx_draw_engine_set_backend(&backend);
    rsx_draw_engine_set_guest_memory(test_read, NULL);
    errno = E2BIG;
    draw_triangle(); /* pre-arm, must not consume the cap */
    errnos[0] = errno;
    CHECK(!output || access(output, F_OK) != 0, "no capture before arming");
    FILE* f = fopen(trigger, "w");
    CHECK(f != NULL, "synthetic trigger can be created");
    if (f) { fputs("armed\n", f); fclose(f); }
    m(M_COLOR_A_OFFSET, 0x50000);
    errno = EDOM;
    draw_triangle(); /* wrong target, must not consume the cap */
    errnos[1] = errno;
    CHECK(!output || access(output, F_OK) != 0, "no capture for unrelated target");
    m(M_COLOR_A_OFFSET, 0x40000);
    errno = ERANGE;
    draw_triangle();
    errnos[2] = errno;
    if (break_writer) {
        /* Fault only this process's unique diagnostic writer descriptor. */
        struct stat path_stat, fd_stat;
        int closed = 0;
        if (stat(output, &path_stat) == 0) {
            for (int fd = 3; fd < 64; fd++)
                if (fstat(fd, &fd_stat) == 0 && fd_stat.st_ino == path_stat.st_ino &&
                    fd_stat.st_dev == path_stat.st_dev) { close(fd); closed++; break; }
        }
        CHECK(closed == 1, "synthetic diagnostic write failure injected");
    }
    m(M_VTXBUF_OFFSET, 0x80000000u | (GUEST_BYTES + 64));
    errno = EINVAL;
    draw_triangle(); /* unreadable vertex data */
    errnos[3] = errno;
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
    errno = E2BIG;
    draw_count(RSX_PRIMITIVE_TRIANGLES, 2); /* no complete triangle */
    errnos[4] = errno;
    errno = EDOM;
    draw_count(0xFE, 3); /* unsupported primitive */
    errnos[5] = errno;
    fail_pipeline = 1;
    m(0x0310, 1); /* blend changes pipeline identity */
    errno = ERANGE;
    draw_triangle();
    errnos[6] = errno;
    fail_pipeline = 0;
    m(0x0310, 0);
    errno = EINVAL;
    draw_triangle(); /* sixth matching outcome */
    draw_triangle(); /* past cap: draw still executes */
    errnos[7] = errno;
    *reads = observed_reads;
    StubState result = stub;
    engine_down();
    return result;
}
int main(void)
{
    char directory[] = "/tmp/rsx-draw-observer-XXXXXX";
    CHECK(mkdtemp(directory) != NULL, "private synthetic output directory");
    char output[512], trigger[512], broken[512];
    snprintf(output, sizeof output, "%s/requests.jsonl", directory);
    snprintf(trigger, sizeof trigger, "%s/armed", directory);
    snprintf(broken, sizeof broken, "%s/missing/requests.jsonl", directory);
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 1;
    ppu_vm_size = GUEST_BYTES;
    for (u32 i = 0; i < 12; i++) vm_base[VTX_OFFSET + i * 4] = 0x3F,
        vm_base[VTX_OFFSET + i * 4 + 1] = 0x80;
    u8* before = malloc(GUEST_BYTES);
    if (!before) return 1;
    memcpy(before, vm_base, GUEST_BYTES);
    int base_reads, armed_reads, failed_reads, base_errno[8], armed_errno[8], failed_errno[8];
    const StubState base = sequence(NULL, trigger, &base_reads, base_errno, 0);
    CHECK(access(output, F_OK) != 0, "default off creates no output");
    const StubState armed = sequence(output, trigger, &armed_reads, armed_errno, 0);
    CHECK(memcmp(&base, &armed, sizeof base) == 0, "observer preserves all backend behavior/state");
    CHECK(base_reads == armed_reads, "observer adds no guest-memory reads");
    CHECK(memcmp(base_errno, armed_errno, sizeof base_errno) == 0, "successful diagnostic IO preserves errno");
    CHECK(memcmp(before, vm_base, GUEST_BYTES) == 0, "observer leaves guest memory unchanged");
    FILE* f = fopen(output, "r");
    CHECK(f != NULL, "armed matching requests produce a receipt");
    int lines = 0, emitted = 0, fetch = 0, topology = 0, zero = 0, pipeline = 0;
    char line[8192];
    while (f && fgets(line, sizeof line, f)) {
        lines++;
        emitted += strstr(line, "\"outcome\":\"emitted\"") != NULL;
        fetch += strstr(line, "\"outcome\":\"fetch\"") != NULL;
        topology += strstr(line, "\"outcome\":\"topology\"") != NULL;
        zero += strstr(line, "\"outcome\":\"zero_geometry\"") != NULL;
        pipeline += strstr(line, "\"outcome\":\"pipeline\"") != NULL;
        CHECK(line[0] == '{' && strchr(line, '\n'), "bounded complete JSONL record");
    }
    if (f) fclose(f);
    CHECK(lines == 6, "cap applies only to matching armed requests (%d)", lines);
    CHECK(emitted == 2 && fetch == 1 && topology == 1 && zero == 1 && pipeline == 1,
          "accepted and rejected requests carry exact outcomes");
    const StubState failed = sequence(broken, trigger, &failed_reads, failed_errno, 0);
    CHECK(memcmp(&base, &failed, sizeof base) == 0, "IO failure preserves backend behavior/state");
    CHECK(base_reads == failed_reads, "IO failure adds no guest-memory reads");
    CHECK(memcmp(base_errno, failed_errno, sizeof base_errno) == 0, "IO failure preserves original errno");
    CHECK(memcmp(before, vm_base, GUEST_BYTES) == 0, "IO failure leaves guest memory unchanged");
    unlink(output);
    const StubState write_failed = sequence(output, trigger, &failed_reads, failed_errno, 1);
    CHECK(memcmp(&base, &write_failed, sizeof base) == 0, "write failure preserves backend behavior/state");
    CHECK(base_reads == failed_reads, "write failure adds no guest-memory reads");
    CHECK(memcmp(base_errno, failed_errno, sizeof base_errno) == 0, "write failure preserves original errno");
    CHECK(memcmp(before, vm_base, GUEST_BYTES) == 0, "write failure leaves guest memory unchanged");
    /* Empty groups must be visible without causing a backend submission. */
    unlink(output);
    setenv("RSX_ENGINE_DRAW_OBSERVER", output, 1);
    setenv("RSX_ENGINE_DRAW_OBSERVER_MAX", "1", 1);
    engine_up();
    errno = EDOM;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_BEGIN_END, 0);
    CHECK(stub.n_draw == 0 && stub.n_pipeline_create == 0,
          "empty request retains original no-submission behavior");
    CHECK(errno == EDOM, "empty request diagnostic preserves errno");
    engine_down();
    f = fopen(output, "r");
    CHECK(f && fgets(line, sizeof line, f) && strstr(line, "empty_packets"),
          "empty request has an explicit receipt");
    if (f) fclose(f);
    unlink(output);
    setenv("RSX_ENGINE_DRAW_OBSERVER_MAX", "0", 1);
    engine_up();
    draw_triangle();
    CHECK(stub.n_draw == 1 && access(output, F_OK) != 0,
          "zero cap disables capture without changing rendering");
    engine_down();
    free(before); free(vm_base);
    unlink(trigger); unlink(output); rmdir(directory);
    printf(g_failures ? "%d checks failed\n" : "all observer checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
