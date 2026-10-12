# Coherent PPU reservation tokens

New PPU lifts use runtime load-reserve and store-conditional entry points. Loads
capture the reserved word and its 128-byte line epoch under the SPU lock-line
lock. SPU PUTLLC commits and notifications use the same lock. A PUTLLC elsewhere
in the line therefore invalidates the PPU token even if its word is unchanged,
including writes that restore the original bytes. Every new SC consumes its token.

Regenerate PPU title and PRX lifts to enable this behavior. Old generated SC
entry points preserve their earlier value-CAS ABI; they do not acquire line
tokens. No context layout change is required. PPU_RESV_OFF remains a diagnostic
override, not an architectural mode.

This change closes the unconditional SPU PUTLLC publication race. It does not
claim complete architectural coverage for every writer: the pre-existing bitmap
fast path has a first-reservation publication window, raw generated vector and
byte-reverse stores bypass notifications, and inline ppu_memory.h LL/SC helpers
retain their old behavior. Those require separate changes and regressions.

Run tools/test_ppu_spu_line_reservation.py for real lifted instructions linked to
actual loader/coherence sources. It covers separate-worker publication, same-line
ABA, retry, unrelated lines, both widths, token consumption, context/address
mismatch, missing tokens, guest address zero, and mixed old/new entry points.
