#ifndef PS3RECOMP_RSX_FIFO_BUSY_H
#define PS3RECOMP_RSX_FIFO_BUSY_H
/* Opt-in busy accounting for the RSX FIFO consumer thread
 * (PS3RECOMP_RSX_FIFO_BUSY=1; off by default, and then every call below is a
 * single load and return).
 *
 * The consumer thread is the first thread that enters an idle wait
 * (rsx_fifo_busy_idle_begin, called from cellGcm_fifo_kick_wait). Spans entered
 * on any other thread are ignored. Spans nest: time is charged to the innermost
 * open span only, so the categories are exclusive and add up to the thread's
 * wall time. Time outside every span is "other".
 *
 * An update is one FIFO flip (rsx_fifo_busy_flip). About every two seconds
 * (PS3RECOMP_RSX_FIFO_BUSY_PERIOD_MS overrides) the consumer prints one
 * "[rsx fifo busy]" line: per-update wall and busy time (wall minus the idle
 * waits), the per-update busy distribution, and each category's exclusive
 * milliseconds and span count per update.
 *
 * Extensions name up to RSX_FIFO_BUSY_EXTENSION_SLOTS categories of their own
 * (rsx_fifo_busy_category) for work they do on the consumer thread, such as a
 * native handoff callback. Names are printed as given; the SDK attaches no
 * meaning to them. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum rsx_fifo_busy_builtin {
    RSX_FIFO_BUSY_OTHER = 0,     /* consumer time outside every span below */
    RSX_FIFO_BUSY_IDLE_PUT,      /* idle wait after a pass that reached put */
    RSX_FIFO_BUSY_IDLE_SEMA,     /* idle wait after a pass held on a semaphore acquire */
    RSX_FIFO_BUSY_IDLE_WRITER,   /* idle wait after a pass held for a native writer to publish */
    RSX_FIFO_BUSY_IDLE_OTHER,    /* idle wait after any other stop (park, early return) */
    RSX_FIFO_BUSY_DRAIN,         /* FIFO walk and register writes not in a nested span */
    RSX_FIFO_BUSY_DRAW,          /* draw and clear methods handed to the draw engine */
    RSX_FIFO_BUSY_SYNC,          /* report/label and inline-image barriers */
    RSX_FIFO_BUSY_FLUSH,         /* backend encode/commit (and any wait) that is not a present */
    RSX_FIFO_BUSY_HANDOFF,       /* native handoff and watch callbacks */
    RSX_FIFO_BUSY_PRESENT,       /* presenting a guest frame, except the drawable wait */
    RSX_FIFO_BUSY_DRAWABLE,      /* present: frames-in-flight bound and drawable acquisition */
    RSX_FIFO_BUSY_PUMP,          /* window and event pump */
    RSX_FIFO_BUSY_PRESENT_HOST,  /* a host-driven present call, outside the present it makes */
    RSX_FIFO_BUSY_LEASE_WAIT,    /* blocked taking the consumer lease or the native owner lock */
    RSX_FIFO_BUSY_BUILTIN_COUNT
};
#define RSX_FIFO_BUSY_EXTENSION_SLOTS 8

/* Why the last FIFO pass stopped; selects the idle category of the next wait. */
enum rsx_fifo_busy_stop {
    RSX_FIFO_BUSY_STOP_PUT = 0,
    RSX_FIFO_BUSY_STOP_SEMA,
    RSX_FIFO_BUSY_STOP_WRITER,
    RSX_FIFO_BUSY_STOP_OTHER
};

int rsx_fifo_busy_enabled(void);
/* A category id for an extension name (at most 23 characters), the same id
 * for the same name; -1 when disabled, the name is empty or every slot is
 * taken. Thread-safe. */
int rsx_fifo_busy_category(const char* name);
/* Open/close a span of a category on the consumer thread. Closing a category
 * that is not innermost closes the spans inside it too and is counted as
 * unbalanced; closing one that is not open is ignored. */
void rsx_fifo_busy_enter(int category);
void rsx_fifo_busy_leave(int category);
/* The idle wait: the first caller becomes the consumer thread. Leaving it may
 * print the periodic line. */
void rsx_fifo_busy_idle_begin(void);
void rsx_fifo_busy_idle_end(void);
void rsx_fifo_busy_pass_stopped(int stop);
/* One update boundary, counted on the consumer thread. */
void rsx_fifo_busy_flip(void);

/* Tests only: replace the clock and the line sink, and force enablement
 * (resets all state). NULL restores the defaults. */
void rsx_fifo_busy_test_configure(int enabled, uint64_t (*clock_ns)(void),
                                  void (*sink)(const char* line), uint64_t period_ns);

#ifdef __cplusplus
}
#endif
#endif
