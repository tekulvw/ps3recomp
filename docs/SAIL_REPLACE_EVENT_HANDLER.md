# Sail event-handler replacement

`cellSailPlayerReplaceEventHandler` (NID `47632810`) updates the callback OPD
and opaque callback argument in the existing initialized-player registry.
It emits no event and performs no guest-memory writes. The current callback
finishes normally; the next notification reads the replacement callback and
argument. In particular, replacement during Boot completion redirects the
subsequent settled-state notification without changing the event sequence.

## Evidence and bounded contract

Original retail MAG v1.0 (`mage_g.elf` SHA256
`0089f0d6d17e48533ce7ff8d943dddd03487b925e7b04a1a055bfb907568694c`)
calls import stub `0152A234` at `003CDC98`, LR `003CDC9C`. The callback's
major-2/minor-1 completion path first calls FutureSet and then replaces the
handler. Registers hold player `this+0x80`, replacement callback loaded from
TOC+`0x3A90`, opaque argument `this`, and two explicitly zero trailing values.
The captured native call supplies player `01FB97E0`, callback OPD `01AE9118`,
and argument `01FB9760`.

[RPCS3's implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp)
is an untyped unimplemented placeholder, so it does not establish replacement
semantics or output parameters. The prototype contains the named import symbol
but a bounded named-DWARF search found no function declaration. Consequently:

- The implementation consumes the observed five-register form. The last two
  arguments have **unknown semantics**, not assumed old-handler outputs.
  If either is nonzero, it returns `CELL_SAIL_ERROR_NOT_AVAILABLE` (`80610708`)
  without changing the registry or writing to guest memory.
- Conservative local validation requires an existing player, a nonnull VM,
  and a nonnull four-byte-aligned callback OPD whose two dispatch words fit
  the guest address range. Invalid inputs return `80610701`. This is runtime
  policy, not a claim of complete firmware validation behavior.
- The opaque callback argument is retained without dereferencing it. An
  eight-byte OPD span is checked; executable mappings and TOC contents are not
  validated by this API. A zero VM-size field denotes the full 32-bit range.
- Existing player lifecycle and synchronous notification behavior are retained.
  This change adds no thread-safety guarantee for concurrent registry access,
  future completion, playback, decoding, callback scheduling, or events.

Existing error aliases elsewhere in the Sail stub have historical numeric
mismatches; the new path uses local exact values without changing older APIs.

## Regression

`python3 tools/test_sail_replace_handler.py` builds the real Sail source,
generated NID registration, and PPU HLE dispatcher. The regression first fails
because the API is absent. Thirty-nine checks cover replacing inside Boot's
completion callback, unchanged event order/count, subsequent routing, two
players, missing/destroyed players, slot reuse, opaque argument preservation,
OPD boundary/alignment/null cases, and unsupported trailing arguments leaving
the old handler intact. The no-VM guard alone is called directly because the
HLE bridge necessarily writes the guest stack before invoking any C handler.
All normal cases use real HLE dispatch.
