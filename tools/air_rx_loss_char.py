#!/usr/bin/env python3
"""Characterize manager air-RX loss as **per LC gap events**.

Each gap event is one sequence jump on accept: ``missed_slots`` LC slots lost and
``recovery_ms`` wall time since the previous accepted slot.

Polls ``lur`` (``rxpkt`` / ``rxgap``). Use a short ``--poll-ms`` (default 2 ms) on
the link-test upstream so each poll usually sees at most one accept.
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from air_rx_gap_report import GapEvent, print_gap_event_report  # noqa: E402
from radio_stats import Endpoint, mgr_request  # noqa: E402


def parse_upstream_rx_stat(text: str, upstream_id: int) -> tuple[int, int]:
    prefix = f"upstream_rx_stat id={upstream_id} "
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        rxpkt = _grab_int(line, "rxpkt")
        rxgap = _grab_int(line, "rxgap")
        return rxpkt, rxgap
    raise ValueError(f"no upstream_rx_stat id={upstream_id} in reply: {text[:200]!r}")


def _grab_int(line: str, key: str) -> int:
    m = re.search(rf"(?:^|\s){re.escape(key)}=(\d+)", line)
    if not m:
        return 0
    return int(m.group(1))


def _missed_per_gap_event(dr: int, dg: int) -> list[int]:
    """Split poll-window (dr, dg) into per-accept gap events (sum == dg)."""
    if dr <= 0 or dg <= 0:
        return []
    if dr == 1:
        return [dg]
    n = min(dr, dg)
    base = dg // n
    rem = dg % n
    return [base + (1 if i < rem else 0) for i in range(n)]


def poll_gap_events(
    mgr: Endpoint,
    upstream_id: int,
    duration_s: float,
    poll_ms: float,
) -> tuple[list[GapEvent], int, int, int]:
    period_s = poll_ms / 1000.0
    t_end = time.monotonic() + duration_s

    text = mgr_request(mgr, f"lur ids={upstream_id}")
    rxpkt, rxgap = parse_upstream_rx_stat(text, upstream_id)

    events: list[GapEvent] = []
    last_slot_t: float | None = None
    idle_polls = 0
    total_dr = 0
    total_dg = 0

    while time.monotonic() < t_end:
        t0 = time.monotonic()
        while time.monotonic() - t0 < period_s:
            time.sleep(min(0.001, period_s))
        now = time.monotonic()

        text = mgr_request(mgr, f"lur ids={upstream_id}", timeout=3.0)
        rxpkt_n, rxgap_n = parse_upstream_rx_stat(text, upstream_id)
        dr = max(0, rxpkt_n - rxpkt)
        dg = max(0, rxgap_n - rxgap)
        rxpkt, rxgap = rxpkt_n, rxgap_n
        total_dr += dr
        total_dg += dg

        if dr == 0 and dg == 0:
            idle_polls += 1
            continue

        if dr > 0 and dg > 0:
            recovery_ms = 0.0
            if last_slot_t is not None:
                recovery_ms = (now - last_slot_t) * 1000.0
            missed_each = _missed_per_gap_event(dr, dg)
            coalesced = dr > 1 or len(missed_each) > 1
            for i, missed in enumerate(missed_each):
                events.append(
                    GapEvent(
                        recovery_ms=recovery_ms if i == 0 else 0.0,
                        missed_slots=missed,
                        coalesced=coalesced,
                    )
                )

        if dr > 0:
            last_slot_t = now

    return events, total_dr, total_dg, idle_polls


def lc_events_to_gap_events(lc_events) -> list[GapEvent]:
    return [
        GapEvent(recovery_ms=e.recovery_ms, missed_slots=e.missed_slots, coalesced=False)
        for e in lc_events
    ]


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mgr-host", default="127.0.0.1")
    p.add_argument("--mgr-port", type=int, default=2424)
    p.add_argument("--upstream", type=int, default=2)
    p.add_argument("--duration", type=float, default=90.0)
    p.add_argument("--poll-ms", type=float, default=2.0)
    p.add_argument("--label", default="")
    p.add_argument(
        "--hist-ms",
        default="1,2,5,10,20,50,100,200,500",
        help="histogram upper edges for recovery time (ms)",
    )
    args = p.parse_args()
    mgr: Endpoint = (args.mgr_host, args.mgr_port)
    edges = [float(x) for x in args.hist_ms.split(",") if x.strip()]
    if len(edges) < 2:
        raise SystemExit("--hist-ms needs at least two comma-separated edges")

    events, total_dr, total_dg, idle_polls = poll_gap_events(
        mgr, args.upstream, args.duration, args.poll_ms
    )
    total_slots = total_dr + total_dg
    agg = total_dg / total_slots if total_slots else float("nan")

    label = args.label or f"mgr {mgr[0]}:{mgr[1]} upstream {args.upstream}"
    extra = (
        f"poll={args.poll_ms:g}ms duration={args.duration:.1f}s "
        f"idle_polls={idle_polls}"
    )
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
