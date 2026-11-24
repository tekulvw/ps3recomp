# NP social provider (friends, presence, friend requests, lookups)

PSN's friends list, presence and friend requests have no server in this runtime. An embedding
application can supply one through a **social provider**, declared in
[`include/ps3emu/np_social.h`](../include/ps3emu/np_social.h). The provider is a set of host
callbacks plus a queue it feeds. It is generic: it carries NP IDs (online ID handles) and opaque
bytes, and knows nothing about any title or service.

**Inert by default.** Until `ps3_np_social_set_provider` is called, every function below behaves
exactly as it did without this file: `sceNpBasicGetEvent` answers `SCE_NP_BASIC_ERROR_NO_EVENT`
without touching its arguments, `sceNpBasicSetPresenceDetails` answers `NOT_SUPPORTED`, the
context-sensitive handler is accepted and never called, and `sceNpBasicAddFriend`,
`sceNpBasicSendMessageGui`, `sceNpLookupNpIdAsync` and `sceNpLookupNpId` stay unregistered imports
(the host's unresolved-import policy applies to them, unchanged). Nothing is allocated, no thread
starts and the sysutil poll does no extra work.

This page is the behaviour specification, written from the public SDK semantics of these calls.
The implementation is `libs/network/np_social.c`; the synthetic tests are
`tests/test_np_social.c`, run by `tools/test_np_social.py`.

## Registering

`ps3_np_social_set_provider(&provider)` installs the provider. Call it once, from the host, before
the guest starts; a second call, a NULL provider or a structure whose `abi` is not
`PS3_NP_SOCIAL_ABI` is refused (non-zero result) and changes nothing. Every callback may be NULL;
a NULL callback makes its import answer as described under that import. Registering adds the four
imports above to the HLE table (as context handlers), so a title that never calls them sees no
difference.

Callbacks run on a guest thread, inside the import that triggered them. They must return
promptly: copy what they need and do any network work elsewhere. Pointers passed to a callback
are valid only during the call.

## Events (provider to title)

`ps3_np_social_push_event(event, ret_code, online_id, data, size)` queues one NP Basic event. It may
be called from any thread. `event` is an SDK `SCE_NP_BASIC_EVENT_*` value; the ones a friends list
needs are:

| Event | Value | Meaning | Data |
|---|---|---|---|
| `OFFLINE` | 0 | the friend signed out | none |
| `PRESENCE` | 1 | the friend is online in this title; the data is the presence they published | their presence bytes, 0 to 128 |
| `OUT_OF_CONTEXT` | 9 | the friend is online, but not in this title | none |
| `FRIEND_REMOVED` | 10 | the friend is no longer on the list | none |

Other event values (for example `ADD_FRIEND_RESULT`, 3) pass through the same way, with
`ret_code` as the result. `online_id` is the friend's online ID, 1 to 16 bytes without a NUL
inside; `data` is at most 128 bytes (`SCE_NP_BASIC_MAX_PRESENCE_SIZE`). The runtime never reads
meaning into the bytes. A call with no provider, a bad ID, oversized data, or a full queue
(256 events) is refused with a non-zero result and a one-line message on stderr for each kind of
refusal; a refused event is never delivered later.

Delivery follows the SDK model of a registered event handler plus a fetch:

1. The title registers a handler with `sceNpBasicRegisterContextSensitiveHandler(context,
   handler, arg)`. The registration is recorded (handler and argument; the context is not
   checked). `sceNpBasicUnregisterHandler`, `sceNpBasicTerm` and `sceNpTerm` (noticed at the next poll) drop it.
2. At each `cellSysutilCheckCallback`, while a handler is registered, NP is initialised and a guest
   caller is installed, every queued event that has not yet been announced is announced: the
   handler is called once per event, in queue order, with `(event, ret_code, 0, arg)`. Nothing is
   announced without a handler; queued events wait for one (still bounded by the queue size).
   When the handler is dropped, announced events the title has not fetched become unannounced
   again and go to the next handler. If NP is no longer initialised at a poll, the handler is
   dropped.
3. `sceNpBasicGetEvent(&event, &from, data, &size)` hands over the oldest *announced* event and
   removes it: the event code (s32), `from` as an `SceNpUserInfo` (216 bytes: the NP ID with the
   online ID as its handle, the online name equal to the online ID, an empty avatar URL), and the
   data. `size` is in/out: on entry the room in `data`, on return the number of bytes written,
   which is the event's data size, cut to the room given. With no announced event it answers
   `NO_EVENT` (0x8002A66A) and writes nothing. A NULL `event`, `from` or `size` answers
   `INVALID_ARGUMENT` (0x8002A665) and hands nothing over. A NULL `data` receives nothing
   (the returned size is 0) and still takes the event.

A title may call `sceNpBasicGetEvent` inside its handler or later; both see the same order. An
event is handed out once. The first announcement and the first fetch of a process each print one
`[np-social]` line on stderr, so a run's log shows that events reach the title.

The provider is told when the title gains or loses its handler (`handler_changed`), so it can
send the current state again (for example presence for every online friend) to a title that
registers late or re-registers.

## Own presence (title to provider)

`sceNpBasicSetPresenceDetails(details, options)` publishes this player's presence. `details` is an
`SceNpBasicPresenceDetails` in guest memory (392 bytes: title 128, status 64, comment 64,
data 128, size u32, state s32, big-endian). `options` is a mask of `SET_DATA` (1) and
`SET_STATUS` (2).

- Argument checks are unchanged: a NULL `details`, or `options` outside 1 to 3, answers
  `INVALID_ARGUMENT`.
- Without a provider, or with a provider whose `set_presence` is NULL, the answer stays
  `NOT_SUPPORTED` (0x8002A663): nothing pretends to publish.
- With one: a `size` above 128 with `SET_DATA` answers `EXCEEDS_MAX` (0x8002A66B) and nothing is
  sent. Otherwise `set_presence` receives the data bytes (`size` of them) when `SET_DATA` is set,
  and the status text (up to 63 bytes, cut at its NUL or at 64) when `SET_STATUS` is set, each
  with a flag saying whether it was set this time. The call answers `CELL_OK`.

## Friend requests, messages and lookups (title to provider)

These imports exist only while a provider is registered.

**`sceNpBasicAddFriend(contact, body, container)`** asks to add a friend. `contact` is an
`SceNpId`; its handle is the online ID. `body` is an optional message (NUL-terminated, at most 512
bytes counted); `container` is ignored. A NULL `contact`, or an empty or unterminated handle,
answers `INVALID_ARGUMENT`; a longer body answers `EXCEEDS_MAX`. Otherwise `add_friend` receives
the online ID and message and its result is the answer (0 for "request sent"); with no
`add_friend` callback the answer is `NOT_SUPPORTED`. The real system shows a dialog first; here
the request goes out directly. A provider that wants to report an outcome later pushes
`ADD_FRIEND_RESULT`.

**`sceNpBasicSendMessageGui(message, container)`** sends a message through the system message
dialog. `message` is an `SceNpBasicMessageDetails` (40 bytes: message ID u64, main type u16,
sub type u16, features u32, recipient NP ID array pointer, recipient count u32, subject pointer,
body pointer, data pointer, data size u32). A NULL message, no recipients, more than 12, or a
recipient without a valid handle answers `INVALID_ARGUMENT`; a subject over 18 bytes, a body over
512 or data over 1 MiB answers `EXCEEDS_MAX`. Otherwise `send_message` receives the fields with the
strings and data copied out of guest memory, and its result is the answer; with no callback the
answer is `NOT_SUPPORTED`.

**`sceNpLookupNpIdAsync(trans, onlineId, npId, prio, option)`** resolves an online ID to an NP
ID within a lookup transaction (`sceNpLookupCreateTransactionCtx`). An unknown transaction answers
the existing invalid-ID error; a NULL `onlineId` or `npId`, or an empty or unterminated online ID,
answers `INVALID_ARGUMENT`; a transaction that already has a lookup running answers
`BUSY_BY_ANOTHER_TRANSACTION`. Otherwise the lookup starts: `lookup_online_id` receives the online
ID and a request number, and the call answers `CELL_OK`. With no callback the lookup completes at
once as not found.

The provider finishes the lookup, from any thread, with
`ps3_np_social_complete_lookup(request, result, online_id)`: `result` 0 and the canonical online
ID when found, or a negative SDK error (`SCE_NP_COMMUNITY_SERVER_ERROR_NO_SUCH_USER_NPID`,
0x8002A41B, for an unknown name). The `npId` out-parameter is written when the title next polls
after completion: the handle is the canonical online ID, everything else zero. A completion for an
unknown or finished request is ignored.

While the lookup runs, `sceNpLookupPollAsync` answers 1 ("still running") and leaves its result
untouched; afterwards it answers 0 and writes the lookup's result. `sceNpLookupWaitAsync` waits
for completion, polling every 10 ms, for at most 30 seconds, and then answers
`SCE_NP_COMMUNITY_ERROR_TIMEOUT` (0x8002A10F) and abandons the lookup. `sceNpLookupAbortTransaction`
abandons a running lookup: its result becomes `SCE_NP_COMMUNITY_ERROR_ABORTED` (0x8002A109) and a
later completion is ignored. Destroying the transaction abandons it the same way.
`sceNpLookupNpId` (the blocking form) is the asynchronous call followed by the wait.

Transactions without a lookup behave as before (everything else in the Lookup library completes
at the call).

## Limits and what is not covered

- One provider per process, for the life of the process.
- Messages and invitations the title would *receive* (`MESSAGE`, `INCOMING_INVITATION` and the
  `Recv*` imports) are not wired; a provider may push those events, but the receive imports are
  unchanged.
- The non-context handler (`sceNpBasicRegisterHandler`) is unchanged and does not receive events.
- The friend-list getters (`sceNpBasicGetFriendListEntryCount` and the rest) still describe an
  empty list. Titles that build their list from events, as the context-sensitive model intends,
  get the full list through the events above.
