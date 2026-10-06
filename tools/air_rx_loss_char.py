#!/usr/bin/env python3
"""Characterize manager air-RX loss (LC seq gaps) on one upstream.

Polls ``lur`` for ``rxpkt`` / ``rxgap`` and reports:
  - percentiles of per-sample loss rate (gap / (gap + delivered air slots))
  - histogram of loss-episode duration (wall time from first gap in a burst until RX resumes)

Loss rate uses manager ``rxgap`` (``air_rx_gap_loss``): missing LC sequence numbers on the
air leg before FEC / app delivery.
"""

from __future__ import annotations

import argparse
import math
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

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


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * (p / 100.0)
    f = int(k)
    c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)


def histogram_ms(durations: list[float], edges_ms: list[float]) -> list[tuple[str, int]]:
    """Half-open buckets [0,e0), [e0,e1), ... last is >= penultimate edge."""
    edges = sorted(edges_ms)
    if len(edges) < 2:
        raise ValueError("need at least two histogram edges")
    counts = [0] * len(edges)
    for d in durations:
        for i, edge in enumerate(edges):
            if d < edge:
                counts[i] += 1
                break
        else:
            counts[-1] += 1
    out: list[tuple[str, int]] = []
    lo = 0.0
    for i, edge in enumerate(edges):
        if i < len(edges) - 1:
            label = f"{lo:g}-{edge:g}ms"
            lo = edge
        else:
            label = f">={edges[-2]:g}ms"
        out.append((label, counts[i]))
    return out


def run_char(
    mgr: Endpoint,
    upstream_id: int,
    duration_s: float,
    sample_ms: float,
    min_slots: int,
    burst_loss_threshold: float,
) -> dict:
    period_s = sample_ms / 1000.0
    t_end = time.monotonic() + duration_s

    text = mgr_request(mgr, f"lur ids={upstream_id}")
    rxpkt, rxgap = parse_upstream_rx_stat(text, upstream_id)

    loss_rates: list[float] = []
    idle_samples = 0
    silence_durations_ms: list[float] = []
    burst_durations_ms: list[float] = []
    traffic_active = False
    silence_ms = 0.0
    in_burst = False
    burst_start = 0.0

    total_dg = 0
    total_dr = 0

    while time.monotonic() < t_end:
        t0 = time.monotonic()
        time.sleep(max(0.0, period_s - 0.001))
        # Sleep remainder
        while time.monotonic() - t0 < period_s:
            time.sleep(min(0.002, period_s - (time.monotonic() - t0)))

        now = time.monotonic()
        text = mgr_request(mgr, f"lur ids={upstream_id}", timeout=3.0)
        rxpkt_n, rxgap_n = parse_upstream_rx_stat(text, upstream_id)
        dr = max(0, rxpkt_n - rxpkt)
        dg = max(0, rxgap_n - rxgap)
        rxpkt, rxgap = rxpkt_n, rxgap_n
        total_dr += dr
        total_dg += dg

        slots = dr + dg
        if slots < min_slots:
            idle_samples += 1
            if in_burst:
                burst_durations_ms.append((now - burst_start) * 1000.0)
                in_burst = False
            continue

        rate = dg / slots if slots else 0.0
        loss_rates.append(rate)

        if rate >= burst_loss_threshold:
            if not in_burst:
                burst_start = now - period_s
                in_burst = True
        elif in_burst:
            burst_durations_ms.append((now - burst_start) * 1000.0)
            in_burst = False

        # Loss-duration episodes: contiguous RX silence while the link was active.
        if dr > 0 or dg > 0:
            if traffic_active and silence_ms > 0:
                silence_durations_ms.append(silence_ms)
            silence_ms = 0.0
            traffic_active = True
        elif traffic_active:
            silence_ms += sample_ms

    if traffic_active and silence_ms > 0:
        silence_durations_ms.append(silence_ms)
    if in_burst:
        burst_durations_ms.append((time.monotonic() - burst_start) * 1000.0)

    agg_rate = total_dg / (total_dg + total_dr) if (total_dg + total_dr) else float("nan")

    return {
        "loss_rates": loss_rates,
        "idle_samples": idle_samples,
        "silence_durations_ms": silence_durations_ms,
        "burst_durations_ms": burst_durations_ms,
        "burst_loss_threshold": burst_loss_threshold,
        "aggregate_loss": agg_rate,
        "total_dr": total_dr,
        "total_dg": total_dg,
    }


def print_report(
    label: str,
    mgr: Endpoint,
    upstream_id: int,
    duration_s: float,
    sample_ms: float,
    result: dict,
    hist_edges_ms: list[float],
) -> None:
    rates = result["loss_rates"]
    silence = result["silence_durations_ms"]
    bursts = result["burst_durations_ms"]
    burst_thr = result["burst_loss_threshold"]
    print(f"\n=== {label} ===")
    print(
        f"mgr={mgr[0]}:{mgr[1]} upstream={upstream_id} "
        f"duration={duration_s:.1f}s sample={sample_ms:.0f}ms "
        f"active_samples={len(rates)} idle_samples={result['idle_samples']}"
    )
    print(
        f"aggregate air-RX loss: {100.0 * result['aggregate_loss']:.4f}% "
        f"(gap={result['total_dg']} slots={result['total_dg'] + result['total_dr']})"
    )
    if rates:
        print(
            "loss rate percentiles (per-sample, %): "
            f"p50={100*percentile(rates,50):.3f} "
            f"p90={100*percentile(rates,90):.3f} "
            f"p95={100*percentile(rates,95):.3f} "
            f"p99={100*percentile(rates,99):.3f} "
            f"max={100*max(rates):.3f}"
        )
    else:
        print("loss rate percentiles: n/a (no active traffic samples)")

    print(f"RX silence episodes (no air slots in sample): n={len(silence)}")
    if silence:
        print(
            "silence duration (ms): "
            f"p50={percentile(silence,50):.1f} "
            f"p90={percentile(silence,90):.1f} "
            f"p95={percentile(silence,95):.1f} "
            f"p99={percentile(silence,99):.1f} "
            f"max={max(silence):.1f}"
        )
    print("silence duration histogram:")
    for bucket, count in histogram_ms(silence, hist_edges_ms):
        bar = "#" * min(count, 60)
        print(f"  {bucket:>16}  {count:5d}  {bar}")

    print(
        f"high-loss episodes (sample loss >= {100*burst_thr:.0f}%): n={len(bursts)}"
    )
    if bursts:
        print(
            "high-loss episode duration (ms): "
            f"p50={percentile(bursts,50):.1f} "
            f"p90={percentile(bursts,90):.1f} "
            f"p95={percentile(bursts,95):.1f} "
            f"p99={percentile(bursts,99):.1f} "
            f"max={max(bursts):.1f}"
        )
    print("high-loss episode duration histogram:")
    for bucket, count in histogram_ms(bursts, hist_edges_ms):
        bar = "#" * min(count, 60)
        print(f"  {bucket:>16}  {count:5d}  {bar}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--mgr-host", default="127.0.0.1")
    p.add_argument("--mgr-port", type=int, default=2424)
    p.add_argument("--upstream", type=int, default=1)
    p.add_argument("--duration", type=float, default=90.0)
    p.add_argument("--sample-ms", type=float, default=50.0)
    p.add_argument(
        "--min-slots",
        type=int,
        default=1,
        help="min delivered+gap slots per sample to count (skip idle)",
    )
    p.add_argument("--label", default="")
    p.add_argument(
        "--hist-ms",
        default="10,20,50,100,200,500,1000,2000,5000",
        help="histogram upper edges in ms (last bucket is >= penultimate)",
    )
    p.add_argument(
        "--burst-threshold",
        type=float,
        default=0.30,
        help="fractional per-sample loss to start a high-loss episode (0-1)",
    )
    args = p.parse_args()
    mgr: Endpoint = (args.mgr_host, args.mgr_port)
    edges = [float(x) for x in args.hist_ms.split(",") if x.strip()]
    if len(edges) < 2:
        raise SystemExit("--hist-ms needs at least two comma-separated edges")

    label = args.label or f"mgr {mgr[0]}:{mgr[1]} upstream {args.upstream}"
    result = run_char(
        mgr,
        args.upstream,
        args.duration,
        args.sample_ms,
        args.min_slots,
        args.burst_threshold,
    )
    print_report(
        label, mgr, args.upstream, args.duration, args.sample_ms, result, edges
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
