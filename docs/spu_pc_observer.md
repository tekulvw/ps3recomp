# SPU pre-instruction buffer observation

`SPU_PC_OBSERVE=/absolute/path/to/config` enables a diagnostic observer for
instructions executed by the microstep and full interpreter. It is off by
default. Configuration is loaded once, on the first executed instruction;
changing the environment or file afterwards has no effect. Lifted/generated
instruction bodies do not use this observer.

The observer reads local-store bytes and native preferred register scalars
(lane 0), and prints one `[spu-pc-buffer]` line per admitted event. It does not
change guest registers, memory, DMA, scheduling, or context layout. Each line
contains context address, image label, actual PC/opcode, raw r0/r1, configured
identity registers, source/destination pointer and size registers, raw sizes,
divisors, byte spans, and F32BE finite/nonzero/nonfinite counts and peak magnitude.
An image label is not an overlay identity. All global guard words, the selected site group's guards (if configured), and the
site opcode must match live LS before any dynamic buffer is read.

The fixed configuration, event count, and sequence are shared across contexts,
threads, microstep and full interpretation in one linked runtime image. The
merged C-linkage object also supports C++ callers. Independent runtime copies
in separate dynamic libraries are not promised a shared cap. Initialization has
one atomic owner. Each microstep/run_until invocation caches its immutable
enabled flag with one acquire load; disabled instructions perform only a flag
branch. Disabled direct observer calls perform an acquire load and branch. These
checks do not read the context, clock, buffers or allocate memory. An unsupported
microstep opcode is observed by the full interpreter after handoff; recognized
handlers observe immediately before execution. There is no handoff duplicate
and no suppression of legitimate repeated visits to the same PC.

Configuration uses whole lines, decimal register numbers/counts/divisors, and
hexadecimal LS addresses/opcodes (optional `0x` prefixes). Blank lines and lines
beginning with `#` are allowed. No inline comments are accepted. For example,
with synthetic instructions only:

```
limit 8
guard 30010 35002780
identity 3 6
site 30000 34000194 3 4 1 5 6 2
end
```

A `site` line has eight values: PC, expected opcode, source pointer register,
source byte-count register, source divisor, destination pointer register,
destination byte-count register, destination divisor. Raw counts must divide
exactly by the configured divisor. Counts are bytes after division. Each span
must fit entirely in LS and be at most 16 KiB. Empty spans are permitted, and
trailing partial four-byte words are excluded. Both signed zeros count as zero;
finite subnormal values count as nonzero; infinities and NaNs count as nonfinite.
Source and destination may refer to the same buffer.

An optional `trigger /absolute/path/to/arm` line defers event admission until
that caller-owned, non-directory file exists. The path is cached with the rest
of the configuration, is at most 128 bytes, and must be an absolute POSIX,
drive-rooted Windows or UNC path without spaces/escaping. Duplicate, missing,
relative or oversized trigger declarations disable the entire configuration.

Only matching sites with valid opcode, overlay guards and in-bounds spans can
poll the trigger. A shared atomic window permits at most one metadata lookup
per 250 milliseconds across contexts/threads. The lookup reads no file contents
and makes no observer allocations; host error state is preserved. Unarmed calls
scan no buffers and spend no event budget. Once file presence arms the observer,
arming remains latched for that linked runtime: removal or environment/config
changes do not disarm it, restart a capture or reset the cap. Concurrent contexts
may skip an event while another context is completing the first lookup, so a
capture can start partway through an invocation.

The owner should remove any stale trigger before launch, then create it only at
the desired application state. The runtime never creates, deletes or rewrites
that file. Omit `trigger` for the original immediate-admission behavior. When
the observer is disabled, the cached instruction path is unchanged and performs
no trigger work; when configured without a trigger, no file polling or clock
reads are added.

Limits are 4096 file bytes, 160 bytes per line, 8 unique sites, 32 unique guard
words, 8 optional identity registers, divisors 1 through 16, and 256 events.
One `limit`, at least one guard and site, and a final `end` line followed by a
newline are required. Guards/site PCs must be aligned and contain a complete
four-byte word in LS. Invalid, oversized, truncated or unreadable configuration
disables the entire observer. No partial configuration is activated. Once the
shared event limit is exhausted, subsequent matched observer calls skip context/LS
reads, and runtime loops drop their local enabled flag on the next instruction.

Records are observations, not a proof that events from different contexts,
register identities or blocks form a producer/consumer chain. Numeric statistics
on arbitrary LS bytes do not establish that a span contains PCM.

An optional grouped mode can qualify different code overlays in one capture:

```
limit 8
group 1 4
group 2 4
groupguard 1 30010 35002780
groupguard 2 30010 40200000
groupsite 1 30000 34000194 3 4 1 5 6 2
groupsite 2 30000 34000194 3 4 1 5 6 2
end
```

`group <id> <quota>` reserves that many events for the group. IDs are decimal
1 through 8, unique, and must be declared before `groupguard`/`groupsite`
references. `groupguard <id> <address> <word>` adds a guard for that group;
`groupsite <id> <eight site values>` assigns a site to it. Every declared group
must have at least one site and guard. Its quota must be positive, and all
quotas must sum exactly to `limit` (at most 256). This prevents one overlay from
spending another overlay's budget. At most 8 sites total and 32 guards per group
are accepted; the existing 4096-byte file bound still applies. Global `guard`
lines remain optional in grouped mode and, when present, are required in
addition to the selected group's guards. Grouped and legacy `site` lines cannot
be mixed. Existing ungrouped configurations and their output format are
unchanged.

Site PCs may repeat across different groups. Each matching candidate checks
its own guards; a wrong overlay cannot spend that group's quota. Duplicate PCs
within one group are rejected. An exhausted group is skipped before context/LS
reads; an aliased PC belonging to another live group can still require LS reads
to qualify that other candidate. The total cap still disables the cached hook.
Quota admission is atomic across contexts/threads and shares the existing
trigger lifecycle. Rejected guards, invalid spans, or absent triggers spend
neither group nor total budget. No new clock reads, file lookups, allocations,
guest writes, or guest context fields are introduced.

Grouped lines append `group` and `group_seq`. `seq` identifies the process-wide
admission reservation; `group_seq` identifies that group's reservation. Host
threads can print in a different order from either reservation and can be
preempted between admission and reading a buffer. Neither sequence is a frame
number or a producer/consumer identity. Use the actual context, call/stack
registers, configured identity registers, dynamic buffer spans and transfer
addresses to qualify a chain; repeated pointers can refer to reused storage.
These fields do not establish that a source block was copied to a later block.
Disabled cached instruction hooks retain their original flag branch, and the
observer's enable/trigger fast paths are unchanged.
