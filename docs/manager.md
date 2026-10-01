# Manager management plane protocol

**Host requirement:** Linux kernel **5.11+** (`epoll_pwait2`) for sub-millisecond reactor timers.

Commands accept a long name or a **shorthand** alias (`long\|short`). Examples: `list_upstream`, `lu`.

## Message Exchange

M-plane protocol is always client initiated.

```
client -> manager : request
client <- manager : response (optional)
```

## FEC

* Request fields: `fec=NONE\|BLOCK` (alias `RS_BLOCK_ERASURE`) with `k=` and `n=` when FEC is enabled.
* In responses and stats, FEC is one field: `fec=none` or `fec=block(<k>,<n>)` (e.g. `fec=block(10,16)`).

## Messages

| Request | Description | Response |
| -------- | ----------- | -------- |
| `help\|?\|h` | List this document. | (help text) |
| `ping\|p` | Query manager availability. | `pong` |
| `add_upstream\|au id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=<NONE\|BLOCK> k=<10> n=<16> fec_timeout=<50ms> quanta=<bytes>` | Add an upstream. | **OK:** `OK upstream id=<u8> txbus=<u8> rxbus=<u8> type=<UDP> rx=<address:port> tx=<interface:port> fec=none\|block(<k>,<n>) fec_timeout=<50ms> quanta=<bytes>`<br>**NOK:** `NOK <message>` — `NOT_FOUND`, `INVALID_ARGUMENT` |
| `list_upstream\|lu [ids=<u8>,...]` | List upstreams; optional id filter. | `N=<u8:count>`<br>`upstream id=... fec=none\|block(<k>,<n>) ...` (one line per upstream) |
| `update_upstream\|uu id=<u8> [fec=<NONE\|BLOCK> k=<n> n=<n> fec_timeout=<50ms> quanta=<bytes>]` | Update one or more upstream fields. | **OK:** same shape as `add_upstream` OK line<br>**NOK:** `NOK <message>` — `NOT_FOUND`, `INVALID_ARGUMENT` |
| `remove_upstream\|ru id=<u8>` | Remove an upstream. | **OK:** `OK`<br>**NOK:** `NOK <message>` — `NOT_FOUND` |
| `list_upstream_rx_stat\|lur [ids=<u8>,...]` | RX stats; optional id filter. | `N=<u8:count> T=<u64:epochus>`<br>`upstream_rx_stat id=<u8> rxbyt=<u64> rxpkt=<u64> rxgap=<u64> fec=none\|block(<k>,<n>) fec_rec=<u64> fec_lost=<u64> fec_rxbyt=<u64> fec_rxpkt=<u64> fec_rxgap=<u64>` (one line per upstream) |
| `list_upstream_tx_stat\|lut [ids=<u8>,...]` | TX stats including **queue (pending)** depth; optional id filter. `tx_pending_byt` / `tx_pending_pkt` are bytes and packets waiting in the upstream TX queue (not yet on the air). | `N=<u8:count> T=<u64:epochus>`<br>`upstream_tx_stat id=<u8> txbyt=<u64> txpkt=<u64> fec=none\|block(<k>,<n>) fec_txbyt=<u64> fec_txpkt=<u64> tx_pending_byt=<u64> tx_pending_pkt=<u64>` (one line per upstream) |
| `get_metrics\|gm [keys=<string>,...]` | Metrics snapshot; optional keys filter. | `N=<u16:count> T=<u64:epochus>`<br>`key=value` (one line per key) |
| `radio_info\|ri` | Forwards to radio [`radio_info`](radio.md); body is the radio reply. | `radio_tx channel=<u16> tx_power=<int> modulation=<string>`<br>`radio_rx rssi=<i8>` |
| `radio_tx\|rt [channel=<u16> tx_power=<int> modulation=<string>]` | Forwards to radio [`radio_tx`](radio.md); update one or more fields. | **OK:** `OK radio_tx channel=<u16> tx_power=<int> modulation=<string>`<br>**NOK:** `NOK <message>` — `INVALID_ARGUMENT` (wrong value) |
| `reset\|r id=<u8>` | Forwards to radio [`reset`](radio.md). | **OK:** `OK id=<u8>`<br>**NOK:** `NOK <message>` — `EALREADY` (id already processed) |
