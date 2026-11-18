# Upstream FEC

Each upstream carries its datagrams through a [winject-fec](https://github.com/winject-wireless/winject-fec)
write stream (TX) and read stream (RX), the library vstreamer also moves to. The pin is in
[`cmake/WinjectFecFetch.cmake`](../cmake/WinjectFecFetch.cmake); the streams run on the
upstream's CV reactor. [`DatagramAir`](../src/core/endpoint/DatagramAir.h) connects them to the
app and the radio.

## LC framing

Every slot is `u8 bus | u8 lc_type | u16 sn (BE) | body` (see [winject.md](winject.md#lc-header)):

- `lc_type` 0 (plain): the body is the app datagram. 1 (fec): a winject-fec shard. Other types
  are dropped and counted (`air_rx_lc_type_unknown`).
- `sn` counts every slot an upstream sends on its bus, plain, data and parity alike, so the
  receiver sees air gaps and duplicates whatever the type.
- The type, not the body, decides decoding: app bytes are never inspected (the 0xF1 outage of
  v1.0.0 cannot come back).
- No version field: both ends run matching managers. This layout replaces the 3-byte header
  (`bus | is_fec:1 seq:15`); older managers do not interoperate with it.

The prefix takes one byte more than before: a plain upstream carries datagrams up to **1444**
bytes (was 1445); an FEC upstream up to **1438** (unchanged).

## Sending

- The upstream copies each app datagram once into a buffer with 7 bytes of room in front:
  `[LC room 4][fec slot 3][datagram]`. The write stream writes its data header into the slot
  and the upstream writes the LC header into the room, so a data shard goes to the radio queue
  without another copy. Plain datagrams lose the unused fec slot from the front of the view.
- Data shards go out at once; they never wait for parity or for the block to fill.
- A block closes after k datagrams or `fec.timeout_ms` after its first one. It always sends its
  full n − k parity, also when the timeout closed it short.
- Parity row j of a block of c shards is released at
  `max(close + j·spread/c, first + (k + j)·spread/c)`. With `fec.spread_ms` = 0 (the default)
  that is right after the block's last data shard. With a spread, a block's shards span about
  `fec.spread_ms` from its first datagram, and consecutive blocks interleave. See
  [fec-spreading.md](fec-spreading.md) for when that helps: against fades in time, not against
  bursts that take back-to-back datagrams.
- Raw and FEC slots share one 1024-packet radio TX queue; overflow drops the oldest
  (`upstream_<id>_tx_queue_drops`).
- Parity waiting for its release time lives in the write stream and is not in `tx_pending_*`,
  which now reports the radio queue only.

## Receiving

- Plain bodies go to the app at once, as a view of the radio buffer.
- FEC data shards go out in sequence: at once when nothing before them is missing. Behind a
  hole they wait until the hole is rebuilt from its block's parity, or given up. The order
  matters to consumers such as an RTP depacketizer, which drops a packet that arrives after a
  later one.
- A hole is given up at a deadline learnt from the sender's measured spread, bias and jitter;
  until two blocks have been measured it is 250 ms after the block's close. Without parity (a
  data-only stream) a hole is given up as soon as later data shows the sender is past it.
- k/n come from each parity header, so the receiver needs no FEC configuration. An upstream
  with FEC off still decodes fec bodies and counts them in `upstream_<id>_air_rx_fec_unexpected`.
- `fec_rec` counts data shards rebuilt from parity; `fec_lost` counts blocks that lost data
  (given up, failed or evicted) plus malformed and k/n-mismatched shards.

## Lifecycle

- A FEC change (type, k/n, timeout, spread; config, API or `uu`) applies to the running
  upstream: the write stream takes it from its next block. The open block finishes under the
  settings it started with and keeps its parity, the air sn and the FEC data sequence run on,
  and the receive side, which reads k/n from the shards, needs nothing. No restart, no
  `on_queue` callback, nothing dropped.
- `stop()` may run on any thread but the CV reactor's; it waits out an in-flight data-path call,
  and the streams' timers hold the streams only weakly.

## Builds and tests

- Host builds add winject-fec as a subdirectory (`winject::fec`, which brings ISA-L with NEON on
  aarch64). The ESP-IDF component requires the `winject_fec` component: a project adds
  `${WINJECT_FEC_SOURCE_DIR}/idf/winject_fec` to `EXTRA_COMPONENT_DIRS` after including
  `src/BfcFetch.cmake` and `cmake/WinjectFecFetch.cmake` (see `idf/example`). Its Kconfig has
  the PIE encode kernel and the PSRAM row options (`WINJECT_FEC_*`), which replace
  `CONFIG_WINJECT_RS_PIE`.
- winject-fec has its own codec, stream and full-path tests. Here, `DatagramAirTest` covers the
  slot layout, data-at-once and parity timing, spread, timeout flush, a lossy loopback with
  in-order recovery, give-up on the reactor timer, sequence carry-over and teardown;
  `LCHeaderTest` the wire layout, wrap and unknown types; `UpstreamTest` and `AppShutdownTest`
  the upstream and live FEC changes.
