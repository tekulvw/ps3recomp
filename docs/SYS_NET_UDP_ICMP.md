# Datagram sockets and ICMP errors

## Behaviour (the specification)

On the console, libnet is a BSD stack. A datagram socket that has **not** been
connected never reports an ICMP error to its owner. If a peer's host answers a
datagram with "port unreachable" or "network/TTL" ICMP, the error is dropped:
the next `recvfrom` sees only the datagrams that actually arrived, or
`EWOULDBLOCK` when none did. A title can therefore treat any `recvfrom` error
other than `EWOULDBLOCK` on such a socket as fatal, and titles do.

The host path must keep that contract on every host:

- A `recvfrom`, `recv`, `sendto` or poll on an unconnected guest datagram socket
  never fails or wakes because an earlier datagram drew an ICMP error.
- Data that arrives later is still delivered normally.
- Stream sockets are not affected. A reset or refused TCP connection still
  reports its error.

## Why the host needs help

POSIX hosts (macOS, Linux) already behave this way for unconnected datagram
sockets. Winsock does not. By default, after a datagram draws ICMP "port
unreachable", the next `recvfrom` on that socket fails with `WSAECONNRESET`,
which is `ECONNRESET` once translated. After a TTL/network ICMP it fails with
`WSAENETRESET`. A single stray ICMP is enough to make a title believe its
server connection was reset. For example, a load balancer may briefly reject
one packet, or a server's UDP port may close while its TCP session lives on.

## The host mapping

On Windows, `sys_net_bnet_socket` turns both reports off for every datagram
socket it creates. It uses the per-socket controls Winsock provides for this,
`SIO_UDP_CONNRESET` and `SIO_UDP_NETRESET`, each set to FALSE. If a control is
refused, the socket still works with the host default, and the refusal is
logged once. POSIX hosts need no change.

Known limit: BSD reports ICMP errors on a **connected** datagram socket, as
`ECONNREFUSED` from the next call. With the controls off, a connected guest
datagram socket on Windows hears no ICMP errors at all. No title we run
connects its datagram sockets. A title that does would need the refusal
mapped per socket.

## Test

`libs/network/tests/test_sys_net.c`, "a datagram that draws ICMP": a guest
datagram socket sends to a loopback port nobody listens on (the loopback host
answers with port unreachable). The next nonblocking `recvfrom` must say
`EWOULDBLOCK` (errno 35), not `ECONNRESET` (54), and a datagram sent afterwards
must still be received. Without the controls the case fails on Windows. It
passes on POSIX hosts, which is the behaviour it pins.
