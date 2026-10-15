# Opt-in verification of fixed-placement lifted overlays

The runtime can reuse a lifted image in a policy context after validating its
actual local-store executable bytes. This API is disabled until the embedding
application explicitly registers expected segments at startup. It is separate
from source-EA and 16-byte signature overlay registration, which retain their
existing behavior. Do not also register a verified image through those legacy
ownership APIs.

1. Generate a selected image with `tools/spu_lifter.py --verified-code-image ID`.
   The positive, distinct image ID must match its exact function registration.
   Default generation and explicit ID zero preserve ordinary generated output.
2. Register its generated functions under that image ID, using the existing
   `spu_begin_image` / generated registration mechanism.
3. Before starting any SPU thread, call `spu_verified_overlay_register` from
   `runtime/spu/spu_verified_overlay.h`. Supply the policy image ID and aligned,
   nonempty, nonoverlapping executable segments at the exact intended LS
   addresses. Expected byte arrays must be valid host arrays during the call;
   successful registration copies them into immutable process-lifetime storage.
   Check the returned status and refuse to enable the image if registration fails.

The registry holds at most 16 images, eight segments per image, and 4 MiB of
copied executable bytes. Per-image bytes and all addresses must fit the 256 KiB
local store. The owning region is the bounding range of its executable segments;
its gaps are authoritative unlifted holes. There is no relocation, loader,
source-EA inference, or scheduler change. Only an initialized private LS context
in the exact policy image and policy mode can activate the compiled image.
Raw/mapped LS cannot activate this path.

At a queried entry the runtime first requires an exact-image function-table hit;
a compatibility wildcard cannot qualify. It then compares every registered
executable segment. An incomplete or changed image, an unregistered entry, or
multiple matching candidates stays interpreted. The comparison happens again on
each eligible transfer. There is no cached trust requiring an incomplete write
invalidation scheme, and no whole-image comparison at every interpreter PC.

Every opted-in translated function body starts with the same verification gate,
so a direct generated call or queued trampoline cannot bypass verification.
External dispatch stubs contain no translated guest instructions and retain
their existing behavior of queuing the verified indirect resolver. A failed
gate publishes its own entry PC, queues the existing indirect dispatcher, and
returns. The existing caller drain installs its architectural return PC before
interpreting the live LS instructions. It preserves the guest link register,
host call depth, and scheduler state. Guarded image IDs are suppressed from
all inappropriate legacy primary/wildcard/resident lookup return paths, even
outside their verified segment bounds, to avoid reject/retry loops. An ordinary
authoritative resident owner miss still returns NULL without falling through.

This is **transfer-entry verification**, not complete support for self-modifying
code. Writes between entries are observed at the next verification. Changes to
instructions while an already-running generated function continues within its
body, and concurrent external LS writes during that body, are outside the
contract. A native trial must separately qualify its selected executable text as
readonly by intended behavior. Executable section flags are identity metadata,
not proof of hardware write protection. Full byte identity and successful
compilation do not establish complete translated-ISA correctness or a speedup.
Frequent generated entries can make full comparison expensive; measure it.

Run `python3 tools/test_spu_verified_overlay.py`, optionally with `UBSAN=1` and
`MUTANT=1`. The fixture lifts neutral caller/callee instructions and executes
actual production dispatch and interpretation. The caller changes the callee
through the production SPU store helper before its direct generated call. The
callee guard must interpret those changed bytes and return to the original
caller. Removing only the generated gates is a failing negative control. Actual
GET and GET LIST partial loads, foreign GET replacement, immutable descriptor
copies, bounds, holes, wrong binding, wildcard rejection, ambiguity, and registry
capacity are also checked. The existing thread-start/rejoin suite is retained.
