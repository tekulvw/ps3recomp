# Retained Sail graphics configuration

Retain the five big-endian guest callback OPD addresses, opaque pArg, exact
32-byte preferred format and index-zero player association. Validate borrowed
records before guest clearing, including OPDs overlapping that clear. Callback
table bytes are copied; OPD records remain guest-owned and must stay alive until
Finalize. Null callback tables retain registration-only NO_FRAME compatibility.

The internal snapshot API is a decoder boundary, not an ownership pin. A future
frame session must prevent concurrent Finalize/Destroy and revalidate OPDs before
dispatch. No callbacks run under registry locks. PlayerDestroy detaches its binding;
Finalize/Term discard retained configuration. Unsupported adapter indices are explicit.

GetFormat is now registered but reports NOT_AVAILABLE until an actual decoded
session exists. It never returns preferred bytes as actual format. GetFrame stays
NO_FRAME with its complete record cleared; no decoder, timing or playback is added.

Regression uses generated imports and the real PPU bridge: borrowed table/format
reuse, table overlapping guest clear, invalid/overlapping OPDs and bounds, lifecycle
association release, distinct players, canaries and truthful format unavailability.
