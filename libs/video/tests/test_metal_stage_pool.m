/* The draw engine's staging arena: recorded into a pooled shared MTLBuffer,
 * kept across growth, handed to the command buffer at commit and returned to
 * the pool when the GPU completes it. Needs a Metal device; no window.
 *
 * A long run of commits must also reuse a bounded set of arenas, and arenas
 * the pool declines must be deallocated: a run that allocated one arena per
 * commit and freed none filled device memory within minutes. Those phases
 * count arenas made, live and freed through RSX_ENG_COUNTS's site tags, on
 * both the synchronous and the asynchronous commit paths. No game data.
 *
 *   clang -fobjc-arc -I include -I libs/video -I runtime/diagnostics \
 *     libs/video/tests/test_metal_stage_pool.m \
 *     libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp \
 *     libs/video/rsx_fifo_busy.c runtime/diagnostics/critical_trace.c \
 *     runtime/diagnostics/render_ablation.c -lc++ \
 *     -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore \
 *     -o test_metal_stage_pool && ./test_metal_stage_pool
 */
#include "../rsx_metal_backend.m"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

float cellAvconfExt_display_gamma(void) { return 1.0f; }
int cellGcm_native_consumer_enabled(void) { return 0; }
void cellGcm_fifo_kick_notify(void) {}
void rsx_ring_progress_metal_committed(uint64_t ticket, uint64_t records) { (void)ticket; (void)records; }
void rsx_ring_progress_metal_completed(uint64_t ticket, uint64_t status) { (void)ticket; (void)status; }

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

static unsigned long long stage_live(void)
{
    return atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]) -
           atomic_load(&s_eng_site_freed[ENG_SITE_STAGE_BUFFER]);
}

/* One commit: a clear, plus staged bytes so the arena is handed to the
 * command buffer. */
static void commit_with_stage(u32 target, int async, u32 bytes)
{
    const float rgba[4] = {0, 1, 0, 1};
    eng_clear_color(NULL, target, rgba);
    u32 off = 0;
    static u8 junk[2u << 20];
    CHECK(eng_stage_copy(junk, bytes, &off), "stage reserve");
    if (async) {
        const u64 t = eng_submit_async(NULL, RSX_BE_FLUSH_GUEST_REFERENCE);
        CHECK(t != 0, "async ticket");
    } else {
        eng_submit_and_wait(NULL, RSX_BE_FLUSH_GUEST_REFERENCE);
    }
}

static NSUInteger pool_count(void)
{
    pthread_mutex_lock(&s_eng_stage_pool_lock);
    const NSUInteger n = [s_eng_stage_pool count];
    pthread_mutex_unlock(&s_eng_stage_pool_lock);
    return n;
}

/* The FIFO walker records from a PPU host thread: a plain pthread with no
 * autorelease pool. With the pool full of buffers too small for an arena,
 * every recording allocates one and every completion drops one, and a
 * dropped arena must be freed while that thread is still running, not kept
 * by an autoreleased reference only the thread's exit releases. Weak
 * references see which arenas are still alive; the thread counts before it
 * returns, because its exit would free them anyway. */
enum { WALKER_COMMITS = 64 };
static __weak id<MTLBuffer> s_walker_arena[WALKER_COMMITS];
static int s_walker_made, s_walker_live;
static uint64_t s_walker_grew;

static int walker_live(void)
{
    int n = 0;
    for (int i = 0; i < WALKER_COMMITS; i++) if (s_walker_arena[i]) n++;
    return n;
}

static void* pool_less_walker(void* arg)
{
    (void)arg;
    /* Replace the pool's buffers in one step, so a completion still landing
     * from the main thread's commit cannot slip a large one back in. */
    pthread_mutex_lock(&s_eng_stage_pool_lock);
    [s_eng_stage_pool removeAllObjects];
    for (int i = 0; i < ENG_STAGE_POOL_KEEP; i++)
        [s_eng_stage_pool addObject:[s_dev newBufferWithLength:1u << 16
                                                       options:MTLResourceStorageModeShared]];
    pthread_mutex_unlock(&s_eng_stage_pool_lock);
    const uint64_t size0 = [s_dev currentAllocatedSize];
    for (int i = 0; i < WALKER_COMMITS; i++) {
        u32 off = 0;
        if (!eng_stage_reserve(1u << 20, &off)) break;
        s_walker_arena[i] = s_eng_stage_buf;
        if ([s_eng_stage_buf length] > (1u << 16)) s_walker_made++;   /* not a pooled one */
        eng_encode_and_commit(nil);   /* waits for the GPU; drops the arena */
    }
    /* Completion handlers run on Metal's thread: let the last ones land. */
    for (int spin = 0; spin < 200 && walker_live() > 0; spin++) usleep(10000);
    s_walker_live = walker_live();
    const uint64_t size1 = [s_dev currentAllocatedSize];
    s_walker_grew = size1 > size0 ? size1 - size0 : 0;
    return NULL;
}

int main(void)
{
    @autoreleasepool {
        /* Before the first arena: the site tags are read once. */
        setenv("RSX_ENG_COUNTS", "1", 1);
        s_dev = MTLCreateSystemDefaultDevice();
        assert(s_dev);
        s_queue = [s_dev newCommandQueue];
        assert(s_queue);

        /* Staged bytes live in a shared MTLBuffer's contents. */
        u8 bytes[256];
        for (u32 i = 0; i < sizeof bytes; i++) bytes[i] = (u8)(i * 7u + 3u);
        u32 off = ~0u;
        assert(eng_stage_copy(bytes, sizeof bytes, &off));
        id<MTLBuffer> first = s_eng_stage_buf;
        assert(first && s_eng_stage == (u8*)[first contents]);
        assert(memcmp((u8*)[first contents] + off, bytes, sizeof bytes) == 0);

        /* Growing the arena before a commit keeps what was recorded. */
        const u32 grow = s_eng_stage_cap + 1u;
        u32 off2 = 0;
        assert(eng_stage_reserve(grow, &off2));
        assert(s_eng_stage_buf != first && s_eng_stage_cap >= off2 + grow);
        assert(memcmp(s_eng_stage + off, bytes, sizeof bytes) == 0);
        assert(pool_count() == 1);        /* the outgrown arena, never committed */
        id<MTLBuffer> grown = s_eng_stage_buf;

        /* A commit hands the arena to the command buffer and starts afresh;
         * the GPU's completion returns it to the pool. */
        eng_encode_and_commit(nil);
        assert(!s_eng_stage_buf && !s_eng_stage && !s_eng_stage_used && !s_eng_stage_cap);
        for (int i = 0; i < 1000 && pool_count() < 2; i++) usleep(1000);
        assert(pool_count() == 2);

        /* The next recording takes a pooled buffer instead of a new one. */
        assert(eng_stage_copy(bytes, sizeof bytes, &off));
        assert(s_eng_stage_buf == first || s_eng_stage_buf == grown);
        assert(pool_count() == 1);

        /* The pool keeps a bounded number of buffers. */
        for (int i = 0; i < ENG_STAGE_POOL_KEEP + 4; i++)
            eng_stage_buffer_release([s_dev newBufferWithLength:4096
                                                        options:MTLResourceStorageModeShared]);
        assert(pool_count() == ENG_STAGE_POOL_KEEP);

        /* Bounded arenas across long runs of commits, counted by site tag. */
        CHECK(eng_init(NULL, 4, 3) == 0, "backend helpers");
        s_eng_active = 1; s_headless = 1;
        const u32 target = eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 4, NULL, 0);
        CHECK(target != 0, "colour target");

        enum { N = 200 };
        /* Synchronous commits: each waits for its GPU work, so its arena is
         * back in the pool before the next commit reserves one. */
        for (int i = 0; i < N; i++) commit_with_stage(target, 0, 1u << 20);
        fprintf(stderr, "sync: arenas made=%llu freed=%llu live=%llu\n",
                atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]),
                atomic_load(&s_eng_site_freed[ENG_SITE_STAGE_BUFFER]), stage_live());
        eng_site_report();
        CHECK(stage_live() <= ENG_STAGE_POOL_KEEP + 1, "sync commits keep a bounded set of arenas");
        CHECK(!atomic_load(&s_eng_checked_failed), "sync commits checked ok");

        /* Asynchronous commits, drained through completion_status the way the
         * consumer does. In flight at once: at most the fence window. */
        for (int i = 0; i < N; i++) {
            commit_with_stage(target, 1, 1u << 20);
            if ((i & 7) == 7) eng_completion_status(NULL, s_eng_fence_next, 1);
        }
        eng_completion_status(NULL, s_eng_fence_next, 1);
        /* Completion handlers run on Metal's thread: let the last ones land. */
        for (int spin = 0; spin < 100 && stage_live() > ENG_STAGE_POOL_KEEP + 1; spin++) usleep(10000);
        fprintf(stderr, "async: arenas made=%llu freed=%llu live=%llu fence_cbs=%lu submitted=%lu\n",
                atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]),
                atomic_load(&s_eng_site_freed[ENG_SITE_STAGE_BUFFER]), stage_live(),
                (unsigned long)[s_eng_fence_cbs count], (unsigned long)[s_eng_submitted count]);
        eng_site_report();
        CHECK(stage_live() <= ENG_STAGE_POOL_KEEP + 1, "async commits keep a bounded set of arenas");
        CHECK(!atomic_load(&s_eng_checked_failed), "async commits checked ok");

        /* An arena that outgrows the pool's largest is a new allocation; the
         * one it replaces goes back to the pool or is freed, never kept. */
        for (int i = 0; i < 16; i++) commit_with_stage(target, 0, (1u << 20) * (u32)(1 + i % 5));
        for (int spin = 0; spin < 100 && stage_live() > ENG_STAGE_POOL_KEEP + 1; spin++) usleep(10000);
        fprintf(stderr, "growth: arenas made=%llu freed=%llu live=%llu\n",
                atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]),
                atomic_load(&s_eng_site_freed[ENG_SITE_STAGE_BUFFER]), stage_live());
        eng_site_report();
        CHECK(stage_live() <= ENG_STAGE_POOL_KEEP + 1, "growth keeps a bounded set of arenas");
        CHECK(atomic_load(&s_eng_site_freed[ENG_SITE_STAGE_BUFFER]) > 0 ||
              atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]) <= ENG_STAGE_POOL_KEEP + 1,
              "arenas the pool declines are freed");

        /* Arenas the full pool drops on a pool-less walker thread are freed. */
        eng_encode_and_commit(nil);   /* the walker starts with an empty arena */
        assert(!s_eng_stage_buf && !s_eng_stage_used);
        pthread_t walker;
        assert(pthread_create(&walker, NULL, pool_less_walker, NULL) == 0);
        pthread_join(walker, NULL);
        fprintf(stderr, "pool-less walker: %d arenas recorded, %d still live, "
                        "device allocation grew %llu KB (while the thread ran)\n",
                s_walker_made, s_walker_live, (unsigned long long)(s_walker_grew >> 10));
        assert(s_walker_made == WALKER_COMMITS);   /* every recording allocated */
        if (s_walker_live != 0) {
            fprintf(stderr, "FAIL: dropped arenas from a pool-less thread were kept alive\n");
            return 1;
        }

        eng_shutdown(NULL);
        assert(!s_eng_stage_buf && !s_eng_stage && pool_count() == 0);
        printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
        if (!failures) puts("The Metal staging arena records into pooled buffers");
    }
    return failures ? 1 : 0;
}
