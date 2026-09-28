# Winject framing protocol

Host **winject-manager** bridges UDP applications and a winject radio. The radio data plane is opaque **full 802.11 MPDU** bytes over UDP (see [`WifiUdp`](../../src/manager/radio/WifiUdp.h)). Inside each MPDU, winject multiplexes several independent **logical channels** (one per configured upstream). Optional **FEC** sits between the user datagram and the logical-channel payload.

## Layering (TX)

```
UDP application datagram          (user SDU)
        |
        v  optional RsBlockErasure (k-of-n Reed–Solomon shards)
air SDU (FEC shard or raw UDP)
        |
        v  LCSequence: 2-byte big-endian seq + SDU
logical-channel payload (LCP body)  per upstream / MPDU slot
        |
        v  Mpdu: up to 5 slots in one 802.11 DATA frame body
802.11 MPDU
        |
        v  UDP inject port
ESP32 / NIC radio (on-air transmit)
```

RX reverses the chain: demux slots → strip LC seq → FEC decode (if present) → deliver UDP.

Implementation references: [`Frame.*`](../../src/manager/frames/Frame.*) (IEEE layout), [`Mpdu.*`](../../src/manager/frames/Mpdu.*) (winject slot/domain), [`LCSequence`](../../src/manager/frames/LCSequence.h), [`TxMux`](../../src/manager/radio/TxMux.cpp) / [`RxDemux`](../../src/manager/radio/RxDemux.cpp), [`RsBlockErasure`](../../src/manager/fec/RsBlockErasure.h).

## 802.11

Winject uses a plain **non-QoS DATA** frame:

| Field | Role |
|--------|------|
| Frame control | Type `DATA`, flags `0` (no To/From DS, no fragmentation flags in use) |
| Duration | `0` |
| Address 1–2 | **Packed slot lengths** (see MPDU); not used as real MAC destinations |
| Address 3 | **Winject domain** (BSSID-like tag); see below |
| Sequence control | 12-bit **MPDU** sequence + fragment number (`0`) assigned by the manager on TX |
| Frame body | Concatenation of all non-empty slot payloads |
| FCS | Not relied on in manager parsing (`set_enable_fcs(false)`) |

Limits ([`RadioDefs.h`](../../src/manager/radio/RadioDefs.h), [`NetUtil.h`](../../src/manager/utils/NetUtil.h)):

- Maximum MPDU size on the radio UDP wire: **1500** bytes (`WIFI_RADIO_INJECT_MAX`).
- 802.11 header: **24** bytes (`WIFI_HDR_LEN`).
- Maximum frame body (all slots combined): **1476** bytes (`k_wifi_payload_max` / `WIFI_PAYLOAD_MAX`).

The manager validates incoming frames as DATA with zero flags and checks that declared slot sizes exactly fill the frame body ([`Mpdu::rescan`](../../src/manager/frames/Mpdu.cpp)).

## MPDU (winject multiplexing)

An **MPDU** here means the full 802.11 MAC frame the manager builds or parses, with winject-specific use of addresses and body layout.

### Domain (Address 3)

Peers on the same RF channel are separated by a 16-bit **domain** configured as `winject.domain` (hex, non-zero). On the wire, Address 3 is:

- Bytes 0–3: fixed prefix `CA:FE:BA:BE` (`WIFI_BSSID_PREFIX`)
- Bytes 4–5: domain, big-endian

[`Mpdu::is_valid_winject_frame`](../../src/manager/frames/Mpdu.cpp) checks the prefix; [`get_domain` / `set_domain`](../../src/manager/frames/Mpdu.h) read/write the domain field. The manager programs the radio with the same domain via the radio console (`set_domain`).

### PDU slots (frame body)

The frame body holds up to **`WIFI_PDU_SLOTS` (5)** back-to-back payloads. Slot **i** corresponds to **upstream index `i`** in the manager config when that upstream is active on the bus ([`RxDemux`](../../src/manager/radio/RxDemux.cpp), [`TxMux::emit_mpdu`](../../src/manager/radio/TxMux.cpp)).

Slot **lengths** (0–2047 bytes each, 11 bits per slot) are bit-packed into **Address 1 and Address 2** (12 bytes total: 1 flag bit + 5×11 bits). Length `0` means the slot is empty. Non-empty slots are stored in order in the frame body with no per-slot headers.

On TX, the mux may place multiple upstreams into one MPDU (primary upstream first, then others with remaining budget and body room) until it hits five slots or runs out of space.

### MPDU sequence vs logical-channel sequence

- **Sequence control** (802.11 header): global per-radio TX counter (`next_tx_sequence()`, 12-bit), one value per emitted MPDU.
- **LCSequence** (inside each slot): per-upstream logical-channel sequence; see below.

## Logical channel (LCP)

Each upstream owns a **logical channel** on the shared air bus. Every payload placed in that upstream’s MPDU slot is a **logical-channel protocol (LCP) body**:

```
+----------+------------------+
| seq (BE) | payload          |
| 2 bytes  | len - 2 bytes    |
+----------+------------------+
```

- [`LCSequence`](../../src/manager/frames/LCSequence.h): big-endian `uint16` **air sequence** (`UpstreamStats::air_tx` on TX).
- After the prefix: the **air SDU** (user bytes or FEC shard).

TX: [`stamp_air_payload`](../../src/manager/endpoint/UpstreamStats.cpp) prepends the sequence and increments `air_tx`.

RX: [`accept_air_payload`](../../src/manager/endpoint/UpstreamStats.cpp):

- Rejects duplicates (same seq as last accepted).
- Tracks gaps for metrics (`air_rx_gap_loss`) using 16-bit wrap-aware “ahead” comparison.
- Returns a pointer to bytes **after** the 2-byte prefix for delivery to the endpoint.

Per-slot size budget: `k_stream_payload_max` = **1474** bytes of air SDU per slot (`1476 - 2` for the LC prefix).

Config **bus** IDs (`upstream-N.tx_bus` / `rx_bus`) select which radio path an upstream uses; they do not change the on-air layout—only which manager upstream index maps to which slot and radio instance.

## FEC and user SDU

### User SDU

The **user SDU** is the application payload—typically one **UDP datagram** received or sent by [`UdpEndpoint`](../../src/manager/endpoint/UdpEndpoint.cpp). Without FEC, that datagram (up to `k_stream_payload_max` bytes) is the air SDU after LC stamping.

### FEC (optional, UDP upstreams)

When `RS_BLOCK_ERASURE` is enabled (`set_upstream_fec` / config), [`RsBlockErasure`](../../src/manager/fec/RsBlockErasure.h) groups **k** user datagrams into **n** on-air shards (`1 <= k < n <= 255`). Any **k** received shards recover the block (Reed–Solomon erasure, ISA-L).

**TX:** UDP → `push_app` → shards queued → `pull_tx` → LC stamp → MPDU slot.

**RX:** slot payload after LC strip → `push_air` → zero or more recovered user datagrams sent to UDP. Datagrams without a valid FEC header **pass through** unchanged.

#### FEC shard layout (air SDU before LC stamp)

8-byte header ([`RsBlockErasure::pack_header`](../../src/manager/fec/RsBlockErasure.cpp)):

| Offset | Field |
|--------|--------|
| 0 | Magic `0xF1` |
| 1 | Version `2` |
| 2–3 | Block id (BE `uint16`) |
| 4 | Shard index |
| 5 | k |
| 6 | n |
| 7 | Flags (e.g. parity `0x01`) |

Systematic shard body: **2-byte BE length** + original UDP bytes (padded for RS math on parity shards). Max original UDP size per shard is reduced by header and length prefix ([`max_original()`](../../src/manager/fec/RsBlockErasure.cpp): `k_stream_payload_max - 8 - 2`).

FEC is configured on **TX encode**; **RX is always FEC-aware** and reads **k/n from each shard header**, so peers can use different encode settings as long as the wire format matches.

### End-to-end byte picture (one slot, with FEC)

```
[ 802.11 header 24 B ][ LC seq 2 B ][ FEC hdr 8 B ][ len 2 B ][ UDP payload ... ]
                       \____________ LCP body ____________/ \____ air SDU ______/
```

Without FEC, the LCP body is `[ LC seq 2 B ][ UDP payload ... ]`.

## Related docs

- Manager config and console: [`manager.md`](manager.md)
- Build and layout: root [`README.md`](../README.md)
