# Winject architecture

Winject carries UDP application traffic between hosts over raw 802.11 frames, without association, ACKs or an access point. Each host runs **winject-manager**, which talks to one winject **radio** over Ethernet. The manager packs application datagrams into 802.11 MPDUs and hands them to the radio, which transmits them as-is. A peer radio receives them in promiscuous mode and forwards them to its manager, which unpacks and delivers them to local UDP applications.

This document covers the system architecture, the manager's internal structure, and the on-air framing.

## System overview

```
  host A                                                          host B
+-------------------------------+                    +-------------------------------+
| UDP apps (video, iperf, ...)  |                    | UDP apps                      |
|        ^   |                  |                    |        ^   |                  |
|        |   v  upstreams       |                    |        |   v  upstreams       |
| winject-manager               |                    | winject-manager               |
|   m-plane (UDP 22) --------+  |                    |  +-------- m-plane (UDP 22)   |
|   d-plane (:9000) --------+  |                    |  +- d-plane (:9000)          |
+----------------------------|--+                    +--|----------------------------+
                Ethernet     |                          |     Ethernet
+----------------------------v--+                    +--v----------------------------+
| radio A (ESP32)               |   802.11 MPDUs     | radio B (ESP32)               |
|   inject -> raw TX            | ~~~~~~~~~~~~~~~~~> |   promiscuous RX -> forward   |
|   forward <- promiscuous RX   | <~~~~~~~~~~~~~~~~~ |   inject -> raw TX            |
+-------------------------------+   same channel     +-------------------------------+
```

The two sides are symmetric. Any number of manager/radio pairs can share a channel; a **domain** (Address 3) separates groups, and **buses** separate traffic flows within a group.

### Planes

| Plane | Endpoint | Purpose |
|---|---|---|
| Radio m-plane | radio UDP **22** | Text commands from the manager to the radio: PHY (`radio_tx`), domain filter (`rx_filter_addr3`), capabilities (`radio_caps_info`), counters (`tx_info`, `rx_info`), `save` / `load`, `reset`. Specified in the winject-radio-esp32 repository |
| Radio d-plane | radio UDP **9000** (typical) | One port for inject (24–1472 byte MPDUs), peer registration (1–23 bytes), and forward (MPDU + 4-byte trailer to the registered peer). See the radio repos for the length rules |
| Manager m-plane | `manager.console_in` | Text commands from operators and tools to the manager: upstreams, metrics, radio passthrough. Specified in [mplane.md](mplane.md) |

## Manager

### Components

| Component | Source | Role |
|---|---|---|
| `App` | [`App.*`](../src/manager/App.cpp) | Owns everything below; startup, radio programming sequence, metrics, console handlers |
| `Config` | [`Config.*`](../src/manager/Config.cpp) | Parses the `winject.*`, `upstream-N.*` and `manager.*` keys |
| `IOReactor` | [`IOReactor.h`](../src/manager/utils/IOReactor.h) | epoll reactor (`bfcext::epoll_reactor`); runs all socket I/O and timers on one thread |
| `ConsoleClient` | [`ConsoleClient.*`](../src/manager/console/ConsoleClient.cpp) | Radio m-plane client: correlated requests (`cmd:<u8>`), timeouts, `apply_radio`, `radio_caps_info` query |
| `ConsoleService` | [`ConsoleService.*`](../src/manager/console/ConsoleService.cpp) | Manager m-plane server (`console_in`; replies to sender) |
| `RadioManager` | [`RadioManager.*`](../src/manager/radio/RadioManager.cpp) | Keeps the radio in the configured state: PHY reconcile, filter reconcile, heartbeat, forward re-registration, TX pacing per modulation |
| `WifiUdp` | [`WifiUdp.*`](../src/manager/radio/WifiUdp.cpp) | d-plane socket: MPDUs and 1-byte registration to `radio_device dplane`; checks the forward trailer per FCS mode |
| `TxMux` | [`TxMux.*`](../src/manager/radio/TxMux.cpp) | TX scheduler: picks upstreams, builds MPDUs, paces them to the PHY airtime and `max_rate_kbps` |
| `RxDemux` | [`RxDemux.*`](../src/manager/radio/RxDemux.cpp) | RX: validates domain, splits slots, routes each slot by its LC bus to upstreams |
| `RadioUpstreamTable` | [`RadioUpstreamTable.*`](../src/manager/radio/RadioUpstreamTable.cpp) | Shared upstream list (`bus_tx`, `bus_rx`, upstream pointer), guarded by one mutex |
| `Upstream` / `UdpEndpoint` | [`endpoint/`](../src/manager/endpoint/UdpEndpoint.cpp) | One application flow: UDP socket in `UDP_SERVER_FORWARDING`, `UDP_CLIENT_FORWARDING` or static mode; TX queue; optional FEC |
| `UpstreamStats` | [`UpstreamStats.*`](../src/manager/endpoint/UpstreamStats.cpp) | Stamps and checks the LC header (bus, sequence); duplicate drop and gap counting |
| `RsBlockErasure` | [`fec/`](../src/manager/fec/RsBlockErasure.cpp) | Optional k-of-n Reed–Solomon erasure coding (ISA-L) |
| `Mpdu` / `Frame` / `LCHeader` | [`frames/`](../src/manager/frames/Mpdu.cpp) | On-air framing (see § Framing) |
| `MetricsRegistry` | [`MetricsRegistry.*`](../src/manager/utils/MetricsRegistry.cpp) | Counters exported by `get_metrics` |

### Threads

| Thread | Runs |
|---|---|
| Reactor (main) | All sockets: application UDP, radio m-plane, d-plane forward, manager console. The full RX path (`WifiUdp` → `RxDemux` → `Upstream::on_radio_rx` → FEC decode → UDP send), application RX into upstream TX queues, timers and stats |
| `TxMux` TX thread | Wakes every 250 µs (`k_tx_tick_interval_us`) or on request, builds MPDUs from upstream TX queues and sends them through `WifiUdp`, spacing frames by the modelled airtime (`PhyAirtime`) plus `tx_gap_us`, capped at `max_rate_kbps` |

The two threads share `RadioUpstreamTable` (mutex) and each upstream's TX queue (per-upstream mutex).

### Data path: TX

1. An application sends a UDP datagram to an upstream socket (`UdpEndpoint`, reactor thread). Datagrams over 1445 bytes are dropped and counted (`app_rx_oversize_pkt`).
2. With FEC enabled, the datagram goes into the encoder, which emits k systematic and n−k parity shards; otherwise it is queued as-is.
3. The TX thread picks a primary upstream that has data, then fills the remaining room in the MPDU from other upstreams, up to 5 slots and 1448 body bytes.
4. Each slot payload is prefixed with the LC header (upstream `tx_bus`, FEC flag, per-bus sequence).
5. The MPDU header gets the domain in Address 3, the slot lengths in Address 1–2, and a 12-bit MPDU sequence.
6. `WifiUdp::send` sends it as one UDP datagram to the configured d-plane address. Pacing decides when the next MPDU may go.
7. The radio transmits it unchanged.

### Data path: RX

1. The radio forwards the frame plus trailer to the registered host port. `WifiUdp::on_forward` (reactor) checks the trailer for the radio's FCS mode, and drops and counts failures (`radio_fcs_err_pkt`). It strips the trailer.
2. `RxDemux::on_mpdu` validates the frame (DATA, `CA:FE:BA:BE` prefix, slot sizes fill the body), and drops other domains (`rx_drop_domain`).
3. For each slot, the LC bus selects every upstream with that `rx_bus`. An unknown bus is counted (`rx_drop_bus`), and bus 0 is ignored.
4. `UpstreamStats` checks the LC sequence: it drops duplicates and counts gaps (`air_rx_gap_loss`).
5. If the LC header FEC flag is set, the upstream runs the FEC decoder; otherwise the payload is the datagram. It sends the resulting datagrams to the application: the configured destination (client mode) or the last sender (server mode).

### Control path

At startup, when `radio_device` has an `mplane` address (from config or `radio_device mplane=`), the manager connects to the radio m-plane and:

1. queries `radio_caps_info` to learn the FCS trailer mode (`NOK ENOSYS` from older firmware means `ACTUAL`);
2. programs `radio_tx` (channel, power, modulation, and CCA when `winject.cca` is set) and `rx_filter_addr3` (domain);
3. stores it with `save <slot>`.

While running, `RadioManager` periodically re-reads the PHY and filter and corrects them, pings the radio, and re-registers on the d-plane port about once a second so the radio keeps forwarding to this manager. The first registration happens when the d-plane address is applied.

Without an `mplane` address, the manager never talks to the radio m-plane: set `radio_device fcs` explicitly (`SIGNAL` or `ACTUAL`) or use config `winject.radio_fcs`.

The manager m-plane also forwards radio commands for operators and bench tools (`radio_info`, `radio_tx`, `radio_stats`, `reset`, `radio_caps_info`); see [mplane.md](mplane.md). The bench scripts and tools reach the radios only through these.

## Radio (ESP32)

The radio firmware is deliberately simple. It knows nothing about slots, buses or FEC, only MPDUs and the domain filter.

| Stage | Behavior |
|---|---|
| Inject | The Ethernet RX callback takes IPv4/UDP frames to its own MAC/IP on port 9000 directly off the EMAC ring, bypassing lwIP. IP fragments are not reassembled: they fall through to lwIP, which answers with ICMP port unreachable. That's why the manager caps an MPDU at 1472 bytes |
| TX queue | Frames go into a bounded inject queue (`tx_queue_sz`), drained by one task into `esp_wifi_80211_tx` with at most 4 in flight. The task pauses after every 8 frames. Measured sustained rate at OFDM 24M: ~1,450–1,550 frames/s; above that the queue overflows (`dropped_tx_queue`) |
| RX | Promiscuous mode; frames whose Address 3 matches `rx_filter_addr3` are queued (`rx_queue_sz`), others are counted as `dropped_filter_mismatched` |
| Forward | A drain task sends each queued frame + trailer to the registered peer on the d-plane port. The ESP32 hardware does not deliver the received FCS, so it reports `fcs=SIGNAL`: trailer `00000000` = hardware CRC passed, `FFFFFFFF` = failed |
| Counters | `tx_info` / `rx_info` report monotonic counters for every stage (`ether_pkt`, `air_pkt`, `dropped_*`), so a host can attribute each lost frame to a stage |

The radio m-plane, d-plane and counters are specified in the winject-radio-esp32 repository.

## Domains and buses

- **Domain** (`winject.domain`, 16-bit hex, non-zero): carried in Address 3 as `CA:FE:BA:BE:<hi>:<lo>`. The radio filters on it in its RX callback, so frames of other domains never reach the manager. Use one domain per group of cooperating managers on a channel.
- **Bus** (`upstream-N.tx_bus` / `rx_bus`, 1–2 hex digits, non-zero, for example `b2`): carried in each slot's LC header. On TX, an upstream stamps its `tx_bus`. On RX, a slot is delivered to **every** upstream whose `rx_bus` matches. Bus 0 is not valid.
- **Pairing:** a flow from host A to host B needs A's upstream `tx_bus` equal to B's upstream `rx_bus`. A bidirectional pair uses two buses, for example A `tx_bus=b2 rx_bus=a1` and B `tx_bus=a1 rx_bus=b2`. The bench configs in `configuration/winject-tests/<scenario>/` use pairs `b2`/`a1` and `c3`/`d4`.
- Slot position in the MPDU has no meaning; only the LC bus routes a slot.

## Framing

### Layering (TX)

```
UDP application datagram                    (user SDU, ≤ 1445 bytes)
        |
        v  optional RsBlockErasure          (k-of-n Reed–Solomon shards)
air SDU (FEC shard or raw datagram)
        |
        v  LCHeader: [bus u8][is_fec 1b | seq 15b, BE] + air SDU
slot payload                                (per upstream)
        |
        v  Mpdu: up to 5 slots in one 802.11 DATA frame body
802.11 MPDU                                 (≤ 1472 bytes on inject)
        |
        v  UDP to radio:9000
radio (raw 802.11 TX)
```

RX reverses the chain: demux slots → route by LC bus → check LC seq → FEC decode (if the LC header marks a shard) → deliver UDP.

### 802.11 header

Winject uses a plain **non-QoS DATA** frame:

| Field | Role |
|---|---|
| Frame control | Type `DATA`, flags `0` (no To/From DS, no fragmentation, no retry) |
| Duration | `0` |
| Address 1–2 | **Packed slot lengths** (see § Slots). The first packed bit is always `1`, which is the group (multicast) bit of Address 1, so radios never expect an ACK |
| Address 3 | **Domain**: `CA:FE:BA:BE:<domain BE>` |
| Sequence control | 12-bit MPDU sequence (`next_tx_sequence()`), fragment `0` |
| Frame body | Non-empty slot payloads, back to back |
| FCS | Added by the transmitting radio's hardware. On RX the manager sees the 4-byte forward trailer, interpreted per `fcs=` mode by `WifiUdp`; MPDU buffers exclude it (`set_enable_fcs(false)`) |

Limits ([`RadioDefs.h`](../src/manager/radio/RadioDefs.h), [`NetUtil.h`](../src/manager/utils/NetUtil.h)):

| Limit | Value | Symbol |
|---|---|---|
| MPDU on inject (host → radio) | **1472** bytes: one IPv4/UDP datagram on a 1500-byte MTU | `WIFI_RADIO_INJECT_MAX` |
| MPDU on forward (radio → host) | **1500** bytes | `WIFI_RADIO_RX_MAX` |
| 802.11 header | **24** bytes | `WIFI_HDR_LEN` |
| TX frame body (all slots) | **1448** bytes | `WIFI_PAYLOAD_MAX`, `k_wifi_payload_max` |
| RX frame body | **1476** bytes | `WIFI_RX_PAYLOAD_MAX` |
| User datagram per slot | **1445** bytes (1448 − 3 LC header) | `k_stream_payload_max` |

On RX, [`Mpdu::rescan`](../src/manager/frames/Mpdu.cpp) accepts only DATA frames with zero flags whose declared slot sizes exactly fill the body.

### Slots

The frame body holds up to **5** (`WIFI_PDU_SLOTS`) slot payloads. Their lengths are bit-packed into Address 1 and Address 2 (12 bytes): 1 marker bit (always `1`), then 5 × 11 bits, least-significant bit first. Each length is 0–2047; `0` means the slot is empty. Non-empty slots are stored in order with no per-slot header beyond the LC header.

On TX the mux puts the primary upstream first, then other upstreams with data, within each upstream's scheduling share and the remaining body room, until it reaches five slots or runs out of space.

### LC header

Every slot payload starts with a 3-byte **logical-channel header** ([`LCHeader`](../src/manager/frames/LCHeader.h)):

```
 0               1               2
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+---------------+-+-----------------------------+------------------+
|    bus u8     |F|        seq (BE, 15 bit)     | air SDU          |
+---------------+-+-----------------------------+------------------+
```

- **bus**: the sending upstream's `tx_bus`; selects receiving upstreams by `rx_bus`.
- **F** (`is_fec`, MSB of the second byte): the air SDU is an FEC shard. This flag is the only thing that sends a payload to the FEC decoder; the receiver never looks inside the SDU to decide.
- **seq**: per-bus air sequence, 15 bits, incremented per slot payload sent on that bus and wrapping at 32768 ([`stamp_air_payload`](../src/manager/endpoint/UpstreamStats.cpp)).
- RX ([`accept_air_payload`](../src/manager/endpoint/UpstreamStats.cpp)) drops a payload with the same seq as the last accepted one (duplicate), counts gaps with 15-bit wrap-aware comparison (forward jumps below 16384; `air_rx_gap_loss`), and passes the bytes after the header on with the F flag.

Two sequence numbers exist: the 12-bit **MPDU sequence** in the 802.11 header (per radio, per frame) and the 15-bit **LC sequence** (per bus, per slot). Loss accounting uses the LC sequence.

### FEC (optional)

Configured per upstream: `upstream-N.fec.type = RS_BLOCK_ERASURE` with `fec.k`, `fec.n` and `fec.timeout_ms` (default 20), or at runtime with `add_upstream` / `update_upstream fec=BLOCK k= n= fec_timeout=` ([mplane.md](mplane.md)). [`RsBlockErasure`](../src/manager/fec/RsBlockErasure.h) groups **k** datagrams into **n** shards (`1 ≤ k < n ≤ 31`); any k received shards recover the block (Reed–Solomon erasure, ISA-L).

- **TX:** datagram → `push_app` → shards queued → `pull_tx` (tagged as FEC) → LC header with F set → slot. A partial block is closed after `fec.timeout_ms`.
- **RX:** a slot whose LC header has F set → `push_air` → zero or more recovered datagrams. Slots without F always pass straight to the application, whatever their bytes. The decoder reads k/n from each shard header, so peers can use different k/n, and it decodes F slots even on an upstream with FEC disabled; those are counted in `upstream_<id>_air_rx_fec_unexpected` because the two ends disagree on `fec.type`. A shard whose header is invalid is dropped and counted in the decode failures.

Shard header (5 bytes, [`RsBlockErasure::pack_header`](../src/manager/fec/RsBlockErasure.cpp)); the same layout as vstreamer's FEC shard header:

```
 0               1               2               3               4
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-------------------------------+---------+---------+---------+-+---------+-----+
|       sdu_base (BE u16)       |    k    |    n    |   idx   |s|  sdu_n  |spare|
+-------------------------------+---------+---------+---------+-+---------+-----+
```

| Field | Bits | Meaning |
|---|---|---|
| `sdu_base` | 16 | Datagram sequence of data shard 0; the block id. The TX sequence starts at a random value and advances by `sdu_n` per block, so a restarted peer does not reuse recent ids |
| `k`, `n` | 5 + 5 | Data and total shards |
| `idx` | 5 | Shard index; `idx ≥ k` is parity |
| `sdu_n` | 5 | Datagrams in the block (`1 ≤ sdu_n ≤ k`). A block closed by `fec.timeout_ms` has `sdu_n < k`: data slots `sdu_n … k−1` are empty pads that are not sent, and the receiver rebuilds them, so any `sdu_n` received shards recover the block |
| `s`, spare | 1 + 3 | Zero; a non-zero spare bit makes the header invalid |

Shard body: 2-byte BE length + original datagram (parity shards are padded for the RS arithmetic). The largest datagram an FEC upstream carries is `k_stream_payload_max − 5 − 2` = **1438** bytes ([`max_original()`](../src/manager/fec/RsBlockErasure.cpp)).

### Byte layout of one slot

```
with FEC:     [ LC bus 1 B ][ F=1 + LC seq 2 B ][ FEC hdr 5 B ][ len 2 B ][ datagram ... ]
without FEC:  [ LC bus 1 B ][ F=0 + LC seq 2 B ][ datagram ... ]
```

An MPDU is the 24-byte 802.11 header followed by up to five such slots.

## Related docs

- Manager configuration, console and metrics: [mplane.md](mplane.md)
- Code style and tests: [contributing.md](contributing.md)
