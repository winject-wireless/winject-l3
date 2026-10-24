# Upstream endpoints

`Upstream` owns radio RX buffering, FEC state, timer registrations and common
metrics. `UDPUpstream` and `QueueUpstream` implement application delivery.
Instances have immutable transport/FEC configuration and no worker thread.
Air framing is unchanged.

## Construction and ownership

Construct with `std::make_shared<UDPUpstream>(cv, io, config, radio_tx_queue)` or
`std::make_shared<QueueUpstream>(cv, config, radio_tx_queue, app_tx_queue,
app_rx_queue)`,
then call `bind_callbacks()` before publishing the instance. `Upstream` inherits
`enable_shared_from_this`; binding requires shared ownership so callbacks can
retain weak references. Repeated binding is harmless.

There are no public open, close, stop-input, drain or FEC reconfiguration methods.
Destruction removes callbacks and timers and discards pending work. Reconfigure
by replacing the instance. Management FEC updates do this; invalid configuration
is rejected before replacement. An instance replacement resets learned UDP peers,
receive sequence state, codec state and per-instance counters. Scheduler-budget
updates alone do not require replacement.

The caller supplies shared pointers to the CV reactor and radio TX queue. UDP
additionally retains a shared pointer to its I/O reactor. `DatagramAir` retains
the CV reactor and uses its timer and wake method directly. The caller runs the
reactors. `App`
runs one shared CV reactor for UDP upstream processing, while queue upstreams use
the externally registered host CV reactor.

`rx_queue()` returns a `shared_ptr` to the upstream-owned bounded, lossy RX queue.
`upstream-N.rx_queue_capacity` sets its capacity (default 1024, must be positive)
and the internal UDP application inbox capacity. Supplied application queue
capacities remain the responsibility of their producers.
Radio registration stores that queue and the supplied TX queue. The table holds
only a weak upstream reference for statistics. Unregister radio access before
releasing the instance. Retaining a queue does not retain its upstream callback
target. Deferred queue leases protect BFC's raw-pointer reactor scan snapshots.

## Receive path

RX demux validates and depayloads MPDUs and routes each LC-framed SDU by bus ID.
It does not strip the LC header, interpret FEC flags, filter sequence numbers,
decode FEC or deliver application packets. It pushes the SDU into the registered
RX queue. The queue is bound using BFC `set_reactor`; push automatically wakes
the CV reactor.

The upstream's CV callback drains the inbox in turns of at most 32 packets. It
checks LC sequence numbers, interprets the FEC flag, and feeds `DatagramAir`.
Both RX recovery timers and TX flush timers use this same CV reactor. Recovered
or raw datagrams call the derived `deliver_datagram` method: nonblocking UDP send
or push into the application output queue.

## Transmit path

The scheduler creates a bounded `BoundedLossyQueue` through `TxMux::make_tx_queue`
and the caller passes it to the upstream constructor. Raw packets and encoded
shards are pushed immediately into this queue; there are no upstream TX pull,
size or availability methods. Queue push automatically requests scheduler work.
The scheduler peeks/pops the queue directly while respecting airtime pacing,
scheduling budgets and frame capacity. Its queue wake binding is safe if a
producer retains the queue after scheduler destruction.

Queue application input is registered with the supplied CV reactor and bound for
automatic push notification. UDP's I/O callback receives at most eight datagrams
per turn and enqueues them into an internal application inbox. The CV reactor
consumes application input in batches of 32 and performs encoding. I/O and CV
reactors can therefore run on different caller-owned threads without splitting
FEC state across those threads.

Application queues must be distinct, have nonzero capacities and retain fixed
capacities while registered. Each input queue has one consumer; conflicting
upstream input bindings are rejected by queue identity. The application output
consumer binds that output queue to its own reactor if it needs notification.

## Delivery and counters

FEC processing is independent of UDP peer readiness. A UDP server learns its
peer from incoming application datagrams; received radio output before discovery
is dropped and counted. No output backlog waits for a peer.

`rx_counters()` tracks application delivery attempts, accepted packets/bytes, missing-peer
drops, send errors and output-queue evictions. Local acceptance does not prove
peer consumption. `tx_counters()` tracks application input packets/bytes,
oversize rejection and radio TX queue evictions separately. FEC TX counters
count shards submitted to the supplied TX
queue, including shards that can later be evicted. TX pending statistics include
partial encoder input and queued output. Overflow retains the newest packets.

See [upstream-fec.md](upstream-fec.md) for codec behavior and
[reactive-tx.md](reactive-tx.md) for scheduler pacing.
