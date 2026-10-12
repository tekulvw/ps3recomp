# Bounded silent MAG startup video playback

Set `PS3RECOMP_SAIL_TIMED_THX=1` and `PS3RECOMP_GCM_CHECKED_REF=1` in an
FFmpeg-enabled build with a checked renderer. This is an explicit, narrow
retail MAG startup path. It conflicts with the held-picture diagnostic.

The accepted local descriptor selects MPEG-2 stream 0x1E0 from `thx.pam` or `mag_intro.pam`.
Start requires the 40-byte command whose first word is 1 and remaining words
are zero. Every picture must match the known retail profile: 179 THX or 1806 intro progressive
1280x720 RGBA frames, pitch 5120, time base 1/90000, duration 3003 and rate
30000/1001. All nonterminal pictures retain their real PTS; the final unknown PTS is
published unchanged and its scheduling cadence is independently verified by
real decoder drain EOF. Other media and commands return an explicit error
through the existing descriptor close path.

The original caller pumps GetFrame/GetState. Guest callbacks run outside the
graphics lock, under the descriptor mutation guard. The original graphics
allocator supplies four buffers (a validated partial count of at least three
is accepted). Only new allocations are written. Replaced buffers retain two
frame-bearing polls before their original freeFrame callback; replenishment
uses that same allocator. A stalled consumer never causes a live overwrite.
The final selected picture remains available for at least its full duration,
even after a late publication. Natural completion emits state transitions,
without inventing a Stop-call completion or attributing harness termination
to EOF. Interrupted ownership transfers remain quarantined.

Guest-buffer release alone cannot exclude a future texture bind. Integrators
must bracket the title's original frame selection with
`sail_graphics_picture_capture_begin/end`; end transfers one owned token.
Bind it to the actual immutable descriptor using `rsx_picture_bind_descriptor`
and release the transferred reference. Capture that descriptor's identity at
the actual FIFO offset payload word with `rsx_picture_capture_command`, before
exposing the command through PUT. The serialized engine takes that identity
and keys texture uploads by it. An uncaptured reference overlapping a tracked
picture range fails closed. No latest-picture-by-address lookup is valid.
MAG retains exact-image hooks for original E14D48 descriptor construction and
9841FC FIFO copying outside this SDK. Other games need their own proved capture
boundaries; this producer is not generic Sail playback.

Source regression: `python tools/test_sail_timed_video.py`. It drives real
NID/HLE registration with a bounded decoder fake and the actual identity
registry, checks both profiles, sequential close/destroy/reopen, intro interruption, deadlines and final drain, late final selection, sparse
poll backpressure, original callbacks, cancellation and failure quarantine.
Actual FFmpeg hashes, native cadence, visible output and original deferred
allocator reuse must also be verified by the game integration.

The compatibility switch retains its THX name. The intro adds a per-session profile, without widening codec/geometry limits. The v1.0 intro asset SHA256 is `7b2a9f771380ecb04c4c892bbf3762b318e002fdd79de866ddc5990e9f623964`; independent FFmpeg9 probing confirms 1806 pictures over 60.2602 seconds, PTS90000+(ordinal-1)*3003 until the unknown terminal timestamp, followed by true decoder EOF. Audio is not delivered.
