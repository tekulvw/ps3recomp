# Selected PPU scalar notes

This default-off extension reuses the bounded selected-call wait-note ring,
clock, trigger, scope, limit and final flush. Enable it with valid audio
chronology and selected-target configuration plus both
`AUDIO_CHRONOLOGY_PPU_WAIT_NOTES=1` and
`AUDIO_CHRONOLOGY_PPU_SCALAR_NOTES=1`. The latter accepts only the literal `1`.
No target address, title-specific layout, branch classification or code bytes
are part of the SDK.

`audio_chronology_scalar_scope()` returns the current admitted selected span
serial, or zero. It performs no guest/context access, clock query or metadata
lock. Initialization can inspect configuration. The serial is diagnostic scope,
not a job, packet or completion identity. This is cached admission: it does not
query time and can remain nonzero until a timed hook or poll notices expiry.
Every scalar append independently checks the actual shared deadline while
holding the metadata lock, so a cached scope cannot admit an expired row.

`audio_chronology_scalar(token, tag, a0, a1, a2, a3)` appends one `kind=3` row
using the existing `[ppu-call-note]` fields. `phase` is the caller-supplied tag,
`rc` is zero, and all four arguments retain raw 64-bit values. Passing token
zero allocates a diagnostic token; subsequent related rows may reuse its
nonzero return within that same selected scope. Zero return means no record.
The function reads no supplied pointers and preserves host error state. The
caller must not add guest loads, API calls or functional changes to obtain data.
Token correlation is a caller contract, not a guest-ownership certificate.

The extended summary explicitly declares
`coverage=existing-sleep-workload-and-scalar-only`. Strict older wait-note
readers reject this opt-in stream; use a reader that validates kind 3 and the
caller's versioned scalar contract. With the option absent, ordinary coverage,
row format, stream ordering, limits and behavior stay unchanged. Audio and PPU
call-span schemas are unchanged.

Scalar rows share the existing note cap and serial counter. They do not allocate
another ring or increase its memory bound. Cap loss and capture expiry can split
a caller's multi-row bundle; missing rows must remain incomplete. Censored or
abnormal selected spans must never imply complete observations. Callers should
bound repeated measurements, identify their source/version externally, and
report their deduplication policy. Values are sequential observations, not an
atomic guest-memory snapshot. Diagnostic overhead still affects wall timing.

Run `python3 tools/test_ppu_scalar_notes.py`, repeat with `--sanitize`, and retain
`tools/test_ppu_wait_notes.py`. The tests use synthetic scopes and inert clocks;
no guest, device or native target is launched. Original-source adapters and their
payloads belong outside this generic SDK and require separate source/parity
proof and review before native integration.
