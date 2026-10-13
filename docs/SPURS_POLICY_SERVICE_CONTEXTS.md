# Policy-module PPU service contexts

Resident SPURS policy modules can send user events requesting a PPU service to
read their local store and reply through their inbound mailbox. They execute
outside an ordinary LV2 thread group. Previously these events used identity zero;
the LV2 local-store syscall could not resolve it, and the mailbox syscall dropped
the PPU reply while reporting success.

Each resident policy context now has a runtime service identity backed by that
actual context. Identities use a disjoint namespace starting at `0x80000000` and
are never recycled. Event data1 carries this identity. The existing `spu_id`,
physical lane in the guest kernel context, architectural context layout, workload
arguments, and group-thread event paths remain unchanged.

The LV2 local-store and mailbox handlers acquire a reference to the endpoint
before touching its context. Local-store accesses validate size and bounds and
use the real policy local store. Replies append to the actual inbound mailbox;
a full hardware-depth mailbox returns BUSY without dropping or replacing words.
The kernel enqueue acknowledgement and the PPU service reply remain separate
FIFO words. For service events the mailbox lock covers event publication and
enqueue acknowledgement, keeping a concurrently arriving PPU reply behind it.

A successfully published service event arms a reply wait in its endpoint. After
consuming the kernel acknowledgement, an empty `RdInMbox` blocks until the PPU
actually appends its reply. A failed enqueue disarms it; a full reply mailbox
returns BUSY and keeps it armed. This scoped wait preserves legacy empty-read
behavior for contexts with no pending service exchange. It avoids repeating the
latched acknowledgement as a fabricated formatter result. The delayed-formatter
fixture uses a separate PPU host thread and verifies the actual second channel
read cannot finish before that thread queues its real reply.

An unchanged resident identity persists between policy dispatches. A policy/data
reset retires its old identity, blocks new references, and waits for existing PPU
operations before clearing local store. Workload removal checks all lane service
references together and returns BUSY without partial retirement when any are
held. Instance teardown waits for references after its workers have joined.
Callbacks need no admission or policy-slot lock to finish, preventing retirement
from waiting on a lock that a PPU operation must acquire.

`python3 tools/test_spurs_policy_service.py` exercises a manufactured policy
request through the real PM runner, SPU channels, event bridge, LV2 LS reader and
reply mailbox. It checks separate acknowledgements, context-backed bytes,
concurrent instance/lane identities, workload identity isolation, stale IDs,
BUSY removal, referenced reset, unrelated-instance progress, LS bounds and full
mailbox behavior. The event queue transport and unrelated subsystems are test
stubs. The test requires POSIX threads; native Windows compilation is a separate
validation requirement.

This restores a PPU service contract. It does not assert that a queued job
completed, change queue records, or replace guest completion predicates.
