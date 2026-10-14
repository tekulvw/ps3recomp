# Bounded audio chronology diagnostic

This diagnostic records metadata about real SPU copies into allocated cellAudio
rings and the host consumer. It does not change pacing/synchronization policy, DMA results, tag
completion, notification behavior, port arithmetic, PCM, gains or routing. It
is disabled by default and requires an explicit trigger path:

```
AUDIO_CHRONOLOGY=1
AUDIO_CHRONOLOGY_TRIGGER=/private/tmp/audio-chronology-arm.trigger
AUDIO_CHRONOLOGY_MS=10000
AUDIO_CHRONOLOGY_LIMIT=32768
```

Set these before process launch, remove the old trigger before launch, and
create a regular trigger file when the desired capture phase begins. The
audio poll path checks for it at most every 250 ms. Hot hooks and extra SDL
queue queries use only atomic active-window admission; they do not take the
metadata lock, read the clock or sample PCM before trigger admission or after
flush. Cold port open/close still maintains the allocation registry before
arming. After flush, even the poll path stops clock/lock work. The default window is ten seconds;
valid overrides are 1–30,000 ms and 1–32,768 metadata records. Invalid config
fails closed. Port allocation metadata is tracked before arming, but PCM is not
sampled and records/budget are not consumed before the trigger is admitted.

Records are kept in a preallocated, globally bounded buffer. A private lock
serializes only diagnostic metadata; it does not protect guest audio bytes or
alter their ownership. No log is printed on each transfer. The audio thread
polls the deadline after submission and notification. Once the window expires,
or cellAudio quits, the bounded buffer and summary are printed to stderr once.
The summary includes dropped events, pending recorded writer-slot brackets, queue-depth
minimum/zero counts and maximum submission interval. Printing the final buffer
can delay execution after the capture; exclude that interval from performance
comparisons. The observer also adds bounded sampling/metadata overhead during
the window, so native timing must remain qualified.

Each actual port open/close increments a diagnostic epoch. Port records describe
its EA, channels (`lsa`), blocks (`cmd`) and bytes per 256-frame block (`size`).
They may also appear as an initial arm-time snapshot. These epochs distinguish
allocation reuse, not guest packets, WWS jobs or logical producer generations.

Event kinds:

| Kind | Meaning |
| --- | --- |
| 1 | Port allocation or arm-time registry snapshot |
| 2 | Before consumption: monotonic read index, slot mask, whole-block raw magnitude nonzero count |
| 3 | Immediately before clearing the consumed block |
| 4 | Immediately after clearing |
| 5 | After publishing the next guest modulo ring index (`arg0`) |
| 6 | Completed real DMA attempt overlapping a registered ring |
| 7 | SDL queue before/after submission (`arg0`/`arg1`), actual result, frames and interval (`value`) |
| 8 | Resolved queue/key and actual notification push result; -2 means no queue was resolved or pushed |
| 9 | First 16 block-tag calls (`cmd=0`) and first 16 timestamp calls (`cmd=1`), arguments/result/error |

DMA records use the existing actual copy-outcome interface. `begin_us` brackets
the request; `copy_us` is sampled immediately after the real copy, before the
bounded diagnostic sample; `us` records the outcome callback. EA/size and LSA
are actual element spans; list transfers produce independent element records.
DMA `index` is a diagnostic sum of matching-slot consumption serials, not a
producer frame index. Context/SPU/PC are runtime snapshots, not an instruction or job-generation
certificate. Flags: 1 = possible same-slot copy/consume overlap; 2 = port epoch
changed mid-attempt; 4 = actual copied PUT disposition. Copy success requires
both the copied flag and result zero. Rejected or malformed attempts have no
copy certificate. Failed DMA tag completion cannot substitute for this result.

`arg0`/`arg1` in a DMA record describe the ring-overlap offset/byte length within
the transfer. `words` gives the complete aligned BE32 words actually sampled;
`nz` counts integer magnitude bits, so it treats both signs of zero as zero and
does not depend on floating-point denormal handling. Partial-word transfers
can have zero sampled words even though bytes were copied. Nonzero counts are
not proof of finite, audible PCM. Flags cover copy/consume overlaps only;
flags zero is not an unambiguous producer certificate. An assessor must also
reject intersecting same-EA/epoch DMA request-through-outcome brackets for
both producers, including equal timestamps at clock resolution. Pending
brackets can describe copies completed after window admission closed; they
are not proof that the physical transfer is still running at flush.
Overlapping reads/writes are explicitly
ambiguous; sampling does not add PCM locks or resolve the underlying race.

The queue depth is measured before and after each SDL submission; the minimum
is over those observations, not an asynchronous device-empty certificate.
Only SDL backends currently expose these queue observations. WASAPI still gets
port/DMA/consume/API records. Notifications are observed without retries. API
observations preserve existing arithmetic and error returns; unknown counters
or output values use UINT64_MAX. A reference mismatch is not corrected by this
diagnostic.

Coverage is SPU ordinary/list DMA publication only. CPU stores, atomics, direct
host writes and arbitrary HLE producers are not all traced. Therefore missing
PUTs do not establish missing production, and zero consumed blocks cannot by
themselves distinguish intended silence, late publication, wrong slots or a
logical ownership error. Exhausted budgets and overlapping/incomplete brackets
also limit conclusions. Match port epoch, byte span, slot and monotonic consumer
index before assessing chronology. Do not infer a unique packet from an EA.

Synthetic verification on macOS (clang and SDL2 headers):

```
python3 tools/test_audio_chronology.py
python3 tools/test_audio_chronology.py --sanitize
```

The fixtures use synthetic committed VM/LS and inert SDL/notification services;
they do not open an audio device or control a game. They exercise real ordinary
and list DMA, zeros/failures, overlaps, port reuse, cursor wrap, cap/drop counts,
concurrent event writers, pre-trigger/disabled poison pointers, host error-state
preservation and actual cellAudio mixer/tag/timestamp behavior.
