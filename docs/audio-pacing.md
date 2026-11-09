# cellAudio block pacing

The mixing thread reads one block (256 frames) from every started port, sends the period
notification and submits the mix to the host device. Guest mixers that are notified once per
period and write the next block just in time depend on those reads being evenly spaced: one every
256 frames of the 48 kHz clock, about 5.33 ms.

## The problem with reads tied to device room

Host devices drain their queue in chunks. Through SDL, CoreAudio takes 512 frames (two blocks) at a
time. The previous pacing read a guest block whenever the device queue had room, within a cap of
four blocks. Each two-block pull therefore produced two guest reads about 1 ms apart, then none for
about 10 ms. A synthetic probe against the real CoreAudio device on macOS measured 50% of read
intervals under 2 ms.

A writer that keeps several blocks ahead of the read index does not notice. A writer that produces
exactly one block per notification does: it has not written the second block of each pair yet, so
about half of what it plays is silence. In one title, the movie mixer filled only 97 to 115 of 188
blocks per second this way, and its audio sounded broken up. Its other audio engine, which writes
seven blocks ahead, was unaffected.

## Clock pacing (default)

`audio_pace` schedules guest reads on the monotonic clock: block *k* is read at
`epoch + k * 256 / 48000 s`. The device pull size has no effect on that schedule. The cushion
for the device's chunked pulls is made of silence, queued once at the device, rather than guest
blocks read early:

| Parameter | POSIX (SDL) | Meaning |
|---|---|---|
| prime | 4 blocks (21.3 ms) | silence queued before the first read, and again after the device returns |
| low | 1 block | below this at a read, one more silence block is queued (device clock faster than ours) |
| high | 12 blocks (64 ms) | at or above this, the schedule moves one period later instead of reading (device slower than ours, or stalled) |

Against the real CoreAudio device, the queue holds 1024 to 1280 frames at each read: 21 to 27 ms of
output latency. The previous pacing held about 768 frames, so the change adds about 5 to 10 ms. Over
15 s the probe saw no read interval under 2 ms and no silence beyond the initial prime.

Unchanged from before:
- With no device, or with SDL's dummy driver and `PS3RECOMP_AUDIO_DUMMY_CLOCK=1`, reads are paced
  on the clock alone.
- A suspension, or a stall longer than four periods, drops the backlog instead of reading it in a
  burst.
- A stalled device stops the guest once twelve blocks are queued.

## Windows

The WASAPI loop had the same problem. It read a guest block whenever the 20 ms endpoint buffer had
256 frames of room, and shared-mode WASAPI releases room one device period at a time (about 10 ms,
480 frames). That gave about two reads back to back per period.

With clock pacing, the Windows loop uses the same `audio_pace`. It requests a 40 ms endpoint buffer
and keeps half of it queued as silence:

| Parameter | WASAPI | Meaning |
|---|---|---|
| prime | half the endpoint buffer, in whole blocks (about 20 ms) | as above |
| low | 1 block | as above |
| high | buffer minus one block | a block needs 256 frames of room |

Between reads the Windows loop waits on a high-resolution waitable timer (Windows 10 1803 and
later) until the next read is due. `Sleep(1)` lasts a full 15.6 ms system tick unless the process
has raised the timer resolution, and Windows 11 may ignore that request for an occluded or
minimized window. A schedule woken that late would read three blocks back to back. The loop logs
which timer it got at startup. On every platform, reads are never less than half a period apart,
however late the thread wakes; the schedule catches up at that spacing.

A build with `PS3RECOMP_AUDIO_USE_SDL2` on Windows uses the SDL parameters. Test mode 11 checks this
WASAPI shape against a simulated device: 480-frame pulls every 10 ms and 1 ms polling. The shape has
not yet been measured against a real Windows endpoint.

## Switch

| Variable | Values | Effect |
|---|---|---|
| `PS3RECOMP_AUDIO_PACING` | unset (default): clock pacing; `queue`: the previous queue-gated pacing | Selects how guest block reads are paced. `queue` is kept for A/B comparisons |

## Diagnostics

With `AUDIO_DIAGNOSTICS=1` the once-a-second `[audio-diag]` lines also report:

| Field | Line | Meaning |
|---|---|---|
| `read_gap_us=[min,median,max]` | summary | intervals between consecutive guest block reads in the window, in microseconds (on Windows from the performance counter, no longer from `GetTickCount64`) |
| `short_gaps` | summary | intervals under 2 ms: reads that a one-block-per-notification writer cannot keep up with |
| `refilled_prev` | per port | reads where the slot read one period earlier already holds PCM again. Normal for a writer that runs ahead and refills freed slots. For a writer that fills the next slot just in time, it means the write arrived after the read it was meant for |

If `read_gap_us` stays near 5333 and `short_gaps` is 0 while a just-in-time port still reads
silence, the reader is not the cause: the writer is late.

## Tests

`python tools/test_posix_audio_pacing.py` runs the real POSIX mixing thread against a
deterministic clock and an inert SDL device, under both pacings. Mode 10 models CoreAudio's
two-block pulls:
- clock pacing must keep every read at least 4 ms after the previous one, with no device
  starvation;
- the queue pacing is asserted to read in pairs, which documents the defect.

Mode 11 drives `audio_pace` alone with the shape of a WASAPI endpoint. Mode 12 wakes the pacer
0 to 3 ms late at every wait and checks that reads stay at least half a period apart and keep the
long-run rate.
