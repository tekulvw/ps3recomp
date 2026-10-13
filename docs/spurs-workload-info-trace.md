# Observing SPURS workload readiness

`PS3_SPURS_INFO_TRACE=1` enables a bounded diagnostic at the HLE dispatch boundary
for `cellSpursGetWorkloadInfo`. Its default is disabled. It does not change SPURS
workload eligibility, counters, or scheduling.

Each completed call emits one `[spurs-info-trace]` record containing the caller's
actual LR, thread ID, SPURS instance, workload ID, output address, result, and the
returned ready count, current contention, minimum/maximum contention, idle request,
and signal. Returned fields are marked valid only on success and when the entire
output structure is inside the VM mapping.

Before/after request snapshots additionally report the first workload bank's ready,
contention, idle, signal, flag receiver and flag value. They are separately marked
valid; they are observations around dispatch, not an atomic scheduler snapshot.
The native scheduler active count and cold-admission count are unavailable in this
observer. A zero request snapshot alone does not prove why a workload was admitted.

Optional controls:

| Environment variable | Meaning |
| --- | --- |
| `PS3_SPURS_INFO_TRACE_LIMIT` | Decimal record limit; default 65536, maximum 1048576. |
| `PS3_SPURS_INFO_TRACE_SPURS` | Filter by hexadecimal instance address. |
| `PS3_SPURS_INFO_TRACE_WID` | Filter by hexadecimal workload ID. |
| `PS3_SPURS_INFO_TRACE_LR_MIN` | Inclusive hexadecimal caller LR lower bound. |
| `PS3_SPURS_INFO_TRACE_LR_MAX` | Inclusive hexadecimal caller LR upper bound. |
| `PS3_SPURS_INFO_TRACE_ARM_FILE` | Begin admitting records when this file can be opened for reading. The file is checked on each matching import, so it can be created during the run. |

Filtering and arming occur before admission against the limit. The disabled path
and unrelated NIDs do not dereference the context or guest memory and do not perform
file I/O. The observer preserves `errno` and changes no guest registers or memory.
Records use one bounded print operation so concurrent calls remain attributable.

Run `python3 tests/test_spurs_workload_info_trace.py` for the standalone regression.
It exercises the original GetWorkloadInfo body with and without the observer and
compares the complete PPU context, guest VM and workload mirror. Additional modes
check poison memory when disabled, filtering, runtime arming, invalid observation
ranges, return errors, cap saturation and concurrent record attribution. It uses
UBSan. The `--red` switch substitutes an empty observer to show that the output
assertion fails when the diagnostic is absent.
