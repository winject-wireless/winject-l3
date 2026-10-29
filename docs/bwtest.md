# BW test timestamps and latency

Both peers run `app_bwtest` on the paired L3 queue upstream. The P4 apps
expose its UDP console on port 5100. Run `clr` on the receiver, then
`test_send <offer_kbps> <duration_ms>` on the sender, and query `stats` on
the receiver during or after the send.

`stats` retains the packet/byte/drop counters and adds:

- `lat_samples`: timestamped packets included in latency statistics.
- `lat_invalid`: packets with missing/unknown headers, zero timestamps, or
  timestamps ahead of the receiver clock. These still count as received.
- `lat_last_us`, `lat_min_us`, `lat_avg_us`, `lat_max_us`: one-way latency
  in microseconds since the last `clr`; zero when there are no samples.
  The average is an integer arithmetic mean.

Latency runs from immediately before sender queue submission until receiver
queue polling. It includes application queues, L3/FEC, radio transport, and
receiver scheduling, including up to the 10 ms idle receive poll interval
(plus scheduler delays). It does not measure camera or display latency.

Both devices must use synchronized Unix realtime clocks. The P4 apps already
use krono to synchronize the rover against the ground. Check synchronization
before interpreting results: clock offset or adjustment biases latency;
positive clock errors cannot be detected from a packet timestamp alone.
Negative differences are excluded and counted in `lat_invalid`.

Wire format: 16 bytes, followed by zero padding to the configured payload size.
Bytes 0..3 are ASCII `BWT1`, bytes 4..7 are the sequence number (little-endian
uint32), and bytes 8..15 are send time in microseconds since the Unix epoch
(little-endian uint64). Payload size includes this header and must be at least
16 bytes. Existing configured/default payload sizes and offered rates retain
their total datagram sizes. Update both peers for latency; old packets still
contribute to received packet/byte counters but have no latency sample.
