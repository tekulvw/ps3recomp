/* SPU interpreter attribution sink: default-off no-op, keys, reasons, nesting,
 * unwinding, concurrent aggregation and the report format. argv[1] is "off"
 * or "on"; the runner sets SPU_INTERP_ATTRIBUTION only for "on". */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spu_interp_attribution.inc"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static spu_context ctx;
static char report[1 << 20];

static void put32(uint32_t lsa, uint32_t w)
{
    ctx.ls[lsa] = (uint8_t)(w >> 24); ctx.ls[lsa + 1] = (uint8_t)(w >> 16);
    ctx.ls[lsa + 2] = (uint8_t)(w >> 8); ctx.ls[lsa + 3] = (uint8_t)w;
}

static const char* read_report(const char* path)
{
    FILE* f = fopen(path, "r");
    CHECK(f != NULL);
    size_t n = fread(report, 1, sizeof report - 1, f);
    report[n] = 0; fclose(f);
    return report;
}

/* Row for an entry LSA, or NULL. */
static const char* row_for(const char* text, const char* entry_field)
{
    for (const char* line = text; line && *line; ) {
        const char* end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[512]; if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, line, len); buf[len] = 0;
        if (strstr(buf, entry_field)) { static char out[512]; strcpy(out, buf); return out; }
        line = end ? end + 1 : NULL;
    }
    return NULL;
}

static void field(const char* row, int index, char* out, size_t cap)
{
    const char* p = row;
    for (int i = 0; i < index; i++) { p = strchr(p, '\t'); CHECK(p); p++; }
    size_t n = strcspn(p, "\t"); if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
}

static void* worker(void* arg)
{
    static spu_context local[4];
    const uintptr_t id = (uintptr_t)arg;
    spu_context* c = &local[id];
    memset(c, 0, sizeof *c); spu_context_init(c, 0); c->image_id = 900 + (int)id;
    char frame;
    for (int i = 0; i < 20000; i++) {
        spu_interp_attr_begin(c, 0x800, SPU_IATTR_UNLIFTED, &frame);
        spu_interp_attr_add_steps(3);
        spu_interp_attr_end(&frame);
    }
    return NULL;
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
    const int on = !strcmp(argv[1], "on");
    const char* path = argv[2];
    memset(&ctx, 0, sizeof ctx); spu_context_init(&ctx, 0);
    ctx.image_id = 101; ctx.policy_mode = 1;
    for (uint32_t i = 0; i < 16; i++) ctx.ls[0x100 + i] = (uint8_t)(0xA0 + i);
    put32(0x1330, 0x00004000u);
    char outer, inner_storage[64];
    char* inner = inner_storage;   /* any address below `outer` is a deeper frame */
    if ((uintptr_t)inner > (uintptr_t)&outer) inner = (char*)&outer - 256;

    if (!on) {
        CHECK(!spu_interp_attr_enabled());
        spu_interp_attr_begin(&ctx, 0x100, SPU_IATTR_UNLIFTED, &outer);
        spu_interp_attr_add_steps(5);
        spu_interp_attr_end(&outer);
        spu_interp_attr_unwind();
        CHECK(spu_interp_attr_flush() == -1);
        CHECK(fopen(path, "r") == NULL);
        CHECK(!t_run.active);
        printf("PASS off: %u checks\n", checks);
        return 0;
    }

    CHECK(spu_interp_attr_enabled());
    /* Plain run. */
    spu_interp_attr_begin(&ctx, 0x100, SPU_IATTR_UNLIFTED, &outer);
    spu_interp_attr_add_steps(10);
    spu_interp_attr_end(&outer);
    /* Nested run only adds steps to the open outer run. */
    spu_interp_attr_begin(&ctx, 0x200, SPU_IATTR_VERIFIED_MISMATCH, &outer);
    spu_interp_attr_begin(&ctx, 0x204, SPU_IATTR_DIRECT, inner);
    spu_interp_attr_add_steps(5);
    spu_interp_attr_end(inner);
    spu_interp_attr_add_steps(7);
    spu_interp_attr_end(&outer);
    /* A begin from the same frame closes an unwound run. */
    spu_interp_attr_begin(&ctx, 0x300, SPU_IATTR_OTHER_IMAGE, &outer);
    spu_interp_attr_begin(&ctx, 0x400, SPU_IATTR_VERIFIED_NO_ENTRY, &outer);
    spu_interp_attr_end(&outer);
    /* Explicit halt unwinding. */
    spu_interp_attr_begin(&ctx, 0x500, SPU_IATTR_RESIDENT_NO_ENTRY, &outer);
    spu_interp_attr_unwind();
    CHECK(!t_run.active);
    /* Same key aggregates. */
    for (int i = 0; i < 3; i++) {
        spu_interp_attr_begin(&ctx, 0x600, SPU_IATTR_DIRECT, &outer);
        spu_interp_attr_add_steps(2);
        spu_interp_attr_end(&outer);
    }
    /* Resident bounded code identity. */
    ctx.resident_code[0].lsa = 0x4000; ctx.resident_code[0].size = 0x1000;
    ctx.resident_code[0].source_ea = 0x00ABC000; ctx.resident_code[0].image_id = 105;
    spu_interp_attr_begin(&ctx, 0x4010, SPU_IATTR_VERIFIED_MISMATCH, &outer);
    spu_interp_attr_end(&outer);
    /* Concurrent aggregation on separate keys. */
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) CHECK(!pthread_create(&threads[i], NULL, worker, (void*)i));
    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);

    CHECK(spu_interp_attr_flush() == 0);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    CHECK(fopen(tmp, "r") == NULL);
    const char* text = read_report(path);
    CHECK(!strncmp(text, "# spu-interp-attribution v1\n", strlen("# spu-interp-attribution v1\n")));
    CHECK(strstr(text, "unwound=2\n"));
    CHECK(strstr(text, "dropped=0 "));
    CHECK(strstr(text, "rows=11 "));
    for (int r = 0; r < SPU_IATTR_COUNT; r++) {
        char want[64]; snprintf(want, sizeof want, "reason %s wall_ns=", spu_interp_attr_reason_name(r));
        CHECK(strstr(text, want));
    }
    const char* hdr = "wall_ns\tsteps\tentries\treason\timage\tpolicy\tovl\ttask\tcode_image\tcode_src_ea\tcode_size\tentry\tls1330\tentry_hash\tls4000_hash\tfirst16\n";
    CHECK(strstr(text, hdr));
    char f[64];
    const char* r100 = row_for(text, "\t0x00100\t");
    CHECK(r100);
    field(r100, 1, f, sizeof f); CHECK(!strcmp(f, "10"));
    field(r100, 2, f, sizeof f); CHECK(!strcmp(f, "1"));
    field(r100, 3, f, sizeof f); CHECK(!strcmp(f, "unlifted"));
    field(r100, 4, f, sizeof f); CHECK(!strcmp(f, "101"));
    field(r100, 12, f, sizeof f); CHECK(!strcmp(f, "0x00004000"));
    field(r100, 15, f, sizeof f); CHECK(!strcmp(f, "A0A1A2A3A4A5A6A7A8A9AAABACADAEAF"));
    const char* r200 = row_for(text, "\t0x00200\t");
    CHECK(r200);
    field(r200, 1, f, sizeof f); CHECK(!strcmp(f, "12"));
    field(r200, 3, f, sizeof f); CHECK(!strcmp(f, "verified-mismatch"));
    CHECK(row_for(text, "\t0x00204\t") == NULL);
    CHECK(row_for(text, "\t0x00300\t") && row_for(text, "\t0x00400\t") && row_for(text, "\t0x00500\t"));
    const char* r600 = row_for(text, "\t0x00600\t");
    field(r600, 1, f, sizeof f); CHECK(!strcmp(f, "6"));
    field(r600, 2, f, sizeof f); CHECK(!strcmp(f, "3"));
    const char* r4010 = row_for(text, "\t0x04010\t");
    CHECK(r4010);
    field(r4010, 8, f, sizeof f); CHECK(!strcmp(f, "105"));
    field(r4010, 9, f, sizeof f); CHECK(!strcmp(f, "0x00ABC000"));
    for (int i = 0; i < 4; i++) {
        char key[32]; snprintf(key, sizeof key, "\tunlifted\t%d\t", 900 + i);
        const char* row = row_for(text, key);
        CHECK(row);
        field(row, 1, f, sizeof f); CHECK(!strcmp(f, "60000"));
        field(row, 2, f, sizeof f); CHECK(!strcmp(f, "20000"));
    }
    /* Rows are sorted by descending wall time. */
    unsigned long long last = ~0ull;
    const char* p = strstr(text, hdr) + strlen(hdr);
    while (*p) {
        unsigned long long wall = strtoull(p, NULL, 10);
        CHECK(wall <= last); last = wall;
        p = strchr(p, '\n'); if (!p) break; p++;
    }

    /* Cost of one attributed interpreted run versus disabled-path checks. */
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 1000000; i++) {
        spu_interp_attr_begin(&ctx, 0x100 + (uint32_t)(i & 15) * 4, SPU_IATTR_DIRECT, &outer);
        spu_interp_attr_add_steps(1);
        spu_interp_attr_end(&outer);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e6;
    printf("PASS on: %u checks; %.0f ns per attributed run\n", checks, ns);
    return 0;
}
