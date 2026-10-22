/* The staging arena pool: a long run of commits reuses a bounded set of
 * shared buffers, and arenas the pool declines are deallocated. A retail
 * title's mission allocated one arena per commit and freed none (7.7 GB of
 * 8 GB device memory after two minutes), so the test counts arenas made,
 * live and freed through RSX_ENG_COUNTS's site tags, on both the
 * synchronous and the asynchronous commit paths. No window or game data.
 * clang -fobjc-arc -I include -I libs/video
 * libs/video/tests/test_metal_stage_pool.m libs/video/rsx_texture_layout.c libs/video/rsx_shader_msl.cpp -lc++
 * -Wl,-dead_strip -framework Metal -framework Cocoa -framework QuartzCore
 * -o /tmp/test_metal_stage_pool */
#include "../rsx_metal_backend.m"
float cellAvconfExt_display_gamma(void){return 1.0f;}
int cellGcm_native_consumer_enabled(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
void rsx_ring_progress_metal_committed(uint64_t ticket,uint64_t records){(void)ticket;(void)records;}
void rsx_ring_progress_metal_completed(uint64_t ticket,uint64_t status){(void)ticket;(void)status;}
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr,"FAIL %s\n",name); } } while (0)

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

/* The FIFO walker is a PPU host thread: a plain pthread, no autorelease
 * pool. With the pool full of arenas too small for a frame, every commit
 * allocates one and every completion drops one; a dropped arena must be
 * freed, not kept by a reference the thread only releases when it exits.
 * So the thread measures before it returns: a join would free them. */
static unsigned long long s_walker_made, s_walker_live;
static void* pool_less_commits(void* arg)
{
    const u32 target = (u32)(uintptr_t)arg;
    const unsigned long long made0 = atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]);
    pthread_mutex_lock(&s_eng_stage_pool_lock);
    [s_eng_stage_pool removeAllObjects];      /* the earlier phases' arenas */
    pthread_mutex_unlock(&s_eng_stage_pool_lock);
    for (int i = 0; i < ENG_STAGE_POOL_KEEP; i++)
        eng_stage_buffer_release([s_dev newBufferWithLength:1u << 16 options:MTLResourceStorageModeShared]);
    for (int i = 0; i < 64; i++) commit_with_stage(target, 0, 1u << 20);
    /* Every commit waited for its GPU work; let the last handlers land. */
    for (int spin = 0; spin < 100 && stage_live() > ENG_STAGE_POOL_KEEP + 1; spin++) usleep(10000);
    s_walker_made = atomic_load(&s_eng_site_made[ENG_SITE_STAGE_BUFFER]) - made0;
    s_walker_live = stage_live();
    return NULL;
}

int main(void)
{
    @autoreleasepool {
        setenv("RSX_ENG_COUNTS", "1", 1);
        s_dev = MTLCreateSystemDefaultDevice(); CHECK(s_dev != nil, "Metal device"); if (!s_dev) return 1;
        s_queue = [s_dev newCommandQueue]; CHECK(eng_init(NULL, 4, 3) == 0, "backend helpers");
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

        /* Dropped arenas from a thread without an autorelease pool, counted
         * while that thread is alive. */
        pthread_t walker;
        CHECK(pthread_create(&walker, NULL, pool_less_commits, (void*)(uintptr_t)target) == 0, "walker thread");
        pthread_join(walker, NULL);
        fprintf(stderr, "pool-less walker: arenas made=%llu live=%llu (while the thread was alive)\n",
                s_walker_made, s_walker_live);
        eng_site_report();
        CHECK(s_walker_made >= 64, "a full pool of small arenas makes every commit allocate");
        CHECK(s_walker_live <= ENG_STAGE_POOL_KEEP + 1, "dropped arenas from a pool-less walker thread are freed");
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
