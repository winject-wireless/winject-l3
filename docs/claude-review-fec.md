# winject-l3 review: FEC items (discussion)

Split out of [`claude-review.md`](claude-review.md) so the FEC findings can be discussed together. All items here are **Open (discussion)**: nothing below is locked yet. Each item keeps its original finding and the plan that was proposed before triage.

**Coupling with R14 (locked):** each MPDU slot now starts with an `LCHeader` (`[u8 bus][BE u16 seq]`). The out-of-band FEC signal that R4 asks for could go into this header, for example as a flag bit or a type byte, instead of being sniffed from the payload. R8's epoch or seed change and R6's streaming change also touch the shard header, so decide R4, R6 and R8 together.

| ID | Sev | Finding |
|----|-----|---------|
| R4 | High | RX is "always FEC-aware", so non-FEC payloads whose first byte is `0xF1` get eaten |
| R6 | Medium | Received systematic shards are held until k shards arrive and dropped if fewer arrive |
| R7 | Medium | `update_upstream fec_timeout=` never reaches the encoder, and `timeout=0` stalls partial blocks forever |
| R8 | Medium | `block_id` restarts at 0 on every `init()`, and the peer drops new blocks as duplicates for about 1 s |
| R9 | Medium | The FEC TX shard queue is unbounded; the non-FEC queue is 1024 packets with drop-oldest |

---

## R4. Payload sniffing for FEC drops arbitrary non-FEC traffic

**Status: Open (discussion)**

### Finding

`UdpEndpoint::on_radio_rx` always calls `fec.push_air` (`UdpEndpoint.cpp:264-271`). `RsBlockErasure::push_air` treats any datagram whose first byte is `0xF1` as a shard (`RsBlockErasure.cpp:707`). Non-FEC user payloads starting with `0xF1` are therefore either:

- dropped and counted as `decode_fail`, when byte 1 isn't `2` or the header is invalid, or
- swallowed into a bogus `rx_block`, where they may later corrupt or block a real block id.

For random or encrypted payloads, that is about 1 in 256 packets lost with no visible error on the non-FEC upstream. The TX-side stats use the same sniff (`UdpEndpoint.cpp:510-515`, `:264`).

**Suggested fix (review):** signal FEC out of band from the payload. One option is a bit in the LC header (for example, steal the top bit of the 16-bit LC seq or add a 1-byte LC type). Another is a per-upstream negotiated mode with RX FEC-awareness bound to config. At minimum, make RX FEC-awareness follow the local `fec` config, as the docs already imply for TX.

### Original plan (not locked)

**Decision (proposed):** RX FEC-awareness follows the **local** FEC config. This needs no wire change. Both peers must enable FEC on paired upstreams. Update the docs to say so.

**Edits:**
- `src/manager/endpoint/UdpEndpoint.cpp`, `on_radio_rx`: if `!fec.enabled()`, send `pkt` straight to `dest` (count `app_rx_*`) and skip `fec.push_air`. Only count `fec_air_rx_*` when FEC is enabled.
- `docs/winject.md`: in the "FEC (optional, UDP upstreams)" section, replace "RX is always FEC-aware… peers can use different encode settings" with: "RX decodes FEC only when the local upstream has FEC enabled; enable FEC on both ends of a pair. k/n are still read from each shard header, so the k/n values may differ."
- `docs/mplane.md`: add the same one-liner under FEC.

**Test:** in `RsBlockErasureTest.cpp` or a new `UdpEndpointFecTest`, use a non-FEC `UdpEndpoint`. Feed `on_radio_rx` a payload starting with `0xF1 0x02 …` and assert a UDP socket at `dest` receives it byte-for-byte. Use client mode so `dest` is valid.

---

## R6. FEC delivery is gated on block completion

**Status: Open (discussion)**

### Finding

`RsBlockErasure::push_air` (`RsBlockErasure.cpp:755-764`) buffers every shard, systematic ones included, until `k` distinct shards have arrived, and only then decodes. Consequences:

- **Latency:** every packet waits for the rest of its block, on top of the TX-side wait for `k` app packets or `timeout_ms` (`push_app`, `:464-491`). With k=10 at video rates, that adds tens of ms per hop.
- **Loss amplification:** if fewer than `k` shards arrive (loss > n−k), the block is expired as `decode_fail` (`expire_rx`, `:629-650`). The systematic shards that *did* arrive intact are discarded with it, so a burst slightly above the parity budget loses the whole block instead of only the missing packets.

**Suggested fix (review):** deliver systematic shards immediately on arrival and remember which indices were delivered. When the block becomes decodable, emit only the recovered missing ones. On TX, systematic shards can likewise be emitted as each app datagram arrives, with parity sent at block close. Without the RS step the systematic body is just `[len][payload]`, so this is cheap.

### Original plan (not locked)

**RX** (`src/manager/fec/RsBlockErasure.h/.cpp`):
- Add `std::vector<bool> delivered;` (sized `k`) to `rx_block_s`.
- In `push_air`, after the duplicate-index check:
  - If `index < k` and the shard isn't a parity shard: parse `[len BE16][payload]` from the shard body. If valid and `len > 0`, append the payload to `*out` immediately and set `delivered[index] = true`. Store the frag as now.
- When `frags.size() >= k`: run `decode_block` and append to `*out` only payloads for indices with `!delivered[i]`.
  - Today `decode_block` returns a flat vector that skips zero-length rows. Add an overload or out-param that returns `std::vector<std::vector<uint8_t>>` indexed by systematic row (empty for len 0) so delivered rows can be filtered.
  - `recovered_` counts only rows that were actually recovered and newly emitted.
- Expiry: in `expire_rx`, increment `decode_fail_` only if some systematic index in `[0, k)` was neither delivered nor received.

**TX:**
- `push_app` immediately emits the systematic shard for this packet: header with the current `block_id`, `index = pending.size() - 1`, `k_`, `n_`, `flags = 0`, then body `[len][payload]`. The shard goes to `tx_shards_` (or `out`).
- `flush`/`encode_block` then emits only:
  - the **empty systematic shards** for indices `pending.size() .. k-1` (when flushing a short block on timeout), and
  - the **parity shards**.
- Refactor `encode_block` with a flag `bool emit_systematic`. Existing tests call it directly and expect all n shards, so keep the default as `true`. The streaming path uses `false`.

**Tests** (`src/test/RsBlockErasureTest.cpp`):
- Push 1 app packet with k=4, n=6. Assert `has_tx_shards()` is true before the 4th packet.
- RX receives systematic 0 and 1 only. Assert both payloads are delivered immediately, and that nothing is emitted twice after parity completes the block.
- Losing 3 of 6 with k=4 (unrecoverable): the delivered systematic payloads still came out.
- Round trip with random loss ≤ n−k: the output multiset equals the input.

**Docs:** update `docs/winject.md` FEC TX/RX bullets to describe the streaming behaviour.

---

## R7. `fec_timeout` updates don't reach the encoder, and `0` is a stall

**Status: Open (discussion)**

### Finding

- `UdpEndpoint::set_fec_timeout_ms` (`UdpEndpoint.cpp:401-413`) only updates `fec_timeout_ms_`. `RsBlockErasure::timeout_ms` (set in `init`, `RsBlockErasure.cpp:74`) still drives `deadline` in `push_app` and `flush_if_deadline`. After `uu id=0 fec_timeout=5ms`, the reactor timer fires at 5 ms, `flush_if_deadline` sees the deadline hasn't passed, re-arms, and spins every 5 ms until the *old* deadline. `list_upstream` meanwhile reports the new value. `rx_hold_ms` / `done_hold_ms` also stay on the old value.
- `timeout=0` is accepted by config (`Config.cpp:454-463`), the console and `init`. `arm_fec_timer` and `flush_if_deadline` both treat `<=0` as "never". A partial block then sits in `pending` until `k` more packets arrive, which may be never for request/response traffic.

**Suggested fix (review):** add `RsBlockErasure::set_timeout_ms()` and call it from `set_fec_timeout_ms` under `tx_mu_`. Reject `0`, or define it as "flush immediately" and implement that.

### Original plan (not locked)

**Edits:**
- `RsBlockErasure`:
  - Add `bool set_timeout_ms(int ms)`, which rejects `ms < 1`, sets `timeout_ms`, and if `deadline_set` re-bases `deadline = first_push_time + ms`. Store `first_push_` when setting the deadline in `push_app`.
  - `init()` rejects `timeout_ms < 1`.
- `UdpEndpoint::set_fec_timeout_ms`:
  - Reject `< 1`.
  - Under `tx_mu_`: set `fec_timeout_ms_`; if `fec.enabled()`, call `fec.set_timeout_ms`; then `cancel_fec_timer(); sync_fec_timer_after_push();`.
- `Config.cpp` (`fec.timeout_ms`): reject `< 1`. Also parse `fec.timeout_ms` even when FEC is off, so it's validated consistently.
- `ConsoleService.cpp` `add_upstream`/`update_upstream` parsing: reject `fec_timeout` < 1 with `INVALID_ARGUMENT`.

**Tests:**
- `set_timeout_ms` shortens the deadline: push 1 packet with timeout 1000, `set_timeout_ms(1)`, sleep 5 ms, call `flush_if_deadline()`, and assert shards exist.
- `ConfigTest`: `fec.timeout_ms = 0` is rejected.

---

## R8. `block_id` reset causes about 1 s of dropped blocks after reconfig

**Status: Open (discussion)**

### Finding

`RsBlockErasure::init` resets `block_id = 0` (`RsBlockErasure.cpp:82`). The peer's `done` set keeps recently completed IDs for `done_hold_ms() = max(50 × timeout, …)`, which is 1 s with the default 20 ms. Every `update_upstream fec=…/k=/n=` and every manager restart therefore has its first blocks silently dropped as duplicates (`push_air`, `:722-725`). The header comment acknowledges the restart case but not runtime reconfig.

**Suggested fix (review):** seed `block_id` randomly in `init()`, or add an epoch byte to the header (the flags byte has room) and clear `done` when the epoch changes.

### Original plan (not locked)

**Edit:** in `RsBlockErasure::init`, seed `block_id` from `std::random_device{}()` truncated to `uint16_t`.

**Tests:**
- Update any `RsBlockErasureTest` that assumes `block_id == 0`; read it back from the shard header instead.
- New test: two `init()` calls on the TX side, and the RX peer decodes the first block after re-init. Use seeded randomness, or pick a test-only seam: an optional `init(k, n, timeout, uint16_t first_block_id)` overload.

---

## R9. Unbounded and over-deep TX queues

**Status: Open (discussion)**

### Finding

- With FEC on, `on_app` → `push_app(…, nullptr)` → `flush` appends to `tx_shards_` (`RsBlockErasure.cpp:524-527`) with no limit. When the app outruns the air rate, memory and latency grow without bound.
- Without FEC, `txq` is capped at 1024 packets with drop-oldest (`UdpEndpoint.cpp:11`, `:158-169`). At 1474 B that's about 1.5 MB per upstream, or roughly 12 s of buffer at 1 Mbps. That's heavy bufferbloat for RTP/video, and nothing counts or reports the drops.

**Suggested fix (review):** cap both queues by bytes or time (e.g. 100–200 ms at the current `max_rate_kbps`), count drops, and expose the counts in `lut`.

**Triage note:** the non-FEC `txq` drop-oldest in `push_tx` is also the cause of the R11 race. R10/R11 (locked) remove that race by pulling during planning, so it no longer constrains R9's queue design.

### Original plan (not locked)

**Decision (proposed):** each upstream gets a fixed byte budget, `k_max_tx_queue_bytes = 256 * 1024`. Drop-oldest. Count the drops.

**Edits:**
- `UdpEndpoint`:
  - Replace the `k_max_udp_queue` packet cap with byte accounting: track `txq_bytes_`, and drop from the front while `txq_bytes_ + new > cap`.
  - Add counters `tx_drop_pkt_` and `tx_drop_byt_`.
- `RsBlockErasure`:
  - Same byte cap on `tx_shards_` plus `pending`, enforced in `push_app`/`flush`.
  - Expose `tx_dropped_pkt()` and `tx_dropped_byt()`, and add them in `UdpEndpoint` getters.
- `ManagerConsoleTypes.h` `ManagerUpstreamTxStatView`: add `tx_drop_pkt` and `tx_drop_byt`. Fill them in `App::console_list_upstream_tx_stat` and print them in `ConsoleService` `lut` (append the fields at the end of the line).
- `docs/mplane.md`: add the two fields to `list_upstream_tx_stat`.

**Tests:** push more than 256 KiB into a non-FEC endpoint's queue via `on_app` (from the R1 harness), or call `push_tx` through a test seam. Assert the bytes stay ≤ the cap and the drop counter is greater than 0.

---

## Test gaps (FEC)

- `RsBlockErasure`: a non-FEC payload starting with `0xF1` passes through (R4); systematic shards are delivered before block completion (R6); `init()` twice and then RX on the peer (R8).
