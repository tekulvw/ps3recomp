# libnet socket observer

`include/ps3emu/net_host_observer.h` lets an embedding application see the guest's socket traffic
on the host-socket libnet (`libs/network/sysNet.c`, enabled with `PS3_NET_ONLINE`) without
logging packets. It exists so an application can measure a session's network health (packets and
bytes per peer, silences between arriving packets, blocked sends, socket errors) next to its own
frame timing, and tell a network stall from a slow frame.

## Behaviour

- **Off by default.** No callback is installed at start. Each observed handler then does one atomic
  load and calls libnet exactly as before.
- `ps3recomp_net_set_observer(fn)` installs a callback, `ps3recomp_net_set_observer(NULL)` removes
  it; each returns the previous one.
- The guest-facing handlers report a call **after it returns**, on the guest thread that made it:

  | Event | Reported for | Peer |
  | --- | --- | --- |
  | `PS3RECOMP_NET_CONNECT` | every `connect`, started (error 0 or `EINPROGRESS`) or failed | the address the guest named |
  | `PS3RECOMP_NET_ACCEPT` | an `accept` that handed out a socket (`socket` is the new one) | the accepted peer |
  | `PS3RECOMP_NET_CLOSE` | every `socketclose` | none |
  | `PS3RECOMP_NET_SEND` | every `send` and `sendto`, including blocked (`EWOULDBLOCK`) and failed ones | `sendto`'s destination; none for `send` |
  | `PS3RECOMP_NET_RECV` | `recv` and `recvfrom` that returned data, a stream's end (0 bytes) or an error | `recvfrom`'s source when the guest asked for it; none for `recv` |

- A receive that fails with `EWOULDBLOCK` is the empty poll of a nonblocking socket and is not
  reported, so a busy poll loop costs nothing extra.
- Each event carries the guest socket number, its transport (stream or datagram), the host socket
  handle (for read-only queries such as the operating system's TCP round-trip time), the bytes moved
  (-1 on failure), the libnet errno on failure (BSD numbering) and, for connects and sends, how long
  the host call took in monotonic nanoseconds.
- The callback may run on several guest threads at once and the guest thread waits for it, so it
  must be quick and thread-safe. It must not call back into libnet.

## Tests

`libs/network/tests/test_sys_net.c` installs a recording callback and checks the events of UDP
and TCP calls over loopback: peers, bytes, errors, that an empty poll is not reported, and that
removing the callback stops the events.
