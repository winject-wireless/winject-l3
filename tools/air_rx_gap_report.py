"""Per LC gap-event statistics (one manager gap == one seq jump on accept)."""

from __future__ import annotations

from dataclasses import dataclass


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


@dataclass
class GapEvent:
    """One air-RX sequence gap (missing LC slots before an accepted slot)."""

    recovery_ms: float
    missed_slots: int
    coalesced: bool = False

    @property
    def event_loss_rate(self) -> float:
        """Loss share for this single accept: missed / (missed + 1 delivered)."""
        m = self.missed_slots
        if m <= 0:
            return 0.0
        return m / (m + 1.0)


def print_gap_event_report(
    label: str,
    events: list[GapEvent],
    aggregate_loss: float,
    total_missed: int,
    total_slots: int,
    hist_edges_ms: list[float],
    extra: str = "",
) -> None:
    print(f"\n=== {label} ===")
    if extra:
        print(extra)
    print(
        f"aggregate air-RX loss: {100.0 * aggregate_loss:.4f}% "
        f"(missed={total_missed} slots={total_slots})"
    )
    print(f"gap events: n={len(events)}")
    if not events:
        print("(no gap events recorded)")
        return

    rates = [e.event_loss_rate for e in events]
    recovery = [e.recovery_ms for e in events]
    missed = [float(e.missed_slots) for e in events]
    coalesced = sum(1 for e in events if e.coalesced)

    print(
        "per-event loss rate (missed/(missed+1)) %: "
        f"p50={100*percentile(rates,50):.3f} "
        f"p90={100*percentile(rates,90):.3f} "
        f"p95={100*percentile(rates,95):.3f} "
        f"p99={100*percentile(rates,99):.3f} "
        f"max={100*max(rates):.3f}"
    )
    print(
        "recovery time (ms since previous accepted slot): "
        f"p50={percentile(recovery,50):.1f} "
        f"p90={percentile(recovery,90):.1f} "
        f"p95={percentile(recovery,95):.1f} "
        f"p99={percentile(recovery,99):.1f} "
        f"max={max(recovery):.1f}"
    )
    if coalesced:
        print(
            f"note: {coalesced} events coalesced from multi-slot poll windows "
            f"(use LC decode for exact per-gap timing)"
        )
    print("recovery time histogram (ms):")
    for bucket, count in histogram_ms(recovery, hist_edges_ms):
        bar = "#" * min(count, 60)
        print(f"  {bucket:>16}  {count:5d}  {bar}")
    print("missed slots per gap event:")
    miss_edges = [1, 2, 3, 5, 10, 20, 50, 100, 500, 1000, 5000]
    for bucket, count in _histogram_int([e.missed_slots for e in events], miss_edges):
        bar = "#" * min(count, 60)
        print(f"  {bucket:>16}  {count:5d}  {bar}")


def _histogram_int(values: list[int], edges: list[int]) -> list[tuple[str, int]]:
    edges = sorted(edges)
    counts = [0] * len(edges)
    for v in values:
        for i, edge in enumerate(edges):
            if v < edge:
                counts[i] += 1
                break
        else:
            counts[-1] += 1
    out: list[tuple[str, int]] = []
    lo = 0
    for i, edge in enumerate(edges):
        if i < len(edges) - 1:
            label = f"{lo}-{edge - 1}" if edge > lo else f"<{edge}"
            lo = edge
        else:
            label = f">={edges[-2]}"
        out.append((label, counts[i]))
    return out
