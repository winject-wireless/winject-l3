# Manager m-plane and configuration

winject-manager is controlled two ways: a **configuration file** read at startup, and the **manager m-plane**, a text console over UDP for changing upstreams and reading stats at runtime. How the manager, radios and air framing fit together is in [winject.md](winject.md).

## Message exchange

The manager m-plane is request/response and always client-initiated:

```
client -> manager : request  (UDP datagram to manager.console_in)
client <- manager : response (UDP datagram to the request's source address)
```

- The manager **binds** `manager.console_in`. Each reply is sent to the **source address and port** of the request. Clients use an ephemeral UDP socket: send to `console_in`, read the reply on the same socket.
- Optional request prefix `cmd:<u8> ` (0–255). Replies are tagged `OK:<id>` / `NOK:<id>` with the same id. Ids are scoped per sender; a retransmitted tagged request still in flight is ignored. Too many in-flight requests → `NOK EBUSY` (tagged when the request was).
- Untagged requests get untagged replies (one at a time per socket; no correlation).
- One command per datagram, at most 511 bytes. Empty lines and lines starting with `#` are ignored (no reply).
- Every command has a long name and a short alias (`list_upstream` / `lu`).
- Unknown commands reply `NOK ENOSYS`.
- There is no authentication. Bind `console_in` to loopback (or a trusted interface) only.

Bench configs use loopback `console_in` per manager, for example A `127.0.0.1:2400` and B `127.0.0.1:2410`.

### Reply conventions

| Reply | Meaning |
|---|---|
| `OK` / `OK <fields>` | Success |
| `NOK EINVAL` | Missing, unknown, repeated, empty or out-of-range argument |
| `NOK ENOENT` | Unknown upstream id |
| `NOK EEXIST` | `add_upstream` with an id already in use |
| `NOK ENODEV` | Radio command while no radio m-plane is configured or connected |
| `NOK ENODATA` | `radio_caps_info` while the FCS mode is not known yet |
| `NOK ENOSYS` | Unknown command |
| `NOK ECANCELED` | A forwarded radio command was dropped because the radio connection was reset (for example by `radio_device mplane=`) |
| `NOK ETIMEDOUT` | The radio did not answer a forwarded command in time |
| `NOK EIO` | Local failure (socket open or send) |
| `NOK EBUSY` | Too many requests in flight on the manager |
| `NOK EPROTO` | The radio's protocol version `X.Y` differs from the manager's, or the radio has no `version` command |
| `NOK <radio code>` | A forwarded radio command failed; the radio's code is passed through |

FEC appears as one field. Requests take `fec=NONE|BLOCK` (`RS_BLOCK_ERASURE` is accepted for `BLOCK`) plus `k=` and `n=`. Replies and stats show `fec=none` or `fec=block(<k>,<n>)`, for example `fec=block(10,16)`.

## Commands

### General

| Request | Description | Response |
|---|---|---|
| `help\|?\|h` | List the commands | One usage line per command |
| `ping\|p` | Check that the manager is alive | `pong` |
| `version\|ver` | Manager build and protocol version | `OK version ver=vX.Y.Z proto=X.Y` |

#### Version discovery (frozen)

A client must discover the peer's protocol before using other commands. The
exchange below is **frozen**: it does not change when `X.Y` is bumped. Only
additive `key=value` tokens may be added to the reply; clients ignore unknown
keys. See [versioning.md](versioning.md) for the release rule and compatibility
check.

| Frozen | Exact form |
|---|---|
| Transport | UDP/IPv4; one request per datagram; replies to the request's source address and port |
| Request | `version` (alias `ver`), no arguments, optionally prefixed `cmd:<u8> ` |
| Reply | starts with `OK version` (or `OK:<u8> version` when tagged), then `ver=vX.Y.Z` and `proto=X.Y` among space-separated tokens |
| Correlation | optional `cmd:<u8>` on the request; matching `OK:<u8>` / `NOK:<u8>` on replies |
| Unknown command | `NOK ENOSYS` (or `NOK:<u8> ENOSYS` when tagged); if `version` returns this, treat the peer as pre-`v1.0` |

Procedure for a new connection:

1. Send `version` (tagged or not) as the **first** request.
2. `OK version ...` → compare `proto=` with the client's own `X.Y`. Equal →
   compatible; unequal → do not use other commands (`NOK EPROTO` on radio
   passthrough when versions differ).
3. `NOK ENOSYS` → incompatible (no `version` command).
4. Timeout → retry; never assume compatible without a successful `version`.

### Upstreams

| Request | Description | Response |
|---|---|---|
| `add_upstream\|au id=<u8> txbus=<u8> rxbus=<u8> type=UDP rx=<address:port> tx=<address:port> fec=<NONE\|BLOCK> k=<n> n=<n> fec_timeout=<ms> quanta=<bytes>` | Add an upstream. `type` must be `UDP`. `rx` only = server mode (bind), `tx` only = client mode (send to), both = static | `OK upstream id=<u8> txbus=<u8> rxbus=<u8> type=UDP rx=<address:port> tx=<address:port> fec=none\|block(<k>,<n>) fec_timeout=<n>ms quanta=<bytes>` |
| `list_upstream\|lu [ids=<u8>,...]` | List upstreams, optionally only the given ids | `N=<count>`, then one `upstream id=...` line per upstream (same fields as above) |
| `update_upstream\|uu id=<u8> [fec=<NONE\|BLOCK>] [k=<n>] [n=<n>] [fec_timeout=<ms>] [quanta=<bytes>]` | Change FEC or scheduling of an existing upstream | Same as `add_upstream` |
| `remove_upstream\|ru id=<u8>` | Remove an upstream | `OK` |

`fec_timeout` accepts a number of milliseconds with an optional `ms` suffix (`50` or `50ms`). It closes a partial FEC block after that long. `quanta` is the upstream's scheduling share per TX pass, in bytes of SDU plus its 3-byte LC header (config key `scheduler_budget`). An upstream may always send its first SDU of a pass even when it is larger than `quanta`, so a small `quanta` slows an upstream but never blocks it.

### Statistics

| Request | Description | Response |
|---|---|---|
| `list_upstream_rx_stat\|lur [ids=<u8>,...]` | RX counters per upstream | `N=<count> T=<epoch µs>`, then per upstream: `upstream_rx_stat id=<u8> rxbyt=<u64> rxpkt=<u64> rx_oversize=<u64> rxgap=<u64> fec=... fec_rec=<u64> fec_lost=<u64> fec_rxbyt=<u64> fec_rxpkt=<u64> fec_rxgap=<u64>` |
| `list_upstream_tx_stat\|lut [ids=<u8>,...]` | TX counters per upstream, including what is still queued | `N=<count> T=<epoch µs>`, then per upstream: `upstream_tx_stat id=<u8> txbyt=<u64> txpkt=<u64> fec=... fec_txbyt=<u64> fec_txpkt=<u64> tx_pending_byt=<u64> tx_pending_pkt=<u64>` |
| `get_metrics\|gm [keys=<key>,...]` | Snapshot of manager and d-plane counters (§ `get_metrics` keys), optionally filtered by a comma-separated key list | `N=<count> T=<epoch µs>`, then one `key=value` line per metric |

- `rx_oversize`: application datagrams dropped because they exceed 1445 bytes (`k_stream_payload_max`).
- `rxgap`: air RX sequence gaps (lost slot payloads on this upstream's bus).
- `tx_pending_byt` / `tx_pending_pkt`: bytes and datagrams waiting in the upstream's TX queue, not yet sent to the radio.

### Radio

| Request | Description | Response |
|---|---|---|
| `radio_device\|rd id=<u8> [mplane=<ip:port>] [dplane=<ip:port>] [fcs=AUTO\|SIGNAL\|ACTUAL]` | Set or read the radio the manager drives. Only `id=0`. Keys left out keep their value; `id=0` alone reads. Addresses cannot be cleared | `OK radio_device id=0 mplane=<ip:port\|-> dplane=<ip:port\|-> fcs=<mode>` |

`mplane` selects the radio m-plane (omit or use `winject.skip_console = 1` in config to leave it unset). `dplane` is the radio's single UDP port for inject, registration and forward. `fcs` is the trailer mode (`AUTO` learns from the radio when the m-plane is up). The reply is sent once manager state is updated; it does not wait for the radio.

These are forwarded to the radio's m-plane when `radio_device` has an `mplane` address and the protocol versions match; otherwise they reply `NOK ENODEV` or `NOK EPROTO`. `radio_caps_info` is the exception (see below).

| Request | Description | Response |
|---|---|---|
| `radio_info\|ri` | Current PHY and last RSSI (radio `radio_tx_info`) | The radio's reply, for example `radio_tx channel=1 tx_power=20 modulation=OFDM_24M cca=false` and `radio_rx rssi=-10` |
| `radio_stats\|rs` | Radio d-plane counters (radio `tx_info` and `rx_info`) | Two lines, for example `tx_info tx_queue_sz=0 in_flight=0 dropped_invalid_frame=0 dropped_tx_queue=0 dropped_wifi=0 ether_pkt=100 air_pkt=100 ts=<µs>` and `rx_info rx_queue_sz=0 dropped_filter_mismatched=0 dropped_rx_queue=0 dropped_no_peer=0 dropped_send_failed=0 ether_pkt=100 air_pkt=100 ts=<µs>` |
| `radio_tx\|rt [channel=<1-14>] [tx_power=<dBm>] [modulation=<name>] [cca=<0\|1>]` | Change one or more PHY fields (radio `radio_tx`); the manager keeps them for its PHY reconcile | `OK radio_tx channel=... tx_power=... modulation=... cca=...` |
| `radio_caps_info\|rci` | FCS trailer mode the manager applies to forwarded frames. Asks the radio when its console is up (and updates the manager); otherwise answers from the configured or last-learned mode | `OK radio_caps_info fcs=SIGNAL\|ACTUAL`, or `NOK ENODATA` if the mode is not known yet |
| `reset\|r` | Restart the radio (not idempotent) | `OK` |
| `config slot=<u8>` | Select the radio settings slot the manager saves to (0 = scratch pad until set). No alias | `OK slot=<u8>` |

`tools/radio_stats.py` reads `radio_stats` and `get_metrics` from both managers and combines the radio counters with the manager's `radio_tx_pkt` / `radio_rx_pkt` for the Ethernet legs.

## `get_metrics` keys

Counters are monotonic for the lifetime of the manager process. Diff two snapshots for rates or loss.

| Key | Meaning |
|---|---|
| `radio_tx_pkt`, `radio_tx_byt` | MPDUs / bytes sent to the radio's inject port (host → radio) |
| `radio_rx_pkt`, `radio_rx_byt` | Datagrams / bytes received from the radio's forward path (radio → host), before the trailer check |
| `radio_fcs_mode` | FCS trailer mode in use: `0` unknown, `1` SIGNAL, `2` ACTUAL |
| `radio_fcs_err_pkt` | Forwarded frames whose trailer check failed (dropped) |
| `radio_fcs_unknown_pkt` | Forwarded frames dropped because the FCS mode was not known yet |
| `radio_proto_ok` | `1` when the connected radio's protocol `X.Y` matches the manager; `0` while unknown or mismatched |
| `radio_version` | Radio `vX.Y.Z` from `version`, or `-` while unknown |
| `tx_send_fail_mpdu`, `tx_send_fail_byt` | MPDUs the manager could not send to the radio (for example over 1472 bytes, or a socket error) |
| `tx_pacing_txtime_us`, `tx_pacing_gap_us` | TX pacing in use: modelled airtime of a full frame, and the gap between frames |
| `rx_drop_domain` | Forwarded frames that are not winject frames of this domain |
| `rx_drop_bus` | Slots whose LC bus matches no upstream `rx_bus` |
| `upstream_<id>_air_rx_gap_loss` | Air RX sequence gaps on upstream `<id>` |
| `upstream_<id>_app_rx_oversize_pkt` | Application datagrams over 1445 bytes dropped on upstream `<id>` |
| `upstream_<id>_air_rx_fec_unexpected` | FEC shards (LC header F flag) received on upstream `<id>` while its FEC is disabled; still decoded. Non-zero means the two ends disagree on `fec.type` |

Example (replies arrive on the same UDP socket you send from):

```text
get_metrics keys=radio_tx_pkt,radio_rx_pkt,radio_proto_ok,radio_version
```

## Configuration file

Run the manager with `winject-manager <config file>`. The file holds `key = value` lines; `#` starts a comment. Examples are in `configuration/` (`host_x86/`, `host_arm/`, and the bench pairs in `winject-tests/esp32/` and `winject-tests/realtek/`).

### Radio and manager (`winject.*`)

| Key | Default | Meaning |
|---|---|---|
| `winject.device` | | Optional; with `console` and `dplane_port` sets `radio_device id=0` at startup |
| `winject.console` | `22` | Radio m-plane UDP port (ESP32 `22`, Realtek `2201`) |
| `winject.dplane_port` | `9000` | Radio d-plane port (`winject.inject_port` accepted as an alias) |
| `winject.channel` | `1` | 1–14; channel 14 allows only DSSS/CCK modulations |
| `winject.modulation` | `DSS_1M_L` | PHY rate name, for example `OFDM_24M` |
| `winject.power` | `20` | TX power, dBm |
| `winject.cca` | | `0` or `1`: radio CCA, programmed with the PHY. Omitted = the radio keeps its current setting |
| `winject.domain` | required | 16-bit hex domain, non-zero; carried in Address 3 (see [winject.md](winject.md)) |
| `winject.skip_console` | `0` | `1`: do not set `radio_device mplane` from the config |
| `winject.radio_fcs` | `auto` | Initial `radio_device fcs` (`auto`, `signal`, `actual`) |
| `winject.max_rate_kbps` | `0` | Cap on the TX rate on top of airtime pacing; `0` = no cap |
| `winject.tx_gap_us` | `0` | Gap between frames in µs; `0` = derived from the modulation |
| `winject.max_data_per_tick` | `4` | Data MPDUs per 250 µs scheduler tick (1–32) |
| `winject.tx_burst_size` | `20` | MPDUs per burst (the radio's inject queue holds 20) |
| `winject.tx_burst_interval_us` | `0` | Pause after a burst; `0` = no pause |
| `winject.stats_sec` | `0` | Log stream stats every N seconds; `0` = off. Overridden by the `WINJECT_STATS_SEC` environment variable |

### Manager console (`manager.*`)

| Key | Meaning |
|---|---|
| `manager.console_in` | `address:port` the manager binds for commands (replies go to the sender) |

If unset, the manager m-plane is off.

### Upstreams (`upstream.*`, `upstream-N.*`)

`upstream.size = <count>`, then for each `N` from 0:

| Key | Meaning |
|---|---|
| `upstream-N.mode` | `UDP_SERVER_FORWARDING`, `UDP_CLIENT_FORWARDING` or `UDP_STATIC_FORWARDING` (`UDP_GENERIC_FORWARDING` is an alias for static). Must match the addresses given |
| `upstream-N.bind_address` (or `rx`) | `address:port` to receive application datagrams on. Server mode replies to the last sender |
| `upstream-N.connect_address` (or `tx`) | `address:port` to send received air datagrams to. Client mode |
| `upstream-N.tx_bus` | Bus stamped on this upstream's air traffic: 1–2 hex digits, non-zero (for example `b2`) |
| `upstream-N.rx_bus` | Bus whose air traffic is delivered to this upstream. At least one of `tx_bus` / `rx_bus` is required |
| `upstream-N.scheduler_budget` | Scheduling share per TX pass, bytes (default 256; console `quanta`) |
| `upstream-N.fec.type` | `NONE` or `RS_BLOCK_ERASURE` |
| `upstream-N.fec.k`, `upstream-N.fec.n` | Data and total shards per block (`1 ≤ k < n ≤ 31`; 5-bit fields in the shard header) |
| `upstream-N.fec.timeout_ms` | Close a partial block after this long (default 20) |

Server mode has `bind_address` only, client mode `connect_address` only, static mode both. A flow between two managers pairs one side's `tx_bus` with the other side's `rx_bus` (see [winject.md](winject.md) § Domains and buses).

## Related docs

- Architecture, data paths and on-air framing: [winject.md](winject.md)
- Code style and tests: [contributing.md](contributing.md)
