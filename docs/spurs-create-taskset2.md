# SPURS Taskset2 creation

MAG retail v1 reached account login/MLS, then stopped at imported
`cellSpursCreateTaskset2`, firmware NID `0x4A6465E3`. The import stub was
`0x015285D4`, LR `0x00125F74`. The caller allocated `0x2900` bytes and passed
SPURS EA `0x44E00500`, taskset EA `0x47010400`, and a NULL attribute. The value
in r6 (`0xE407`) is outside this three-argument API.

## Contract and evidence

The primary implementation references are RPCS3's
[cellSpurs.cpp](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/Cell/Modules/cellSpurs.cpp)
and
[cellSpurs.h](https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Emu/Cell/Modules/cellSpurs.h).
The cached source artifacts consulted on 2026-10-12 have SHA-256 values:

- `cellSpurs.cpp`: `fe9fc920ad97d993b8223a3695fc91512928b58310aa39659d7151f53944347b`
- `cellSpurs.h`: `8ba7a3c5b9377819906f13228354efbd71210f9ecf1a4253cc4fb80894abb1e9`

In those artifacts, `cellSpursCreateTaskset2` is at cpp lines 4483–4507,
`_cellSpursTasksetAttribute2Initialize` at 4390–4408, and the common
`_spurs::create_taskset` at 3943–3983. Taskset2 shares the first-generation
header/TaskInfo layout, extends to `0x2900`, and has 128 16-byte exit-code slots
beginning at `0x1980`. Its guest alignment is 128 bytes.

Attribute2 occupies 512 bytes with 8-byte guest alignment. Its BE fields are
revision at `0x00`, name EA at `0x04`, u64 argument at `0x08`, eight priority
bytes at `0x10`, max contention at `0x18`, signed clear-LS value at `0x1C`, and
task-name-buffer EA at `0x20`. NULL attributes select revision 0, argument 0,
eight priorities of 1, max contention 8, clear-LS disabled, and NULL names.
Workload registration clamps maximum contention to 8.

The reference identifies layout and defaults, but its Taskset2 implementation
also contains a TODO after registering the workload and checking task-name
buffer alignment. It does not establish a complete implementation of all
generation-2 task APIs. The SDK change uses these ABI facts with its existing
BE layout builder and worker registry; no RPCS3 implementation code is copied.

## Implemented behavior

`cellSpursCreateTaskset2` validates guest alignment/ranges, requires a registered
SPURS instance, rejects a duplicate live taskset, and allocates a real workload
id in the same namespace as `cellSpursAddWorkload`. It clears exactly `0x2900`
guest bytes, writes the shared BE header, initializes the workload-flag wait
sentinel to `0x80`, and publishes the taskset EA, priorities, policy image/size,
and contention in the owning SPURS workload record. `cellSpursGetWorkloadInfo`
returns consistent policy/name metadata from its host mirror. The existing task
creation and asynchronous worker path can consume that shared prefix. After a worker
finishes, existing task exit bookkeeping releases its task slot; the existing
taskset destruction path releases its workload registration.

The companion `_cellSpursTasksetAttribute2Initialize` exports firmware NID
`0xC2ACDF43` and writes the real 512-byte BE attribute defaults. Both functions
have ordinary external C definitions discovered by `gen_hle_nids.py`.

The shared creation helper also corrects first-generation initialization to
clear and report its actual `0x1900` guest size. Previously it used the size of
the small native `CellSpursTaskset` shim, leaving stale guest TaskInfo slots and
writing an incorrect size at `+0x1890`. This correction is necessary for the
shared guest-layout path; it never clears the Taskset2 extension for a legacy
allocation.

## Explicit limitations

- A nonzero task-name buffer returns task NOSYS (`0x80410903`), with a diagnostic;
  misalignment returns task ALIGN (`0x80410910`) before mutation.
- A SPURS2 instance with a 32-workload scheduler is unsupported and returns task
  NOSYS. Taskset2 is supported with ordinary SPURS instances.
- `cellSpursCreateTask2`, Task2 join APIs, and `cellSpursDestroyTaskset2` remain
  unimplemented. This change does not claim generation-2 task exit-code or
  task-name handling. Those imports must receive separate implementations if
  MAG calls them next.
- The existing runtime retains a global 16-workload table and existing task
  dispatch/lifetime behavior. The fixture proves registration reuse after a
  completed worker, not concurrent taskset destruction or full firmware
  scheduling equivalence.

## Validation

Use Python 3.10 or newer; macOS system Python 3.9 cannot import the existing
`nid_database.py` union type annotations.

The asset-free `tools/test_spurs_taskset2.py` fixture verifies the missing NID
fails before implementation, default/explicit BE bytes and allocation guards,
NULL/alignment/range/unregistered/duplicate/capacity failures without mutation,
task-name-buffer rejection, SPURS2 rejection, independent workload ownership,
and a real fingerprint-registered async SPU worker. The worker verifies its
loaded ELF and taskset/SPURS/argument context, completes, releases its slot, and
permits workload teardown/recreation. It does not replace the runtime registry,
ELF loader, async dispatcher, or PM context builder with mocks.

The legacy workload-registration fixture checks `0x1900` clearing, the final
TaskInfo slot, exception fields, size field, and adjacent guards. These added
assertions fail on baseline `bc3af2b` at the incorrect size field.

Relevant regression commands:

```text
python3 tools/test_spurs_taskset2.py
python3 tools/test_spurs_taskset_registration.py
python3 tools/test_spurs_workload_flag.py
python3 tools/test_spurs_flag_receiver.py
python3 tools/test_spurs_task_signal.py
python3 tools/test_spurs_policy_argument.py
python3 tools/test_hle_registration_dependencies.py
python3 tools/check_ppu_scaffold.py
```

Runtime MAG validation is performed separately by the integrating task.
