#!/usr/bin/env python3
"""Characterize air-RX LC gap loss on ESP32 without winject-manager.

Configures radios over m-plane (channel, domain filter only), injects MPDUs on
the TX radio UDP :9000, listens on the RX radio forward path UDP :9210, and
decodes slots locally (same LC sequence rules as the manager).

Reports **one row per LC gap event** (sequence jump on accept), not windowed
loss percentages.
"""

from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from air_rx_gap_report import GapEvent, print_gap_event_report  # noqa: E402
from air_rx_loss_char import lc_events_to_gap_events  # noqa: E402
from winject_mpdu import (  # noqa: E402
    LcAirRxTracker,
    build_data_mpdu,
    domain_addr3,
    lc_write,
    parse_bus_hex,
    strip_forward_trailer,
)

MPLANE_PORT = 22
INJECT_PORT = 9000
FORWARD_PORT = 9210


def mplane(ip: str, cmd: str, timeout: float = 2.0) -> str:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    sock.sendto(f"cmd:1 {cmd}\n".encode(), (ip, MPLANE_PORT))
    return sock.recv(4096).decode(errors="replace")


def configure_radio(
    ip: str,
    channel: int,
    domain: int,
    modulation: str,
    power: int,
    cca: bool,
    rx_filter: bool,
) -> None:
    mplane(
        ip,
        f"radio_tx channel={channel} tx_power={power} "
        f"modulation={modulation} cca={'true' if cca else 'false'}",
    )
    if rx_filter:
        mac = ":".join(f"{b:02x}" for b in domain_addr3(domain))
        mplane(ip, f"rx_filter_addr3 addr={mac}")


class ForwardListener:
    def __init__(self, rx_ip: str, tracker: LcAirRxTracker) -> None:
        self.tracker = tracker
        self._stop = threading.Event()
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.bind(("0.0.0.0", 0))
        self._sock.settimeout(0.2)
        self._rx = (rx_ip, FORWARD_PORT)
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sock.close()

    def _run(self) -> None:
        next_reg = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now >= next_reg:
                try:
                    self._sock.sendto(b"\x00", self._rx)
                except OSError:
                    pass
                next_reg = now + 1.0
            try:
                data, _src = self._sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                if self._stop.is_set():
                    break
                continue
            mpdu = strip_forward_trailer(data)
            self.tracker.on_mpdu(mpdu)


class Injector:
    def __init__(
        self,
        tx_ip: str,
        domain: int,
        bus: int,
        payload_size: int,
        kbps: float,
    ) -> None:
        self.dest = (tx_ip, INJECT_PORT)
        self.domain = domain
        self.bus = bus
        self.payload_size = max(1, payload_size)
        self.interval = (self.payload_size + 3) * 8.0 / (kbps * 1000.0)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._lc_seq = 0
        self._mpdu_seq = 0
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=2.0)
        self._sock.close()

    def _run(self) -> None:
        filler = b"W" * self.payload_size
        next_t = time.monotonic()
        while not self._stop.is_set():
            now = time.monotonic()
            if now < next_t:
                time.sleep(min(0.001, next_t - now))
                continue
            slot = lc_write(self.bus, self._lc_seq) + filler
            self._lc_seq = (self._lc_seq + 1) & 0x7FFF
            frame = build_data_mpdu(self.domain, [slot], self._mpdu_seq)
            self._mpdu_seq = (self._mpdu_seq + 1) & 0x0FFF
            try:
                self._sock.sendto(frame, self.dest)
            except OSError:
                pass
            next_t += self.interval
            if next_t < time.monotonic() - self.interval:
                next_t = time.monotonic()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tx", default="", help="TX radio IP (inject :9000); omit with --listen-only")
    p.add_argument("--rx", required=True, help="RX radio IP (forward :9210)")
    p.add_argument(
        "--listen-only",
        action="store_true",
        help="only configure RX and decode forwarded MPDUs (no inject)",
    )
    p.add_argument("--channel", type=int, default=1)
    p.add_argument("--domain", default="1234", help="hex domain, e.g. 1234 or B00B")
    p.add_argument("--bus", default="b2", help="LC bus hex byte for inject/filter")
    p.add_argument("--modulation", default="OFDM_24M")
    p.add_argument("--power", type=int, default=20)
    p.add_argument("--cca", action="store_true")
    p.add_argument("--kbps", type=float, default=12000.0)
    p.add_argument("--payload-size", type=int, default=400, help="air SDU bytes after LC")
    p.add_argument("--duration", type=float, default=90.0)
    p.add_argument(
        "--hist-ms",
        default="1,2,5,10,20,50,100,200,500,1000,2000,5000",
        help="histogram upper edges for recovery time (ms)",
    )
    args = p.parse_args()
    domain = int(args.domain, 16)
    bus = parse_bus_hex(args.bus)
    edges = [float(x) for x in args.hist_ms.split(",") if x.strip()]
    if len(edges) < 2:
        raise SystemExit("--hist-ms needs at least two comma-separated edges")

    if args.listen_only:
        if args.tx:
            print("warning: --tx ignored in --listen-only mode", file=sys.stderr)
    else:
        if not args.tx:
            p.error("--tx is required unless --listen-only")
        configure_radio(
            args.tx,
            args.channel,
            domain,
            args.modulation,
            args.power,
            args.cca,
            rx_filter=False,
        )
    configure_radio(
        args.rx,
        args.channel,
        domain,
        args.modulation,
        args.power,
        args.cca,
        rx_filter=True,
    )

    tracker = LcAirRxTracker(bus=bus, domain=domain)
    listener = ForwardListener(args.rx, tracker)
    injector = None
    if not args.listen_only:
        injector = Injector(args.tx, domain, bus, args.payload_size, args.kbps)
    listener.start()
    if injector is not None:
        injector.start()
    time.sleep(0.5)
    try:
        time.sleep(args.duration)
    finally:
        if injector is not None:
            injector.stop()
        listener.stop()

    events: list[GapEvent] = lc_events_to_gap_events(tracker.gap_events)
    total_dr, total_dg = tracker.snapshot()
    total_slots = total_dr + total_dg
    agg = total_dg / total_slots if total_slots else float("nan")

    if args.listen_only:
        label = (
            f"ESP32 listen {args.rx} ch={args.channel} domain=0x{domain:04X} "
            f"bus=0x{bus:02X} (LC gap events, d-plane)"
        )
    else:
        label = (
            f"ESP32 {args.tx}→{args.rx} ch={args.channel} domain=0x{domain:04X} "
            f"bus=0x{bus:02X} (LC gap events, d-plane)"
        )
    extra = f"duration={args.duration:.1f}s decode=per LC seq jump on accept"
    print_gap_event_report(
        label,
        events,
        agg,
        total_dg,
        total_slots,
        edges,
        extra=extra,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
