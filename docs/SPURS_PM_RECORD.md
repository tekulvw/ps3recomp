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
per run. Every hook site in the MFC engine and the resolver first tests
`g_spu_dispatch_gates.pm_extras` (a word on the gate cache line those paths
already load, predicted not taken), which the recorder opens only when one of
these variables is set; only then does a site read the context's observer
pointer.

| Variable | Meaning |
|---|---|
| `PS3RECOMP_SPURS_PM_RECORD=<dir>[:<wid list>][:<max runs>]` | Record runs into `<dir>`, one file per policy worker thread (`pmrec-<pid>-t<n>.bin`). `<wid list>`: hex workload ids separated by commas, empty = all. `<max runs>`: recorded runs per (instance, workload), default unlimited. Suffixes are parsed from the right and must be a valid list/number, so `C:\dir` and `C:\dir:1,4:5000` both work. |
| `PS3RECOMP_SPURS_PM_RECORD_BYTES=<n>` | Data bytes kept per transfer element (default 4096; `0` keeps all). Every transfer also carries a 64-bit FNV-1a hash of all its bytes. |
| `PS3RECOMP_SPURS_PM_RECORD_START=<s>` | Only record runs that begin at least `<s>` seconds after the first policy run (skip boot). |
| `PS3RECOMP_SPURS_PM_RECORD_EVERY=<n>` | Record one eligible run in `n` on each worker (default 1: consecutive runs). |
| `PS3RECOMP_SPURS_PM_RECORD_LSREAD=1\|<hex limit>` | With the recorder or the statistics on: report job code's local-store quadword reads below `0x4000` (`1`) or below the given hex limit. See "Local-store reads" below. Ignored, with a notice, when neither is set. |
| `PS3RECOMP_SPURS_PM_RECORD_REGS=1` | With the recorder on: a `regs` event (kind 9) with the whole register file (128 × 4 words), `int_enable`, `event_mask` and status at every job entry, and at a halt or stop of the observed context (not the run's normal exit to the kernel) the same plus the 512 bytes of local store from the halted function's entry. See "Register files" below. |
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

## Local-store reads

`PS3RECOMP_SPURS_PM_RECORD_LSREAD` shows which local-store addresses job code
reads that lie outside what it was handed: it reports each quadword load (`lqd`,
`lqx`, `lqa` of non-register-promoted lifted code, `spu_ls_read128`) that job
code at depth 0 performs below the limit during an observed run. Loads in the
policy image, in a service a job called, by the runtime's own helpers, or at or
above the limit are not reported. Register-promoted images compile the read
gate out unless built with `-DSPU_LS_DIAG=1`.

It costs nothing when unset: the hook is the runtime's existing read-diagnostic
gate (`g_spu_ls_rdiag`, one load and an untaken branch per quadword load, see
`spu_context.h`), which this switch arms only when it and the recorder are both
set. While armed, every quadword load in every SPU thread takes the cold
diagnostic call, so the switch is for short diagnostic runs.

In a recorded run, each read is an `lsread` event: the address as the code formed
it (the low four bits kept; SPU loads always fetch the whole quadword, which is
`size`), the sixteen bytes the load returned, `in_ctx` (inside the 128-byte block
whose address the job was handed in r3), `at_ctx` (the quadword the current r3
names), the job's entry and the streamed-code source EA it runs from, the
context address it was handed, the resolver's last pc and its code source, and
r0, r1 and r3. Lifted code updates the pc only at branches, so `pc` is the last
one, not the load's own. These events go to the recording file only, never to a native policy
module's shadow comparison, and a shadow comparison alone does not arm the switch.

Independently of recording, every worker keeps an aggregate per (job module,
entry, quadword): reads, how many were `in_ctx` and `at_ctx`, how many returned
a value different from the first, and the first and last values. It is printed
every `PS3RECOMP_SPURS_PM_STATS_SECONDS` and at exit: to
`<dir>/pmrec-lsread-<pid>.txt` (rewritten each time, so a killed process keeps
the last one) when recording, otherwise to stderr (`[spurs-pm-lsread]`).

## Register files

`PS3RECOMP_SPURS_PM_RECORD_REGS` answers "what state did job code start in, and what did it hold
when it stopped". It makes two kinds of event:

- **At a job entry:** the whole register file, with the interrupt enable, the event mask and the
  status.
- **At a halt or stop of the observed context:** the same, plus the 512 bytes of local store from
  the halted function's entry, so the halting instruction and its operand register can be read.

The halt event comes through a hook pointer in `spu_halt` that is NULL unless the switch is set.
Each event is about 2 KiB, so use it with a run limit. Like the local-store read events, these go
to the recording file only, never to a native policy module's shadow comparison.

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
