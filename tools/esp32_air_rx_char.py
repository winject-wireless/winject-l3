#!/usr/bin/env python3
"""Characterize air-RX LC gap loss on ESP32 without winject-manager.

Configures radios over m-plane (channel, domain filter only), injects MPDUs on
the TX radio UDP :9000, listens on the RX radio forward path UDP :9210, and
decodes slots locally (same LC sequence rules as the manager).

Does not start managers or touch rover streamer telemetry.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from air_rx_loss_char import histogram_ms, percentile, print_report  # noqa: E402
from winject_mpdu import (  # noqa: E402
    build_data_mpdu,
    domain_addr3,
    lc_write,
    parse_bus_hex,
    strip_forward_trailer,
)
from winject_mpdu import LcAirRxTracker  # noqa: E402

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


def sample_char(
    tracker: LcAirRxTracker,
    duration_s: float,
    sample_ms: float,
    burst_threshold: float,
) -> dict:
    period_s = sample_ms / 1000.0
    end = time.monotonic() + duration_s
    loss_rates: list[float] = []
    silence_durations_ms: list[float] = []
    burst_durations_ms: list[float] = []
    traffic_active = False
    silence_ms = 0.0
    in_burst = False
    burst_start = 0.0
    prev_rx, prev_gap = tracker.snapshot()
    total_dr = 0
    total_dg = 0
    idle_samples = 0
    last_accept_t: float | None = None
    accept_outages_ms: list[float] = []

    while time.monotonic() < end:
        t0 = time.monotonic()
        time.sleep(period_s)
        now = time.monotonic()
        rx, gap = tracker.snapshot()
        dr = max(0, rx - prev_rx)
        dg = max(0, gap - prev_gap)
        prev_rx, prev_gap = rx, gap
        total_dr += dr
        total_dg += dg
        if dr > 0:
            if last_accept_t is not None:
                gap_ms = (now - last_accept_t) * 1000.0
                if gap_ms >= 200.0:
                    accept_outages_ms.append(gap_ms)
            last_accept_t = now
        slots = dr + dg
        if slots < 1:
            idle_samples += 1
            if in_burst:
                burst_durations_ms.append((now - burst_start) * 1000.0)
                in_burst = False
            continue
        rate = dg / slots
        loss_rates.append(rate)
        if rate >= burst_threshold:
            if not in_burst:
                burst_start = now - period_s
                in_burst = True
        elif in_burst:
            burst_durations_ms.append((now - burst_start) * 1000.0)
            in_burst = False
        if dr > 0 or dg > 0:
            if traffic_active and silence_ms > 0:
                silence_durations_ms.append(silence_ms)
            silence_ms = 0.0
            traffic_active = True
        elif traffic_active:
            silence_ms += sample_ms
    if in_burst:
        burst_durations_ms.append((time.monotonic() - burst_start) * 1000.0)
    if traffic_active and silence_ms > 0:
        silence_durations_ms.append(silence_ms)
    agg = total_dg / (total_dg + total_dr) if (total_dg + total_dr) else float("nan")
    return {
        "loss_rates": loss_rates,
        "idle_samples": idle_samples,
        "silence_durations_ms": silence_durations_ms,
        "burst_durations_ms": burst_durations_ms,
        "burst_loss_threshold": burst_threshold,
        "aggregate_loss": agg,
        "total_dr": total_dr,
        "total_dg": total_dg,
        "accept_outages_ms": accept_outages_ms,
    }


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
    p.add_argument("--sample-ms", type=float, default=50.0)
    p.add_argument("--burst-threshold", type=float, default=0.30)
    p.add_argument(
        "--hist-ms",
        default="10,20,50,100,200,500,1000,2000,5000",
    )
    args = p.parse_args()
    domain = int(args.domain, 16)
    bus = parse_bus_hex(args.bus)
    edges = [float(x) for x in args.hist_ms.split(",") if x.strip()]

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
        result = sample_char(
            tracker,
            args.duration,
            args.sample_ms,
            args.burst_threshold,
        )
    finally:
        if injector is not None:
            injector.stop()
        listener.stop()

    if args.listen_only:
        label = (
            f"ESP32 listen {args.rx} ch={args.channel} domain=0x{domain:04X} "
            f"bus=0x{bus:02X} (direct d-plane, no manager)"
        )
    else:
        label = (
            f"ESP32 {args.tx}→{args.rx} ch={args.channel} domain=0x{domain:04X} "
            f"bus=0x{bus:02X} (direct d-plane, no manager)"
        )
    # Reuse report printer with burst_durations_ms key name expected by print_report
    result["burst_durations_ms"] = result.get("burst_durations_ms", [])
    wrapped = {
        **result,
        "silence_durations_ms": result["silence_durations_ms"],
        "burst_durations_ms": result["burst_durations_ms"],
    }
    print_report(
        label,
        (args.rx, FORWARD_PORT),
        0,
        args.duration,
        args.sample_ms,
        wrapped,
        edges,
    )
    outages = result.get("accept_outages_ms", [])
    print(f"bus slot accept outages (>=200ms without new LC slot): n={len(outages)}")
    if outages:
        print(
            "accept outage (ms): "
            f"p50={percentile(outages, 50):.1f} "
            f"p90={percentile(outages, 90):.1f} "
            f"max={max(outages):.1f}"
        )
        print("accept outage histogram:")
        for bucket, count in histogram_ms(outages, edges):
            bar = "#" * min(count, 60)
            print(f"  {bucket:>16}  {count:5d}  {bar}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
