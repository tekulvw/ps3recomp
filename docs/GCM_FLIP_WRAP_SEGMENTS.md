# Flip wrapping must stay within the current command ring

A context may switch to a small command segment while the FIFO consumer still reads a previous segment. A numerically higher GET does not prove that the new segment's head has been consumed. The flip insertion heuristic previously compared GET only with BEGIN plus its reuse margin. It could rewind CURRENT into that new segment and put the following closing reference behind the consumer.

The heuristic now requires GET to be within the translated extent of the current ring. The margin calculation uses 64 bits, and the last mapped byte establishes the inclusive upper bound without requiring the exclusive END address to be mapped. The existing large-ring wrap remains available when GET has consumed its head within that same extent.

`tools/test_gcm_flip_wrap.sh` compiles the actual GCM implementation with a guest memory fixture. It verifies the small-segment case seen during MAG retail v1.0 startup, an out-of-ring GET for a larger buffer, preservation of adjacent closing reference commands, and the existing valid large-ring wrap. The original implementation failed ten of twenty checks; the fix passes all twenty.

This changes no guest fence values, readiness flags, timers, or movie state. The native application must be rerun to determine whether this explains a particular captured wait; fixture success alone does not establish native startup or rendering success. Noncontiguous or malformed command rings are outside this bounded change.
