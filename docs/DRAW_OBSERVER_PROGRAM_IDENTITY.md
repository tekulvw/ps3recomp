# Observed program identities in draw-request rows

The existing default-off `RSX_ENGINE_DRAW_OBSERVER` adds seven metadata fields:
`program_identity_observed`, `vp_content_valid`, `vp_content_hash`,
`vp_instructions`, `fp_structure_valid`, `fp_structural_hash`, and `fp_bytes`.
The existing raw `shader_control`, FP address, VP start and frame remain present.
There is no new option, observer, output file or GPU operation.

Each admitted row begins with unavailable identities: validity and lengths are
zero, and hash strings are sixteen zeroes. Only the normal pipeline lookup's
already-executed `eng_guest_programs` result supplies pointers and lengths to the
observer. Rows rejected before that lookup stay unavailable. The observer does
not call the resolver or guest-reader callback. This distinction matters because
the existing resolver first requests a 16-byte FP prefix and scans for its end
before validating the full span; newly calling it for early rows would broaden
that existing memory-access boundary.

When the observer is active, the hook hashes at most 544 VP instructions
(8,704 bytes) and 65,536 FP bytes per row. It uses the existing FNV-1a VP byte
hash and FP structural hash, each with seed 1469598103934665603. The FP structural
hash deliberately excludes inline constant values. Its byte count still includes
those slots. VP constants are outside VP program content. Validity is independent
for VP and FP; a missing pair can select the existing built-in pipeline while an
individual resident program remains observable. A zero FP structural hash is
unavailable, matching the helper's existing failure convention.

These are bounded observations of resident inputs before pipeline translation
and lookup, not a coherent memory transaction, a cryptographic identity, an
immutable program certificate, a complete pipeline key, or proof of the program
bound or executed by the GPU. Concurrent guest writes remain possible. Group by
validity, hash, length and existing control fields; never substitute a recycled
raw address for content. Hash collisions remain possible. Pending/failed rows
and absent output do not establish missing terrain or other object classes.

The ordinary engine path, pipeline keys/cache, guest memory, render state and
callback order are unchanged. Inactive observers hash no bytes and follow no
program pointers. Existing trigger, raw color-A filter, 4,096-row cap and output
error behavior remain unchanged. Existing strict readers of the old JSON schema
will require an explicit schema update before accepting these extended rows.

Build `libs/video/tests/test_draw_observer_program_identity.c` with the same
source list as `test_producer_geometry.c`; define `DRAW_OBSERVER_ID_TEST` for the
inactive poison-pointer check. Pass the executable to
`python3 libs/video/tests/test_draw_observer_program_identity.py`.
The fixtures use neutral synthetic programs and an inert backend. They exercise
same-address changed FP/VP bytes, unchanged FP structural identity after inline
constant changes, missing programs, early rejection/reset, maximum lengths, and
inactive poison pointers. Device and native game observations remain separate.
