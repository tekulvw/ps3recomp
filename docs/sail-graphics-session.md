# Explicit Sail graphics frame session

An internal producer pins the retained callback configuration with
`sail_graphics_begin_session`, obtaining the same session number passed to
formatChanged and returned in GetFrame. It owns all guest pixel buffers.
Publish supports one immutable packed RGBA picture per session; a second
publication is rejected even after unpublish, avoiding stale cached formats.
Unpublish atomically hides pixels and invalidates cached timing while retaining
the callback pin. Withdraw ends that pin only after ownership is resolved.
Finalize, player destruction, preferred-format mutation and Term reject active
sessions. No guest callbacks execute under the graphics lock.

GetFrame writes exactly 24 big-endian bytes: buffer, session, tag, decoder
success status zero, and 64-bit source PTS. It returns supported new/repeat
selection classes 0x200/0x100. Firmware timing/drop/count reason flags are not
implemented. No producer means NO_FRAME and the entire record is zeroed.
GetFormat returns the concrete 32-byte published format; preferred bytes are
never readiness. This component neither allocates pixels nor simulates EOS.

PtsToTimePosition is registered at firmware NID 1872331B. Its origin is the
PAMF presentation start supplied by the producer and cached when GetFrame
polls the picture. Firmware uses wrapped `(pts-origin)*1000`, interpreted as
signed 64-bit, divided by 90 with truncation toward zero, producing relative
microseconds. Unknown origin UINT64_MAX yields INVALID_STATE without touching
output. Input PTS UINT64_MAX is not treated as an extra sentinel. Bounds checks
are a safe HLE policy beyond the original firmware's unchecked output pointer.
UpdateAvSync does not establish a GPU fence or allow buffer recycling.

The real generated NID/PPU fixture proves publication, endianness, actual
format, negative and wrapped timestamps, unknown origin, output bounds,
reentrant teardown guards, unpublish pin lifetime and changed-format refusal.
Native media delivery is separate and is not asserted by this fixture.
