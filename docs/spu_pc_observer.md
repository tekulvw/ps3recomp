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
An image label is not an overlay identity. All configured guard words and the
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
