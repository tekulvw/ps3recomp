/*
 * ps3recomp - libnet socket observer (docs/network-observer.md)
 *
 * Off until an embedding application installs a callback. Once installed, the host-socket libnet
 * (libs/network/sysNet.c, PS3_NET_ONLINE) reports every guest socket call that opened, closed or
 * moved data, after the call returns, on the guest thread that made it: one event per call, with
 * the peer, the bytes moved, the error and how long a send took. Receives that would block (the
 * empty polls of a nonblocking socket) are not reported. Nothing is logged or stored here; the
 * application aggregates what it needs (for example a session's network health) and must return
 * quickly, because the guest thread waits for it.
 *
 * The callback may run on several guest threads at once. Installing or removing it is atomic; a
 * call already in flight may still finish with the previous callback.
 */
#ifndef PS3EMU_NET_HOST_OBSERVER_H
#define PS3EMU_NET_HOST_OBSERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3RECOMP_NET_OBSERVER_VERSION 1

enum ps3recomp_net_event_kind {
    PS3RECOMP_NET_CONNECT = 1, /* connect: started (error 0 or EINPROGRESS) or failed */
    PS3RECOMP_NET_ACCEPT  = 2, /* accept handed out a new socket (socket) from a peer */
    PS3RECOMP_NET_CLOSE   = 3, /* socketclose */
    PS3RECOMP_NET_SEND    = 4, /* send or sendto */
    PS3RECOMP_NET_RECV    = 5  /* recv or recvfrom that returned data, end of stream or an error */
};

enum ps3recomp_net_transport {
    PS3RECOMP_NET_STREAM   = 1, /* TCP */
    PS3RECOMP_NET_DATAGRAM = 2  /* UDP */
};

struct ps3recomp_net_event {
    uint32_t version;     /* PS3RECOMP_NET_OBSERVER_VERSION */
    uint32_t kind;        /* enum ps3recomp_net_event_kind */
    int32_t  socket;      /* the guest's socket number */
    uint32_t transport;   /* enum ps3recomp_net_transport */
    int64_t  host_socket; /* the host handle (POSIX fd or Winsock SOCKET), for read-only queries
                             such as the TCP round-trip time; -1 when there is none */
    uint8_t  peer_addr[4];/* IPv4 address in network order: the connect or send destination, the
                             receive source, the accepted peer; all zero when the call names none
                             (send and recv on a connected socket) */
    uint16_t peer_port;   /* host order; 0 with peer_addr */
    uint16_t reserved;
    int32_t  bytes;       /* bytes moved (0 is a stream's end); -1 when the call failed */
    int32_t  error;       /* the libnet errno (BSD numbering, 35 = EWOULDBLOCK) the call failed
                             with, 0 on success */
    uint64_t elapsed_ns;  /* monotonic nanoseconds the host call took (sends and connects; 0 for
                             the others) */
};

typedef void (*ps3recomp_net_observer_fn)(const struct ps3recomp_net_event *event);

/* Installs `fn` (NULL removes it) and returns the previous callback. */
ps3recomp_net_observer_fn ps3recomp_net_set_observer(ps3recomp_net_observer_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_NET_HOST_OBSERVER_H */
