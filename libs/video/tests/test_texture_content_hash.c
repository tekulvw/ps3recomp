/* The texture cache's content hash (eng_hash_bytes): every single-byte and
 * single-bit change it is used to notice changes it, words moved between
 * positions change it, the length is part of it, and the engine's cache
 * still re-uploads a texture whose bytes changed. Prints its speed against
 * the one-chain word FNV it replaced. Synthetic inputs only. */
#define main engine_retention_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include <time.h>

static unsigned checks, failures;
#define T(c, msg) do { checks++; if (!(c)) { failures++; if (failures < 20) fprintf(stderr, "FAIL %s\n", msg); } } while (0)

static u64 old_fnv(const u8* src, u32 span)
{
    u64 h = 1469598103934665603ull;
    u32 i = 0;
    for (; i + 8 <= span; i += 8) { u64 w; memcpy(&w, src + i, 8); h ^= w; h *= 1099511628211ull; }
    for (; i < span; i++) { h ^= src[i]; h *= 1099511628211ull; }
    return h;
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void sensitivity(void)
{
    static const u32 sizes[] = { 1, 7, 8, 9, 31, 32, 33, 40, 63, 64, 65, 255, 4096 + 5 };
    u8 buf[4096 + 8];
    for (u32 s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        const u32 n = sizes[s];
        for (u32 i = 0; i < n; i++) buf[i] = (u8)(i * 37u + 11u);
        const u64 base = eng_hash_bytes(buf, n);
        T(eng_hash_bytes(buf, n) == base, "the same bytes hash the same");
        int all_bytes = 1, all_bits = 1;
        for (u32 i = 0; i < n; i++) {
            const u8 keep = buf[i];
            buf[i] ^= 0xFF;
            all_bytes &= eng_hash_bytes(buf, n) != base;
            buf[i] = keep;
            /* Every bit of the first 40 bytes and of the last one. */
            if (i < 40 || i == n - 1)
                for (u32 b = 0; b < 8; b++) {
                    buf[i] ^= (u8)(1u << b);
                    all_bits &= eng_hash_bytes(buf, n) != base;
                    buf[i] = keep;
                }
        }
        T(all_bytes, "every single-byte change changes the hash");
        T(all_bits, "every single-bit change changes the hash");
    }
    /* Words trading places: across lanes, within a lane (64 bytes apart), and
     * into the tail words. */
    for (u32 i = 0; i < 128; i++) buf[i] = (u8)(i * 13u + 5u);
    const u64 base = eng_hash_bytes(buf, 128);
    static const u32 pairs[][2] = { { 0, 8 }, { 0, 64 }, { 8, 16 }, { 24, 120 } };
    for (u32 p = 0; p < 4; p++) {
        u8 tmp[8], a[8];
        memcpy(a, buf, sizeof a);
        memcpy(tmp, buf + pairs[p][0], 8);
        memmove(buf + pairs[p][0], buf + pairs[p][1], 8);
        memcpy(buf + pairs[p][1], tmp, 8);
        T(eng_hash_bytes(buf, 128) != base, "two words trading places change the hash");
        memcpy(buf + pairs[p][1], buf + pairs[p][0], 8);
        memcpy(buf + pairs[p][0], tmp, 8);
        (void)a;
    }
    T(eng_hash_bytes(buf, 128) == base, "and trading back restores it");
    /* Zero bytes of different lengths. */
    static u8 zero[512];
    u64 seen[512];
    int distinct = 1;
    for (u32 n = 0; n < 512; n++) {
        seen[n] = eng_hash_bytes(zero, n);
        for (u32 k = 0; k < n && distinct; k++) distinct &= seen[k] != seen[n];
    }
    T(distinct, "zero bytes of every length up to 512 hash differently");
}

/* Through the engine: a texture whose bytes change is uploaded again. */
static void cache(void)
{
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    memset(vm_base + TEX_OFFSET, 0x31, 4 * 4 * 4);
    draw_triangle();
    const int creates = stub.n_texture_create;
    T(creates >= 1, "first bind uploads");
    rsx_draw_engine_present();
    draw_triangle();
    T(stub.n_texture_create == creates, "unchanged bytes are not uploaded again");
    vm_base[TEX_OFFSET + 37] ^= 0x40;
    rsx_draw_engine_present();
    draw_triangle();
    T(stub.n_texture_create == creates + 1, "a changed byte uploads again");
    engine_down();
}

static void speed(void)
{
    const u32 n = 64u << 20;
    u8* big = malloc(n);
    if (!big) return;
    for (u32 i = 0; i < n; i++) big[i] = (u8)(i * 2654435761u >> 24);
    volatile u64 sink = 0;
    double t0 = now_s();
    for (int r = 0; r < 4; r++) sink ^= old_fnv(big, n);
    const double old_s = now_s() - t0;
    t0 = now_s();
    for (int r = 0; r < 4; r++) sink ^= eng_hash_bytes(big, n);
    const double new_s = now_s() - t0;
    printf("hash speed: one-chain FNV %.0f MB/s, eight-lane %.0f MB/s (%.1fx)\n",
           4.0 * n / old_s / 1e6, 4.0 * n / new_s / 1e6, old_s / new_s);
    (void)sink;
    free(big);
}

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES); ppu_vm_size = GUEST_BYTES;
    sensitivity();
    cache();
    speed();
    free(vm_base);
    printf("texture content hash: checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
