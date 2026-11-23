/*
 * ps3recomp - cellAudio host timing counters (docs/audio-pacing.md, "Host timing counters")
 *
 * Always on: the mixing thread counts, with relaxed atomics, the timing events its pacing and
 * host device already handle, once per period at most (never per sample). An embedding
 * application polls them, for example to summarise a session's audio health. The counters are
 * cumulative for the whole process, across cellAudioQuit/cellAudioInit.
 *
 * Where each field is counted (libs/audio/cellAudio.c):
 *   periods          a mixed guest block the host device accepted (SDL_QueueAudio succeeded, or
 *                    WASAPI took at least one of its frames). Silence blocks are not periods.
 *   underruns        audio_pace found the device queue below its low-water mark at a read and
 *                    queued a silence block: the device ran ahead of the guest clock.
 *   starved_frames   frames of that low-water silence (256 per underrun). The silence primed once
 *                    at start, and again after a device returns, is not counted.
 *   late_periods     audio_pace moved the schedule one period later at the high-water mark (the
 *                    device is slower than the clock, or stalled), or a read happened a whole
 *                    period or more after it was due (the host woke the mixer late).
 *   dropped_periods  periods of backlog discarded after a stall or suspension longer than four
 *                    periods (every pacing on every host), plus WASAPI submissions the endpoint
 *                    had no room for (each trimmed submission counts one).
 *   device_errors    host backend initialisation failed at cellAudioInit (SDL init or open,
 *                    any WASAPI step), SDL_QueueAudio returned an error, or WASAPI's
 *                    GetCurrentPadding started failing (device invalidated; counted once per
 *                    failing spell, not per poll).
 *   clock_frames     256 per guest block read after the first one.
 *   clock_ns         monotonic nanoseconds from the first guest block read to the latest one.
 *                    clock_frames - clock_ns * sample_rate / 1e9 is the drift of the guest audio
 *                    clock against the nominal 48 kHz (positive: reads ran fast).
 */
#ifndef PS3EMU_AUDIO_HOST_STATS_H
#define PS3EMU_AUDIO_HOST_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3RECOMP_AUDIO_HOST_STATS_VERSION 1

/* Like struct stat and stat(), the struct tag and the getter share one name, so the type is
 * always written `struct ps3recomp_audio_host_stats` (C and C++). */
struct ps3recomp_audio_host_stats {
    uint32_t version;          /* PS3RECOMP_AUDIO_HOST_STATS_VERSION */
    uint32_t sample_rate;      /* 48000 */
    uint64_t periods;          /* 256-frame mix blocks delivered to the host device */
    uint64_t underruns;        /* times the host device/queue ran dry or the low-water mark injected silence */
    uint64_t starved_frames;   /* frames of silence the host path filled in for missing audio */
    uint64_t late_periods;     /* periods the pacing schedule slipped (high-water slip, late mix) */
    uint64_t dropped_periods;  /* periods discarded: backlog drop after a stall, device had no room (round up frames/256) */
    uint64_t device_errors;    /* open failures, queue/submit errors, GetCurrentPadding failures, device loss */
    uint64_t clock_frames;     /* frames the guest consumed through the paced reads since audio started */
    uint64_t clock_ns;         /* monotonic ns elapsed over the same span */
    char backend[16];          /* "sdl2", "wasapi", "dummy", "none" */
};

/* Fills *out; returns 0, or -1 (and zeroes *out except version) when audio never started, that
 * is before the first guest block read. Thread-safe, lock-free, cheap. */
int ps3recomp_audio_host_stats(struct ps3recomp_audio_host_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_AUDIO_HOST_STATS_H */
