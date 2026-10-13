# Bounded FIFO observations

The existing FIFO drain supports an opt-in diagnostic interval through
`GCM_FIFO_OBSERVE=0x1000-0x2000` (IO offsets, lower bound inclusive, upper
bound exclusive). No interval is enabled by default. Invalid or reversed
intervals disable observation. Decimal numbers or numbers prefixed with
`0x` are accepted.

`GCM_FIFO_OBSERVE_LIMIT` caps all emitted records (default 128, maximum
4096). `GCM_FIFO_OBSERVE_WORDS` caps the payload sample per packet (default
16 words, maximum 32). Both must be positive integers within their bounds;
invalid values disable observation. Configuration is read on first drain.

Records begin with `[fifo-observe]`. Packet records show the original loaded
header used by the decoder, IO position, PUT, decoded type/count/method,
exclusive computed packet end, and a bounded sample of mapped payload words.
Selection uses packet-span overlap, so a header before the interval can be
recorded when its payload consumes the selected interval. Sampling starts at
the first complete payload word inside that intersection. `truncated=1`
means the in-range payload exceeded the sample cap; payload outside the
interval is never sampled. Unmapped words are printed as `unmapped`.

Payload samples are additional read-only diagnostic reads, not an atomic
snapshot or proof of the values later consumed by individual methods. The
header is the actual loaded decoder word. Separate branch records show the
actual decision/target. Report records show the argument actually passed to
the report handler. Repeated identical self-parks with the same PUT are
suppressed until another command is seen or the word/PUT changes. Logging
stops at the overall cap. The observer does not write guest memory, move GET,
change publication ordering, or call guest code. Enabled logging can affect
host timing; compare runs with and without observation when investigating a
race.

Run `python3 tools/test_gcm_fifo_observer.py` for the real FIFO-drain fixture.
It checks disabled/malformed configuration, packet-span overlap, payload
clipping and truncation, branch/report records, unchanged completion, repeated
park suppression, and the record cap.
