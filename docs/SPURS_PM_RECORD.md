# SPURS policy-run recorder

An opt-in diagnostic that records what a SPURS policy module does during each
dispatch, as seen from outside it, and an inventory of counters. It exists to
turn a lifted policy module into a black-box reference: the recordings say which
guest memory a run reads and writes, which lock-line transactions it performs
and with what result, which job code it enters with which context, which
services that job code calls back with which arguments and results, and when it
polls or leaves the kernel. Nothing here describes any module's internals.

Implementation: `runtime/spu/spurs_pm_record.inc` (included by
`spurs_policy.c`), hook table `runtime/spu/spurs_pm_observer.h`, decoder
`tools/spurs_pm_record.py`, fixture `tools/test_spurs_pm_record.py`.

## Switches

All are read once, at the first policy run. With none set, no context ever gets
an observer and the policy runner pays one load of a resolved configuration word
per run; every hook site in the MFC engine and the resolver is one untaken test
of a field of the context it is already working on.

| Variable | Meaning |
|---|---|
| `PS3RECOMP_SPURS_PM_RECORD=<dir>[:<wid list>][:<max runs>]` | Record runs into `<dir>`, one file per policy worker thread (`pmrec-<pid>-t<n>.bin`). `<wid list>`: hex workload ids separated by commas, empty = all. `<max runs>`: recorded runs per (instance, workload), default unlimited. Suffixes are parsed from the right and must be a valid list/number, so `C:\dir` and `C:\dir:1,4:5000` both work. |
| `PS3RECOMP_SPURS_PM_RECORD_BYTES=<n>` | Data bytes kept per transfer element (default 4096; `0` keeps all). Every transfer also carries a 64-bit FNV-1a hash of all its bytes. |
| `PS3RECOMP_SPURS_PM_RECORD_START=<s>` | Only record runs that begin at least `<s>` seconds after the first policy run (skip boot). |
| `PS3RECOMP_SPURS_PM_RECORD_EVERY=<n>` | Record one eligible run in `n` on each worker (default 1: consecutive runs). |
| `PS3RECOMP_SPURS_PM_STATS=1` | Print cumulative counters and histograms to stderr (`[spurs-pm-stats]`) every `PS3RECOMP_SPURS_PM_STATS_SECONDS` (default 5) and at exit. Independent of recording. |

`SPURS_PM_FRESH=1` (existing) makes every dispatch start from a fresh context;
combined with the statistics it shows how much module traffic local-store
persistence saves.

## What a recording holds

Per run: the entry (workload, lane, instance EA, workload data, entry poll status,
image id, module EA/size/fingerprint, whether the context was fresh, the 128-byte
line at the workload data's low word read without side effects) and the exit
(result, polls, exit flag, jobs entered, the same line again). Between them, in
order:

- every MFC command as it reaches the engine (opcode, LSA, EA, size, tag);
- every transfer element (GET, PUT, zeroing) with the hash and the first bytes;
- every lock-line operation: GETLLAR with the line received, PUTLLC/PUTLLUC with
  the line the program tried to store and the atomic status;
- crossings between the policy image and code above it: a job entry (target,
  r0, r1, r3-r6 and the 128 bytes at r3), a call from job code into the policy
  image (target, the offset of the word in the job's context that held the
  target, the streamed-code source EA the caller runs from, r0, r1, r3-r6), its
  return (r3), and the job's return where it is visible (through the resolver,
  or inferred when the call frame that entered the job is left);
- kernel services: select-workload with its answer, exit-to-kernel.

Branches taken inside an SPU interrupt handler (between the interrupt and its
return, while the interrupted registers are saved) are the handler's own and are
not classified as crossings; the statistics count them (`irq_branches`).

Each event carries where the program was (`policy`, `job`, or `service` = inside
a call from a job) and the nesting depth. Jobs' own transfers are recorded too
(region `job`).

Recording dispatches a call from job code into the policy image itself so that it
sees the return: it performs the resolution and drains to the caller's r0 link,
the same dispatch and drain target the lifted call bracket uses. That is the only
structural difference while recording; the fixture requires identical results and
guest memory with the recorder off, on, and statistics only.

## Statistics

Per worker, instance, lane and workload: runs, idle runs (no job entered),
recorded runs, jobs, job returns, service calls, polls, exits, GETLLAR, PUTLLC
success/failure, PUTLLUC, MFC and list commands, GET/PUT counts and bytes,
zeroing, job-module images loaded (by the image header) and their bytes, and
writes into the SPURS instance (first and second 4 KiB). Global histograms: the
entry types of job headers the policy code reads, command opcodes by flag
combination from the job records it reads (header and command layouts as public
interface facts), and service calls by (module source EA, context offset).

## Reading recordings

    python3 tools/spurs_pm_record.py <dir>            # per-run summary
    python3 tools/spurs_pm_record.py <dir> --events   # every event
    python3 tools/spurs_pm_record.py <file> --json    # JSON lines

Recordings of a title are game-derived data: keep them out of every repository.
