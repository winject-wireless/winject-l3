"""Minimal winject 802.11 MPDU helpers for host-side inject and RX decode."""

from __future__ import annotations

import struct
from typing import Iterator

WIFI_HDR_LEN = 24
WIFI_PDU_SLOTS = 5
ADDR3_PREFIX = bytes([0xCA, 0xFE, 0xBA, 0xBE])
LC_LEN = 3
SEQ_MASK = 0x7FFF
SEQ_HALF = 0x4000
FC_TYPE_DATA = 0x08  # Data frame, subtype 0


def domain_addr3(domain: int) -> bytes:
    if domain == 0 or domain > 0xFFFF:
        raise ValueError("domain must be 1..0xffff")
    return ADDR3_PREFIX + bytes([(domain >> 8) & 0xFF, domain & 0xFF])


def parse_bus_hex(text: str) -> int:
    t = text.strip().lower()
    if t.startswith("0x"):
        t = t[2:]
    return int(t, 16) & 0xFF


def _emit_bits(packed: bytearray, bit: int, value: int, nbits: int) -> int:
    for i in range(nbits):
        if (value >> i) & 1:
            packed[(bit + i) // 8] |= 1 << ((bit + i) % 8)
    return bit + nbits


def _take_bits(packed: bytes, bit: int, nbits: int) -> tuple[int, int]:
    value = 0
    for i in range(nbits):
        if (packed[(bit + i) // 8] >> ((bit + i) % 8)) & 1:
            value |= 1 << i
    return value, bit + nbits


def unpack_slot_sizes(addr1: bytes, addr2: bytes) -> list[int]:
    packed = addr1 + addr2
    bit = 0
    _, bit = _take_bits(packed, bit, 1)
    sizes: list[int] = []
    for _ in range(WIFI_PDU_SLOTS):
        sz, bit = _take_bits(packed, bit, 11)
        sizes.append(sz)
    return sizes


def lc_write(bus: int, seq: int, is_fec: bool = False) -> bytes:
    word = (seq & SEQ_MASK) | (0x8000 if is_fec else 0)
    return bytes([bus & 0xFF]) + struct.pack(">H", word)


def build_data_mpdu(
    domain: int,
    slot_payloads: list[bytes],
    mpdu_seq: int,
) -> bytes:
    if not slot_payloads or len(slot_payloads) > WIFI_PDU_SLOTS:
        raise ValueError("need 1..5 slot payloads")
    body = b"".join(slot_payloads)
    sizes = [len(p) for p in slot_payloads] + [0] * (WIFI_PDU_SLOTS - len(slot_payloads))
    packed = bytearray(12)
    bit = _emit_bits(packed, 0, 1, 1)
    for sz in sizes:
        bit = _emit_bits(packed, bit, sz & 0x7FF, 11)
    addr1 = bytes(packed[:6])
    addr2 = bytes(packed[6:12])
    fc = struct.pack("<H", FC_TYPE_DATA)
    duration = struct.pack("<H", 0)
    addr3 = domain_addr3(domain)
    seq_ctl = struct.pack("<H", (mpdu_seq & 0x0FFF) << 4)
    return fc + duration + addr1 + addr2 + addr3 + seq_ctl + body


def strip_forward_trailer(datagram: bytes) -> bytes:
    """ESP32 forward datagram is MPDU + 4-byte trailer."""
    if len(datagram) <= 4:
        return datagram
    return datagram[:-4]


def iter_slot_payloads(mpdu: bytes) -> Iterator[bytes]:
    if len(mpdu) < WIFI_HDR_LEN:
        return
    fc = struct.unpack("<H", mpdu[0:2])[0]
    # DATA type, subtype 0, no flags (matches manager Mpdu::validate_data_frame).
    if fc != FC_TYPE_DATA:
        return
    addr1, addr2, addr3 = mpdu[4:10], mpdu[10:16], mpdu[16:22]
    if addr3[:4] != ADDR3_PREFIX:
        return
    sizes = unpack_slot_sizes(addr1, addr2)
    body = mpdu[WIFI_HDR_LEN:]
    off = 0
    out: list[bytes] = []
    for sz in sizes:
        if sz == 0:
            continue
        if sz > 2047 or off + sz > len(body):
            return
        out.append(body[off : off + sz])
        off += sz
    if off != len(body):
        return
    for slot in out:
        yield slot


class LcAirRxTracker:
    """Same gap semantics as manager UpstreamStats::note_air_rx_seq."""

    def __init__(self, bus: int | None = None, domain: int | None = None) -> None:
        self.bus = bus
        self.domain = domain
        self.have = False
        self.last_seq = 0
        self.gap_loss = 0
        self.rx_slots = 0

    def accept_slot(self, payload: bytes) -> bool:
        if len(payload) < LC_LEN:
            return False
        bus = payload[0]
        if self.bus is not None and bus != self.bus:
            return False
        seq = struct.unpack(">H", payload[1:3])[0] & SEQ_MASK
        if self.have and seq == self.last_seq:
            return False
        if not self.have:
            self.have = True
            self.last_seq = seq
            self.rx_slots += 1
            return True
        expected = (self.last_seq + 1) & SEQ_MASK
        ahead = (seq - expected) & SEQ_MASK
        if ahead < SEQ_HALF:
            self.gap_loss += ahead
            self.last_seq = seq
            self.rx_slots += 1
            return True
        return False

    def on_mpdu(self, mpdu: bytes) -> int:
        if self.domain is not None and len(mpdu) >= 22:
            addr3 = mpdu[16:22]
            if addr3[:4] != ADDR3_PREFIX:
                return 0
            dom = (addr3[4] << 8) | addr3[5]
            if dom != self.domain:
                return 0
        n = 0
        for slot in iter_slot_payloads(mpdu):
            if self.accept_slot(slot):
                n += 1
        return n

    def snapshot(self) -> tuple[int, int]:
        return self.rx_slots, self.gap_loss
