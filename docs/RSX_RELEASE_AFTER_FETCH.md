# Releases land after the walker has fetched past them

A label release (`NV406E` `0x6C`, `NV4097` `0x1D70`/`0x1D74`) and a `GET_REPORT`
(`0x1800`) tell the guest that the GPU has got past a point in the command
stream. A title may react by recycling memory that it knows the GPU will not
read again. On the hardware, the front end fetches commands well ahead of the
point where a release writes its label: by the time the label lands, the
front end has read the rest of the segment that held the release, including
the branch out of it. A title can therefore recycle the whole segment, branch
included, as soon as it sees the label.

## The problem

With deferred reports and early release (`RSX_EARLY_LABEL_RELEASE.md`), the
FIFO walker writes a label the moment it decodes the release, and only then
reads the words after it. A guest producer that polls the label (a PPU
thread, or an SPU job polling it by DMA) can rewrite those words in between.
On a loaded host the walker's thread can lose its CPU right after the write,
so the producer always wins that race eventually. The walker then follows a
branch the producer has already re-pointed at a block it has not filled yet,
and decodes stale data as commands:

- `[gcm checked REF] unknown word …`, `malformed-reference`;
- `FIFO JUMP to unmapped IO …` / `FIFO resync (unmapped-JUMP)`;
- a semaphore context set to a float-looking value.

After that, no further labels are released. A job that polls one of them
waits until the SPU identical-transfer watchdog halts it, 30 seconds later.

Seen in MAG: the derail sat right after a segment ending in a `0x1D6C`/`0x1D74`
pair and a `JUMP` (`IO 0x1B75B8` release, `0x1B75C0` `JUMP` into data at
`0x80310`). Display stopped there; about 30 s later the SPU job polling the
label at `0xB00004C0` was halted. It happened only under heavy host load
(load average 8–10 on an 8+4-core Mac).

The same symptoms have a second cause, a walker that catches up with an SPU
write still in progress. `SPU_PUT_LINE_ORDER.md` describes it and its rule.

## Rule

During a FIFO pass, the guest-visible write of a label or report is **held**:
the walker queues it and goes on fetching. Held writes are applied, in stream
order and exactly as they would have been, at the first of:

1. the walker **takes a control transfer**: a `JUMP` (not a park), a `CALL` or
   a `RET`. The branch word has been read, and `GET` has been published for
   a `JUMP`, before any held write lands;
2. the walker is about to do something that reads or orders against label
   memory or guest-visible progress: an `NV406E` acquire (`0x68`), a
   `SET_REFERENCE` (`0x50`), a flip;
3. the walker has fetched `GCM_RELEASE_FETCH_WINDOW` bytes (1 MiB) past the
   first held write without a branch, so a long linear stretch cannot hold
   a release for long;
4. the pass ends for any reason: `put` reached, a park, an acquire that
   blocks, a failure, the word budget.

The held queue grows on demand. A segment can release thousands of times
before its branch: MAG's Training stream overflowed a fixed queue of 1024.
Every release takes at least two words, so the fetch window bounds the queue
at 128 Ki entries. If the queue cannot grow, the held writes are applied
first.

A forced flush, at the fetch window or when the queue cannot grow, lands
releases before the walker has left their segment. That reopens the race for
those releases. Each forced flush is counted and reported, up to eight times,
as `[release hold] fetch window …` or `[release hold] hold full …`.

Nothing else changes:

- The values, their encodings, the report words and their order, the
  SPURS line-commit kick, and the `GET`-leads-the-release rule are the same.
  Only the moment of the write moves, to a point no later than the end of the
  pass that decoded it.
- Each write is validated when it is queued, so a bad destination fails at
  the same point as before.
- Completion receipts (checked `SET_REFERENCE`) carry no guest effect and are
  not held. Neither is anything published outside a pass, such as API-level
  barriers or completion processing between passes.

`PS3RECOMP_GCM_RELEASE_AFTER_FETCH=0` turns the hold off and restores
publication at the release itself. It is read once.

## Cost

A held label lands, at the latest, when the walker reaches the next branch.
In segment-linked streams this is a few words later. Otherwise it is at most
1 MiB of linear commands later, or at the end of the pass. Producers waiting
on a label see it microseconds later, never a frame later.

## Test

`python3 tools/test_gcm_release_after_fetch.py` drives the real walker with a
synthetic producer. The producer reacts to a label inside the label write
itself, which is the worst case of a walker that is descheduled right after
publishing. It recycles the segment by re-pointing its closing `JUMP`, `CALL`
or `RET` at an unfilled block. The test checks that the walker still follows
the branch it was given, that `GET` leads the release, stream order, that an
acquire of a just-released label does not wait on itself, the fetch window,
and that the switch off reproduces the race.
