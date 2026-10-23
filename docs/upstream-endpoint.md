# Unified upstream endpoint

`HostEndpoint` is the configurable application-facing component for UDP and
registered queue transports. `App` constructs one component type and calls
`open(reactor, config, tx_wake)`; `config.mode` selects queue transport, otherwise
the configured rx/tx addresses select the existing UDP mode. The public config
file schema and wire framing are unchanged.

The component contains one selected transport session and `DatagramAir` for FEC,
buffering, deadlines and air SDUs. The former `UdpEndpoint` and `QueueEndpoint`
classes have been removed. The config struct holding either UDP addresses or
queue names is now called `EndpointAddress`.

## Transport selection

For UDP, rx and tx remain host:port strings. Both addresses select static mode;
rx alone selects a server that replies to its last nonempty-datagram sender;
tx alone selects a client with an ephemeral local port. Closing/reopening resets
learned peer state. An unknown server peer never inherits a prior destination.

For queues, rx is the consumed application-to-air queue and tx is the produced
air-to-application queue. Both names and a CV reactor name are required. The
embedding host owns the queues and reactor. Queue objects must be distinct,
have nonzero capacities, and keep those capacities fixed while bound. Each input
queue has one consumer: HostEndpoint rejects conflicting input bindings by object
identity, including aliases under different registry names. The embedding host
must also avoid attaching other component consumers to that input.

## Lifecycle and threading

The owner serializes open/close on the manager IO reactor thread, or while that
reactor is stopped. The IO reactor must outlive the session's close. TX wake is
a notification callback and must not synchronously close/destroy the endpoint
from inside a FEC timer notification.

- Double open fails and preserves the existing active session.
- An initial failed open releases acquired bindings/socket resources and leaves
  the endpoint closed, ready for retry.
- `stop_input()` fences and unregisters transport ingress while retaining pending
  TX/FEC and application delivery, so shutdown can flush through TxMux timers.
- Close is idempotent. It fences ingress, unregisters the selected transport,
  marks it closed, stops FEC timers and clears transport resources.
- Reopen creates a fresh session, clears learned peers, resets LC receive state
  and session counters, and can select a different transport.
- Closed-session counters remain readable until the next successful open.

UDP input runs on the IO reactor, in batches of at most eight datagrams. Queue
input runs on the host CV reactor, in batches of at most 32 entries, with a wake
when more remain. Radio RX and FEC receive polling normally run on the IO reactor.
The FEC adapter and transport state have their own locks.

Reactor callbacks capture weak session references rather than endpoint pointers.
Close waits for active ingress but does not require a CV reactor acknowledgement.
A callback already copied before close can safely resolve to an expired or
closed session. Because BFC scans raw queue-context pointers, a deferred reactor
callback retains the input queue until the scan can finish. If the reactor is
stopped, it retains that lease until it resumes or is destroyed. Endpoint memory
and FEC processing do not remain active merely because that lease exists.

## Delivery and counters

Raw datagrams and decoded output use one internal delivery path that reports
accepted, no peer, closed or local error. UDP uses a nonblocking datagram send;
queue delivery copies one datagram and wakes the peer reactor. A full queue
accepts the new entry and evicts the oldest entry, recording both outcomes.

FEC processing is independent of application peer readiness. If a UDP server has
not learned a peer, its decoded/raw output is dropped and counted at delivery.
There is no application-output backlog waiting for peer discovery.

`transport_counters()` returns a consistent snapshot of delivery attempts,
accepted packets/bytes, no-peer drops, closed drops, send errors and queue
evictions. Neither socket send success nor queue insertion proves peer receipt.
The existing app RX counters count payloads handed to transport delivery;
the new `upstream_<id>_app_delivery_*` metrics explain their local outcomes.

FEC, TX queue and wire-format behavior are described in
[upstream-fec.md](upstream-fec.md).
