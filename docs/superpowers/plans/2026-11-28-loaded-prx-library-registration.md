# Loaded guest PRX library registration

Implement in the isolated SDK checkout based on e01945ed.

1. Write synthetic registry/descriptor fixtures first and observe the missing API failure.
2. Add a typed, library-scoped batch registry API with bounded dynamic allocation, duplicate/range checks, synchronized publication/lookup/unregistration, and explicit failure.
3. Preserve the legacy module/export ABI and function-dispatch lookup while moving module publication onto the validated batch registry.
4. Implement sysPrxForUser register/unregister for the established 0x1c big-endian guest descriptor. Keep function OPDs and variable addresses distinct; reject unsupported TLS and descriptor formats.
5. Run synthetic registration, overflow/atomicity, descriptor, compatibility, and concurrency checks. Confirm generated firmware NIDs include both handlers.
6. Record semantics and source references. Report the diff and source commit for parent review; do not change the primary pinned checkout or publish releases.
