/* Synthetic SPU byte-permute tests. The architectural oracle accesses big-endian
 * bytes through word shifts; it does not use SPU_W, host byte aliases, table
 * indices, or a retained scalar helper. No game data is required. */
#include "spu_helpers.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t cases, digest = UINT64_C(1469598103934665603);
static uint32_t random_state = 0xB71E4D29u;
static uint32_t random32(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}
static unsigned arch_byte(u128 v, unsigned byte) {
    return (v._u32[byte / 4] >> (24 - 8 * (byte % 4))) & 255u;
}
static void arch_put(u128* v, unsigned byte, unsigned value) {
    unsigned shift = 24 - 8 * (byte % 4);
    v->_u32[byte / 4] = (v->_u32[byte / 4] & ~(255u << shift)) | ((value & 255u) << shift);
}
static u128 oracle(u128 input, uint32_t count, int rotate) {
    u128 result = {{0}};
    unsigned amount = count & (rotate ? 15u : 31u);
    for (unsigned destination = 0; destination < 16; ++destination) {
        unsigned source = destination + amount;
        if (rotate) source %= 16;
        if (source < 16) arch_put(&result, destination, arch_byte(input, source));
    }
    return result;
}
__attribute__((noinline)) u128 byte_rotqbyi_wrapper(u128 input, int count) {
    return spu_rotqbyi(input, count);
}
__attribute__((noinline)) u128 byte_rotqby_wrapper(u128 input, u128 count) {
    return spu_rotqby(input, count);
}
__attribute__((noinline)) u128 byte_shlqbyi_wrapper(u128 input, int count) {
    return spu_shlqbyi(input, count);
}
static void equal(u128 got, u128 want, const char* operation, uint32_t count) {
    if (memcmp(&got, &want, sizeof got)) {
        fprintf(stderr, "%s mismatch case=%llu count=%08x\n", operation,
                (unsigned long long)cases, count);
        exit(1);
    }
    for (unsigned word = 0; word < 4; ++word) {
        digest ^= got._u32[word]; digest *= UINT64_C(1099511628211);
    }
    ++cases;
}
static void helper_case(u128 input, int count) {
    const u128 rotation = oracle(input, (uint32_t)count, 1);
    const u128 shift = oracle(input, (uint32_t)count, 0);
    /* Poison every non-preferred count word. Only word zero is the count. */
    u128 counts = {._u32 = {(uint32_t)count, ~(uint32_t)count, 0x87654321u, 0xFEDCBA98u}};
    equal(byte_rotqbyi_wrapper(input, count), rotation, "rotqbyi", (uint32_t)count);
    equal(byte_rotqby_wrapper(input, counts), rotation, "rotqby", (uint32_t)count);
    equal(byte_shlqbyi_wrapper(input, count), shift, "shlqbyi", (uint32_t)count);
    /* The assignment form emitted by lifted code must read before writing. */
    u128 alias = input;
    alias = spu_rotqbyi(alias, count); equal(alias, rotation, "rotqbyi alias", (uint32_t)count);
    alias = input;
    alias = spu_shlqbyi(alias, count); equal(alias, shift, "shlqbyi alias", (uint32_t)count);
    alias = input;
    alias = spu_rotqby(alias, counts); equal(alias, rotation, "rotqby A alias", (uint32_t)count);
    alias = counts;
    alias = spu_rotqby(input, alias); equal(alias, rotation, "rotqby B alias", (uint32_t)count);
    alias = input;
    const u128 all_same = oracle(input, input._u32[0], 1);
    alias = spu_rotqby(alias, alias); equal(alias, all_same, "rotqby A=B=D", input._u32[0]);
}
static void helper_cases(void) {
    u128 tagged = {{0}}, inverse = {{0}}, zero = {{0}}, ones;
    memset(&ones, 255, sizeof ones);
    for (unsigned byte = 0; byte < 16; ++byte) {
        arch_put(&tagged, byte, byte + 1); arch_put(&inverse, byte, 255 - byte);
    }
    /* Every signed RI7 value plus repeated effective masks and negative counts. */
    for (int count = -512; count < 512; ++count) {
        helper_case(tagged, count); helper_case(inverse, count);
        helper_case(zero, count); helper_case(ones, count);
    }
    const int extremes[] = {INT_MIN, INT_MIN + 1, INT_MIN + 15, INT_MIN + 31,
                           INT_MAX, INT_MAX - 1, INT_MAX - 15, INT_MAX - 31};
    for (unsigned i = 0; i < sizeof extremes / sizeof *extremes; ++i)
        helper_case(tagged, extremes[i]);
    /* Every source bit and every effective shift count. */
    for (unsigned bit = 0; bit < 128; ++bit) for (unsigned count = 0; count < 32; ++count) {
        u128 input = {{0}}; input._u32[bit / 32] = 1u << (bit % 32);
        helper_case(input, (int)count);
    }
    /* Every byte value at every architectural position, crossing every word. */
    for (unsigned byte = 0; byte < 16; ++byte) for (unsigned value = 0; value < 256; ++value)
        for (unsigned count = 0; count < 32; ++count) {
            u128 input = {{0}}; arch_put(&input, byte, value);
            helper_case(input, (int)count);
        }
    for (unsigned iteration = 0; iteration < 20000; ++iteration) {
        u128 input;
        for (unsigned word = 0; word < 4; ++word) input._u32[word] = random32();
        /* All 32-bit register patterns; immediates are also tested beyond RI7. */
        uint32_t bits = random32(); int count; memcpy(&count, &bits, sizeof count);
        helper_case(input, count);
    }
    printf("BYTE_PERMUTE PASS cases=%llu digest=%016llx\n",
           (unsigned long long)cases, (unsigned long long)digest);
}

#ifndef BYTE_PERMUTE_NO_INTERPRETER
#include "spu_interp.h"
u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; abort(); }
int spu_lifted_entry_maybe(uint32_t a) { (void)a; return 1; }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}
static void instruction(uint8_t* p, uint32_t bits) {
    p[0] = bits >> 24; p[1] = bits >> 16; p[2] = bits >> 8; p[3] = bits;
}
static uint64_t interpreter_cases;
static spu_context ctx;
static void interpreted(unsigned operation, unsigned count_bits, uint32_t reg_count,
                        unsigned alias_mode, unsigned pattern) {
    unsigned ra = 1, rb = 2, rt = alias_mode == 1 ? 1 : alias_mode == 2 ? 2 : 3;
    if (alias_mode == 3) rb = rt = ra;
    spu_context_init(&ctx, 0); ctx.image_id = -1;
    for (unsigned reg = 0; reg < 128; ++reg) for (unsigned word = 0; word < 4; ++word)
        ctx.gpr[reg]._u32[word] = 0x2468ACE0u ^ (reg * 0x1020304u) ^ (word * 0x5A5A5A5Au);
    for (unsigned byte = 0; byte < 16; ++byte)
        arch_put(&ctx.gpr[ra], byte, pattern == 0 ? byte + 1 : pattern == 1 ? 255 - byte : (byte & 1) ? 0x80 : 0x01);
    if (operation == 0) ctx.gpr[rb]._u32[0] = reg_count;
    u128 before[128]; memcpy(before, ctx.gpr, sizeof before);
    int count = count_bits < 64 ? (int)count_bits : (int)count_bits - 128;
    u128 want = oracle(ctx.gpr[ra], operation == 0 ? ctx.gpr[rb]._u32[0] : (uint32_t)count, operation != 2);
    /* Public ISA encodings: RR rotqby op11=0x1DC; RI7 rotqbyi=0x1FC,
     * shlqbyi=0x1FF. Exhaust all seven encoded immediate bits. */
    uint32_t opcode = ((operation == 0 ? 0x1DCu : operation == 1 ? 0x1FCu : 0x1FFu) << 21)
                      | ((operation == 0 ? rb : count_bits) << 14) | (ra << 7) | rt;
    instruction(ctx.ls + 0x100, opcode); instruction(ctx.ls + 0x104, 0);
    spu_interp_run(&ctx, 0x100);
    if (ctx.status != SPU_STATUS_STOPPED_BY_STOP || ctx.pc != 0x108 ||
        memcmp(&ctx.gpr[rt], &want, sizeof want)) {
        fprintf(stderr, "BYTE_PERMUTE interpreted mismatch operation=%u count=%08x alias=%u pattern=%u\n",
                operation, operation == 0 ? reg_count : count_bits, alias_mode, pattern);
        exit(1);
    }
    for (unsigned reg = 0; reg < 128; ++reg) if (reg != rt && memcmp(&ctx.gpr[reg], &before[reg], sizeof(u128))) {
        fprintf(stderr, "BYTE_PERMUTE interpreter clobbered register %u\n", reg); exit(1);
    }
    ++interpreter_cases;
}
static void interpreted_cases(void) {
    for (unsigned operation = 1; operation <= 2; ++operation)
        for (unsigned count = 0; count < 128; ++count) for (unsigned alias = 0; alias < 2; ++alias)
            for (unsigned pattern = 0; pattern < 3; ++pattern)
                interpreted(operation, count, 0, alias, pattern);
    const uint32_t high[] = {0, 0x01020300u, 0x80000000u, 0xFFFFFF00u};
    for (unsigned h = 0; h < sizeof high / sizeof *high; ++h)
        for (unsigned low = 0; low < 256; ++low) for (unsigned alias = 0; alias < 4; ++alias)
            interpreted(0, 0, high[h] | low, alias, low % 3);
    printf("BYTE_PERMUTE interpreter PASS cases=%llu (all RI7 bits, count masks, aliases, non-destination registers)\n",
           (unsigned long long)interpreter_cases);
}
#endif

static double seconds(void) {
    struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}
static void benchmark(void) {
    enum { N = 2000000, SLOTS = 256 };
    u128 inputs[SLOTS], counts[SLOTS];
    for (unsigned slot = 0; slot < SLOTS; ++slot) for (unsigned word = 0; word < 4; ++word) {
        inputs[slot]._u32[word] = random32(); counts[slot]._u32[word] = random32();
    }
    for (unsigned operation = 0; operation < 3; ++operation) for (unsigned round = 0; round < 5; ++round) {
        uint64_t sum = 0; double start = seconds();
        for (unsigned iteration = 0; iteration < N; ++iteration) {
            unsigned slot = iteration & (SLOTS - 1); u128 value;
            if (operation == 0) value = byte_rotqbyi_wrapper(inputs[slot], (int)counts[slot]._u32[0]);
            else if (operation == 1) value = byte_rotqby_wrapper(inputs[slot], counts[slot]);
            else value = byte_shlqbyi_wrapper(inputs[slot], (int)counts[slot]._u32[0]);
            sum += value._u32[iteration & 3];
        }
        printf("BYTE_PERMUTE BENCH op=%u round=%u n=%u ns_per_op=%.6f checksum=%016llx\n",
               operation, round, N, (seconds() - start) * 1e9 / N, (unsigned long long)sum);
    }
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--bench")) benchmark();
    else {
        helper_cases();
#ifndef BYTE_PERMUTE_NO_INTERPRETER
        interpreted_cases();
#endif
    }
    return 0;
}
