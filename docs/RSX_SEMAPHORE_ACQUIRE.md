# Semaphore acquire waits for its label

`NV406E` semaphore acquire (method `0x68`, after an offset set with `0x64`)
stops the GPU front end until the label at that offset holds the given value.
A title uses it to keep the GPU out of commands that another agent has not
finished producing. Typical cases are an SPU job filling a reserved hole in
the command buffer, or lighting data that a job writes each frame. The
producer releases the label when it is done, and the GPU goes on.

## Default: an unsatisfied acquire is skipped

The runtime's FIFO walker has always decoded acquires. By default it does not
wait on one: an acquire whose label does not hold the value yet is consumed
and the walker goes on. This keeps a title that acquires a label nothing ever
writes from wedging the drain for good. It is also why a title whose
producers are only sometimes late works almost all the time.

The cost is that the walker can overtake the producer. In MAG the walker
decodes about one acquire every few dozen commands. At normal load every one
of them is already satisfied when the walker reaches it. On a loaded host an
SPU job can be late, and the walker then runs into the hole it guards before
the job has written it. It reads zeros, which are NOPs, and whatever the
memory held before. Words from the previous frame's vertex or constant data
decode as an unmapped JUMP (`0x3F7FFFFF`, `0x3F0E170D`, `0x3FE6907B`), and
rendering stops for good. The dump then shows the region as zero, because the
producer has cleared it in the meantime.

## `GCM_SEMA_ACQUIRE=1`: wait, with a bound

With `GCM_SEMA_ACQUIRE=1`, an unsatisfied acquire ends the drain pass without
being consumed. `get` stays on it, the next pass reads it again, and the
commands behind it are not fetched until the label holds the value.

- **Bound.** `GCM_SEMA_ACQUIRE_MS=<ms>` gives up on one acquire, the same
  offset and value, after it has waited that long. It is then consumed as it
  is without the switch, and the walker goes on. Every timeout is logged:
  `[gcm acquire] gave up after … ms …`. `0` waits for ever.
- **Waits are reported.** An acquire that had to wait and was then satisfied
  is logged with how long it waited, for the first 32 waits and then every
  256th: `[gcm acquire] waited … ms for label …`. A normal-load run logs none.
- **Ordering.** A satisfied acquire is followed by an acquire fence, so on a
  weakly ordered host (arm64) the commands behind it are read after the label
  that released them, never before.
- **Wake-up.** While a pass is stopped on an acquire, an SPU transfer that
  writes the label's word wakes the walker at once. The same thing already
  happens for a park. Other writers, such as the PPU, are seen at the next
  kick or idle wake-up.
- **Held releases.** Releases the walker is holding
  (`RSX_RELEASE_AFTER_FETCH.md`) land before the acquire reads the label, so
  a stream never waits on its own release.
- **Watchdog.** A pass stopped on an acquire is a producer wait. The stuck-GET
  watchdog does not count it.

`GCM_SEMA_ACQUIRE=0`, or the variable unset, keeps the old path: no waiting.
Both variables are read once.

## Test

`python3 tools/test_gcm_semaphore_acquire.py` drives the real walker. An
acquire guards a block whose producer is late: the block holds zeros and a
stale data word, and the producer writes the real commands and releases the
label only after the walker has reached the acquire. It checks:

- without the switch, the walker derails on the stale word, as in the field;
- with it, the walker waits across passes and then runs the real block;
- the wait is logged;
- an SPU transfer to the label wakes the walker;
- the bound consumes a stuck acquire and falls back to the old path;
- the held releases of the same pass land before the acquire is read.
