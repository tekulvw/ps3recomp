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

From the SDK root, compile the actual engine fixture with its neutral backend:

```sh
clang -std=c11 -O2 -DDRAW_OBSERVER_ID_TEST -Iinclude -Ilibs/video \
  libs/video/tests/test_draw_observer_program_identity.c \
  libs/video/rsx_dispatch.c libs/video/rsx_vertex_fetch.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c \
  libs/video/rsx_fp_decompiler.c libs/video/rsx_vp_decompiler.c \
  -lm -o /private/tmp/draw-observer-program-identity-test
python3 libs/video/tests/test_draw_observer_program_identity.py \
  /private/tmp/draw-observer-program-identity-test
```

The fixture includes `test_rsx_draw_engine.c` for the neutral backend and
`rsx_draw_engine.c` for direct access to the inactive poison-pointer check.
For undefined-behavior checking, add `-fsanitize=undefined
-fno-sanitize-recover=all` and choose a separate executable name.
The fixtures use neutral synthetic programs and an inert backend. They exercise
same-address changed FP/VP bytes, unchanged FP structural identity after inline
constant changes, missing programs, early rejection/reset, maximum lengths, and
inactive poison pointers. Device and native game observations remain separate.
