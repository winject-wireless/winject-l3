# FEC block spreading

Bench report and guide for spreading the shards of an FEC block over time. The feature lives in
the vstreamer sender (`stream_sender`, `fec_spread_ms`); this page covers why the winject link
needs it, how to size it, and what was measured on the rover ↔ GS bench.

## Summary

- Loss on the winject link is bursty. Frames sent back to back are lost together, and runs of
  10–25 lost frames are common.
- An FEC block that goes on air in a few milliseconds can lose more than its n − k parity shards
  to a single dropout, even when the average loss is well inside the code's tolerance.
- Spreading each block's shards over about 25 ms, with consecutive blocks interleaved, cut
  post-FEC packet loss about 3× at both 2.5 and 10 Mbit/s. It adds no airtime; the cost is
  latency, about equal to the spread.
- Spreading evens loss out; it does not lower it. The FEC still needs parity above the raw loss:
  at 10 Mbit/s, full 16/24 blocks (33 % tolerance) against about 18 % raw loss were not enough,
  even with spreading.

## The problem: burst loss

Measured on the bench link (RTL8812AU rover → RTL8811AU GS, ch 13, OFDM_36M, 2 dBm, about 15 %
raw loss):

- **Loss is not random.** At 16/24 and 15 % loss, random loss would fail about 0.7 % of blocks;
  the link failed 5–6 %. Replaying captured loss patterns needs n ≥ 60 at k = 16 for zero
  failed blocks without spreading.
- **Back-to-back frames fail together.** The first shard of a block, sent after an idle gap, was
  lost 10–14 % of the time; every later shard 16–25 %. On the WireGuard path (upstream 0, L3
  FEC 1/3 repetition), the three copies of a packet arrive within microseconds of each other
  and are lost together: 34 % shard loss gave 22 % packet loss, against about 4 % if losses
  were independent.
- **A slower link does not lower raw loss.** Spacing every frame in the L3 (`winject.tx_gap_us` up
  to 1000 µs, or `winject.max_rate_kbps`) left raw loss at 15–16 %. It did halve block
  failures (5.5 % → 3.4 %) by spreading each block over 22–26 ms, but it caps the whole link.

## How spreading works

The vstreamer sender gives every queued shard a release time. When a block of `n_sent` shards
(its `sdu_n` data shards plus n − k parity) closes at time t0, shard j is released at:

```
t0 + j × spread_ms / n_sent        (data first, then parity)
```

- The send queue is ordered by release time. Blocks overlap, so the shards of consecutive
  blocks interleave on the wire (A0 B0 A1 B1 …), and a dropout is shared between blocks.
- There is no rate cap; only the timing changes. The queue holds about `spread_ms` of output.
- The stream header sequence is stamped at send time, so it stays monotonic on the wire.
- Raw (`fec none`) datagrams are never delayed. `spread_ms = 0` is the previous behaviour.
- Blocks closed by the FEC timeout keep their full n − k parity and use the same window.
- **No wire change.** The receiver already keeps several blocks open, releases data shards as
  they arrive and delivers in order.

## Sizing the spread

A dropout of B ms takes about B / S of a block spread over S ms. With background loss p on the
rest, a block of tolerance t (parity share of the shards sent) survives when:

```
p + (1 − p) · B / S  ≤  t        →        S  ≥  B · (1 − p) / (t − p)
```

The gap t − p decides how far S has to stretch. With p ≈ 15 %:

| Block | t | S per ms of dropout | S for a 10 ms dropout |
|---|---|---|---|
| full 16/24 | 33 % | ≈ 4.7 | 47 ms |
| 9 data + 8 parity (16/24 at 2.5 Mbit/s, 20 ms timeout) | 47 % | ≈ 2.7 | 27 ms |
| full 16/31 | 48 % | ≈ 2.6 | 26 ms |

- **The needed S does not depend on bitrate** for a given dropout and tolerance; only the shard
  spacing (S / n_sent) changes.
- **Choose parity first, then S.** Keep t − p at about 0.25 or more, then set S to about 2.5–3×
  the typical dropout. On this bench that is about 25 ms.
- **Upper limit.** The receiver gives up on a head-of-line block 60 ms (`emit_hold_ms`) after a
  later block is ready, so the sender caps `fec_spread_ms` at 40.
- **Latency.** The worst case for a packet that has to be recovered is about FEC timeout + S.

## Configuration (vstreamer sender)

| Where | Setting |
|---|---|
| Command line | `uvc_stream_sender --fec-spread-ms 25` |
| TX console (runtime) | `set_fec_spread 25` (0–40; out of range replies `err bad fec spread ms`) |
| Component key | `fec_spread_ms` (configure and query) |

The default is 0. A runtime change applies to blocks closed after it and is lost when the
sender restarts. To keep it on the rover, add `--fec-spread-ms 25` to `SENDER_ARGS` in
`/etc/rover/sender.env`.

## Bench results

Rover NUC → GS, winject ch 13, OFDM_36M, 2 dBm, FEC 16/24, rover L3 default pacing, 2 min per
run. Post-FEC loss is the receiver's lost-after-FEC count over application packets.

| Video rate | spread_ms | Raw loss | Blocks failed | Post-FEC loss | need_idr | Display fps |
|---|---|---|---|---|---|---|
| 2.5 Mbit/s | 0 | 13.9 % | 4.1 % | 2.54 % | 95 | 12.3 |
| 2.5 Mbit/s | 25 | 18.1 % | 1.2 % | 0.91 % | 21 | 17.6 |
| 10 Mbit/s | 0 | 18.1 % | 16.2 % | 8.28 % | 985 | 0.2 |
| 10 Mbit/s | 25 | 17.9 % | 3.8 % | 2.31 % | 171 | 4.1 |

- The 2.5 Mbit/s spread-25 run had worse radio conditions than its baseline and still lost
  about 3× fewer packets.
- With spreading, the longest run of consecutive lost wire packets fell from 20+ to 11–13.
- An earlier sweep at 2.5 Mbit/s also ran spread 15 and 40. Spread 40 matched 25, and 15 gave
  about half the gain. That sweep predates the vstreamer late-flush fix (below), so only the
  ordering of those values carries over.
- 10 Mbit/s ran at about 1580 frames/s, about 72 % airtime at 36M, with no sender or L3
  drops. Most blocks were full 16/24, too close to the raw loss; that rate needs more parity
  (for example 16/31, near link capacity at 36M), a faster modulation, or lower raw loss.

## Related fixes found during this work

| Repo | Change | Effect on the link |
|---|---|---|
| winject-l3 v1.0.1 (#4) | FEC flag in the LC header instead of the `0xF1` payload magic; 5-byte shard header | Video packets with stream sequence 0xF1xx (256 every 65536) were swallowed by the L3 FEC decoder: a 0.5 s outage every 55–140 s |
| winject-l3 (#5) | Console request cancel no longer re-enters itself | A single radio ping timeout recursed until the manager segfaulted |
| vstreamer (#9) | The send thread wakes when a new FEC block opens | The 20 ms FEC timeout fired up to 50 ms late on an idle sender, so blocks kept filling (16 + 2 per about 60 ms instead of about 9 per 20 ms) |
| rover config | Upstream 0 buses b2/a1 and bind `127.0.0.1:22180` | WireGuard over winject had never passed traffic |

## Open work

- **Dynamic spreading.** The receiver would measure dropout duration (p95) and the share of
  each block lost, and report them on the reverse link. A controller (for example
  `cbr_controller`) would then set `S = B · (1 − p) / (t − p)` with a margin, within a latency
  budget, and escalate to more parity and then lower bitrate when S hits the cap. This needs a
  link report format change.
- **L3 FEC.** The L3 encoder (`RsBlockErasure` in this repo) still sends a block's shards back
  to back. WireGuard over upstream 0 loses about 20–30 % of pings for that reason; the same
  spreading in `TxMux` or `UdpEndpoint` would apply.
- **Raw loss.** About 15 % raw loss on the bench link at short range is high. During the 22–32 ms
  gaps the GS adapter received nothing while an ESP32 sniffer still heard 25–35 frames. This is
  a separate radio-side investigation.
