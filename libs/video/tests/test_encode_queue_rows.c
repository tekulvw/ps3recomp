/* Synthetic current-ABI row-mask ownership and refused-rebind replay.
 * No register-file engine, Vulkan, shader compiler or game inputs. */
#include "rsx_encode_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS (RSX_DSP_NUM_CONSTANTS / 32u)
#define BYTES ((RSX_DSP_NUM_CONSTANTS + 2u) * 16u)
static unsigned failures, row_calls, whole_calls;
static u8 expected_data[BYTES];
static u32 expected_mask[WORDS], expected_bytes;
static int expected_sparse;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static int start(void* user, u32 width, u32 height)
{ (void)user; (void)width; (void)height; return 0; }
static void stop(void* user) { (void)user; }
static int refuse_rebind(void* user) { (void)user; return 0; }
static void whole(void* user, const void* data, u32 bytes)
{
    (void)user; whole_calls++;
    CHECK(!expected_sparse && bytes == expected_bytes);
    CHECK(!memcmp(data, expected_data, bytes));
}
static void rows(void* user, const void* data, u32 bytes, const u32* mask)
{
    if (!rsx_be_row_mask_fits(mask, bytes)) { whole(user, data, bytes); return; }
    row_calls++;
    CHECK(expected_sparse && bytes == expected_bytes);
    CHECK(!memcmp(mask, expected_mask, sizeof expected_mask));
    const u8* src = (const u8*)data;
    for (u32 row = 0; row < RSX_DSP_NUM_CONSTANTS; ++row)
        if ((mask[row / 32u] >> (row % 32u)) & 1u)
            CHECK(!memcmp(src + row * 16u, expected_data + row * 16u, 16u));
    CHECK(!memcmp(src + bytes - 32u, expected_data + bytes - 32u, 32u));
}
static rsx_draw_backend backend = {
    .init = start, .shutdown = stop, .bind_vs_constants = whole,
    .bind_vs_constant_rows = rows, .rebind_vs_constants = refuse_rebind
};

static void one(const rsx_draw_backend* queue, unsigned variant)
{
    u8 data[BYTES]; u32 mask[WORDS] = {0};
    for (u32 i = 0; i < BYTES; ++i) data[i] = (u8)(i * 7u + variant * 23u);
    if (variant == 0) { /* disjoint runs across word and last-row boundaries */
        mask[0] = 0x80000003u; mask[1] = 0x80000001u; mask[15] = 0xC0000000u;
    } else if (variant == 2) {
        for (u32 i = 0; i < WORDS; ++i) mask[i] = ~0u;
    } else if (variant == 3) {
        mask[0] = 1u << 4; /* does not fit a four-row block: whole-bind fallback */
    }
    const u32 bytes = variant == 3 ? 96u : BYTES;
    memcpy(expected_data, data, sizeof data);
    memcpy(expected_mask, mask, sizeof mask);
    expected_bytes = bytes;
    expected_sparse = variant != 3;
    const unsigned before = row_calls + whole_calls;
    backend.bind_vs_constant_rows(NULL, data, bytes, mask);
    /* Hold the backend so the consumer cannot observe the caller's buffers
     * until after both data and mask have been overwritten. */
    rsx_encq_lock();
    queue->bind_vs_constant_rows(queue->user, data, bytes, mask);
    CHECK(queue->rebind_vs_constants(queue->user) == 1);
    memset(data, 0xEE, sizeof data); memset(mask, 0xAA, sizeof mask);
    rsx_encq_unlock();
    rsx_encq_drain();
    CHECK(row_calls + whole_calls == before + 3u); /* direct, queued, replay */
    /* Replay once more after the queue payload has retired: mask and bytes
     * must now belong to the consumer's retained constant state. */
    CHECK(queue->rebind_vs_constants(queue->user) == 1);
    rsx_encq_drain();
    CHECK(row_calls + whole_calls == before + 4u);
}
int main(void)
{
    setenv("PS3RECOMP_RSX_ENCODE_THREAD", "1", 1);
    const rsx_draw_backend* queue = rsx_encq_wrap(&backend, NULL);
    CHECK(queue != NULL);
    if (!queue || queue->init(queue->user, 8, 8)) return 2;
    for (unsigned variant = 0; variant < 4; ++variant) one(queue, variant);
    queue->shutdown(queue->user); rsx_encq_unwrap();
    printf("encode queue rows: row_calls=%u whole_calls=%u failures=%u\n", row_calls, whole_calls, failures);
    return failures ? 1 : 0;
}
