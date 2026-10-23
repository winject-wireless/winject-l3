# Upstream FEC

Winject uses a local vendor copy of vstreamer's RS codec from commit
`5cd91b7e8dfb5190ca08852b6e066147be780921`. Provenance, source hashes and local
adaptations are recorded in [the vendor README](../src/manager/fec/vendor/README.md).
A shared library extraction is deferred to a separate task; this build does not
depend on a vstreamer checkout.

## Wire compatibility and telemetry

Winject's existing three-byte LC header still carries the bus, FEC flag and
sequence number. RS shards retain their five-byte header and two-byte original
length prefix. The existing shard budget is 1445 bytes, allowing application
payloads up to 1438 bytes with upstream FEC. Configuration still requires
`1 <= k < n <= 31`.

The LC FEC flag alone selects decoding. Raw datagrams are passed through
unchanged. Vstreamer data and telemetry headers remain part of the opaque
application payload; their shared transport does not require winject to inspect
or rewrite them. This change does not introduce a winject telemetry protocol or
add vstreamer's outer four-byte header to the winject air protocol.

## Sending

The encoder collects up to k originals, then returns systematic and parity
shards. Partial blocks flush at their actual codec deadline, defaulting to 20 ms.
A zero timeout disables automatic flush; explicit shutdown/reconfiguration still
flushes pending originals. Short blocks omit their virtual zero-pad data shards.

Raw datagrams and encoded shards share one 1024-packet queue. Overflow drops
the oldest queued packet and increments `upstream_<id>_tx_queue_drops`. This is
packet-level loss and can remove individual shards from a block. Packet sizes
are bounded by the fixed air MTU; pending unencoded input is limited to one
partial block. Pending statistics include those originals as well as queued
output.

The adapter serializes ingestion, output pulls and FEC updates. A valid k/n,
enable/disable or timeout change flushes the old partial block and applies the
new TX configuration in one locked transition. Invalid k/n leaves pending input
and active configuration intact. The TX sequence continues across changes.

## Receiving

Separate RX codec state survives TX reconfiguration. Shard headers determine
receive k/n, including on an upstream whose local TX FEC is disabled. Such input
continues to increment the existing unexpected-FEC metric.

The receiver emits a contiguous systematic prefix of the current head block
without waiting for recovery. Later block output waits for that head to complete
or reach its deadline. At give-up, surviving systematic packets are released
and missing originals are counted. Packets emitted early are not emitted again
when recovery completes. This changes receive ordering/latency compared with
the previous block-at-once implementation.

Deadlines follow the vendored codec's measured sender spread/jitter policy:
initially it assumes a 250 ms spread cap plus a minimum 5 ms margin. The adapter
polls pending receive state on the manager IO reactor every 10 ms, so buffered
output can advance even if no further datagrams arrive. Loss-free head packets
can still leave immediately. No intentional TX shard spreading is added here;
the existing TxMux airtime pacing remains responsible for sending MPDUs.

Receive and transmit state are protected by the adapter mutex. Reactor timers
capture a shared lifetime guard; cancellation invalidates callbacks already
extracted by the timer scheduler, and close fences an active timer callback.
The owner must serialize start/stop and detach transport ingress before
destroying the endpoint. The unified HostEndpoint uses weak session callbacks,
serializes ingress with close and defers queue-context release until a CV reactor
scan can finish; close does not wait for a timed reactor acknowledgement.

## Builds and tests

Linux builds use the existing ISA-L NEON backend on aarch64 and the vendored
portable encoder elsewhere. The ESP-IDF component builds the same sources with
portable ISA-L math and optional ESP32-P4 PIE parity encoding, controlled by
`CONFIG_WINJECT_RS_PIE`.

Tests include the vendored receive sequencing/deadline suite, prior winject
recovery/MTU tests, golden shard bytes from the previous winject codec, bounded
backlog, runtime timeout/k/n changes, receive expiry without new arrivals,
early-delivery deduplication, opaque telemetry forwarding, concurrent ingestion
and reconfiguration, and callbacks extracted before destruction.
