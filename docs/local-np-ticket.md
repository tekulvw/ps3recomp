# Local development NP tickets

`PS3RECOMP_NP_LOCAL_TICKET=1` enables a local provider for
`sceNpManagerRequestTicket2` and `sceNpManagerGetTicket`. Configure the identity
with `PS3_NP_ONLINE_ID` before starting the title. The payload is the normalized
lowercase ASCII online ID (3–16 letters, digits, underscores or hyphens), with
no trailing NUL. This is an account-specific development payload, not a signed
Sony ticket. Replacement services must opt into this format and resolve it to
their stored account. A ticket must not be a shared constant or a permanent
account ID; the server's stored identity remains authoritative.

The requested guest NP ID must match the configured identity. A mismatched ID
returns `SCE_NP_ERROR_DIFFERENT_USER`. The optional guest version must be 2.1;
service ID must be nonempty. Cookie and entitlement processing are unsupported
and return `CELL_ENOSYS`. No PSN or PSNR ticket request is sent.

The manager reports ONLINE in this mode. Registering its callback defers an
ONLINE notification through the same cancellable manager state. Replacing a
registration cancels the previous pending notifications and ticket cache.
Ticket request success queues a
completion for a subsequent `cellSysutilCheckCallback` with a guest caller
available: event 255, result equal to payload length, and the registered guest
argument. The request remains busy through callback execution, including
reentrant polling. Unregistering the manager callback, terminating NP, or
observing the provider disabled cancels pending ticket delivery and clears
cached bytes. Term/unregister also permit recovery when a guest callback exits
its polling thread without returning; a stale callback cannot reset a newer
registration's delivery state. The runtime guest dispatcher must execute these
callbacks synchronously, as the native PPU dispatcher does.

GetTicket supports a NULL-buffer size query and big-endian guest size fields.
An undersized destination returns INSUFFICIENT_BUFFER and the required size
without copying a partial identity. A request still pending, or missing cached
ticket, returns INVALID_STATE. The provider is disabled by default, preserving
offline behavior; a configured PSNR connection does not grant this local ticket
capability by itself. GetTicketParam and actual PSN authentication are not
implemented by this provider.

Run `python3 tools/test_np_local_ticket.py` to exercise the real NID handlers,
identity resolver, guest memory and sysutil pump, including separate PlayerRetail
and Player payloads and cancellation/reinitialization.
