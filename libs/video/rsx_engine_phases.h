/* RSX_ENGINE_PHASES=1: wall time the draw engine spends in a few phases of
 * the FIFO walker's work, printed as per-frame averages every 60 frames.
 * Diagnostic only: off, each probe is one predictable branch on a cached
 * flag and no clock is read. The engine runs on one thread, so the
 * accumulators are plain statics. */
#ifndef RSX_ENGINE_PHASES_H
#define RSX_ENGINE_PHASES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ENG_PH_DRAW,        /* sink_end: one draw, everything included */
    ENG_PH_BIND,        /* texture units resolved for a draw */
    ENG_PH_ZDEPTH,      /* a depth target stored to guest memory */
    ENG_PH_ZD_FLUSH,    /*   its report barrier and flush */
    ENG_PH_ZD_READ,     /*   its backend readback */
    ENG_PH_ZD_STORE,    /*   its conversion into guest memory */
    ENG_PH_SURF_GUEST,  /* a colour surface stored to guest memory */
    ENG_PH_TEX_HASH,    /* full content hashes of cached textures */
    ENG_PH_TEX_UPLOAD,  /* texture decode and upload */
    ENG_PH_FLUSH,       /* rsx_draw_engine_flush (report barrier + wait) */
    ENG_PH_FLUSH_CHK,   /* rsx_draw_engine_flush_checked */
    ENG_PH_ASYNC,       /* rsx_draw_engine_submit_async */
    ENG_PH_PRESENT,     /* eng_present */
    ENG_PH_COUNT
};

static const char* const s_eng_ph_names[ENG_PH_COUNT] = {
    "draw", "bind", "zdepth", "zd_flush", "zd_read", "zd_store", "surf_guest",
    "tex_hash", "tex_upload", "flush", "flush_chk", "async", "present"
};

static int s_eng_ph_on = -1;
static uint64_t s_eng_ph_ns[ENG_PH_COUNT], s_eng_ph_n[ENG_PH_COUNT];
static uint32_t s_eng_ph_frames;

static inline int eng_ph_enabled(void)
{
    if (s_eng_ph_on < 0) {
        const char* e = getenv("RSX_ENGINE_PHASES");
        s_eng_ph_on = e && *e && strcmp(e, "0") != 0;
    }
    return s_eng_ph_on;
}

static inline uint64_t eng_ph_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define ENG_PH_BEGIN(v) const uint64_t v = eng_ph_enabled() ? eng_ph_now() : 0
#define ENG_PH_END(phase, v) do { if (v) { s_eng_ph_ns[phase] += eng_ph_now() - (v); \
                                           s_eng_ph_n[phase]++; } } while (0)

/* Draw stages: the time from one DRAW_OBSERVER_STAGE mark to the next, by
 * stage name (string literals, so the pointer is the key). */
enum { ENG_PH_STAGES = 32 };
static const char* s_eng_ph_stage[ENG_PH_STAGES];
static uint64_t s_eng_ph_stage_ns[ENG_PH_STAGES];
static int s_eng_ph_nstages;
static const char* s_eng_ph_cur;
static uint64_t s_eng_ph_mark;

static inline void eng_ph_stage(const char* name)
{
    const uint64_t now = eng_ph_now();
    if (s_eng_ph_cur) {
        int i = 0;
        while (i < s_eng_ph_nstages && s_eng_ph_stage[i] != s_eng_ph_cur) i++;
        if (i == s_eng_ph_nstages && i < ENG_PH_STAGES) s_eng_ph_stage[s_eng_ph_nstages++] = s_eng_ph_cur;
        if (i < ENG_PH_STAGES) s_eng_ph_stage_ns[i] += now - s_eng_ph_mark;
    }
    s_eng_ph_cur = name;
    s_eng_ph_mark = now;
}

static inline void eng_ph_frame(void)
{
    if (!eng_ph_enabled() || ++s_eng_ph_frames < 60) return;
    char line[1024];
    int n = snprintf(line, sizeof line, "[rsx phases] per frame over %u:", s_eng_ph_frames);
    for (int i = 0; i < ENG_PH_COUNT && n > 0 && (size_t)n < sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.3fms/%.1f", s_eng_ph_names[i],
                      (double)s_eng_ph_ns[i] / 1e6 / s_eng_ph_frames,
                      (double)s_eng_ph_n[i] / s_eng_ph_frames);
    fprintf(stderr, "%s\n", line);
    n = snprintf(line, sizeof line, "[rsx phases] draw stages per frame:");
    for (int i = 0; i < s_eng_ph_nstages && n > 0 && (size_t)n < sizeof line; i++)
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.3fms", s_eng_ph_stage[i],
                      (double)s_eng_ph_stage_ns[i] / 1e6 / s_eng_ph_frames);
    fprintf(stderr, "%s\n", line);
    memset(s_eng_ph_stage_ns, 0, sizeof s_eng_ph_stage_ns);
    memset(s_eng_ph_ns, 0, sizeof s_eng_ph_ns);
    memset(s_eng_ph_n, 0, sizeof s_eng_ph_n);
    s_eng_ph_frames = 0;
}

#endif
