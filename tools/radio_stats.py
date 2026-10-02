#!/usr/bin/env python3
"""Read winject radio tx_info / rx_info counters and build stage accounting tables.

The CLI reads the radios through each winject-manager's m-plane (`radio_stats`,
`get_metrics`); it never talks to a radio directly.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import sys
from pathlib import Path
from typing import Any

# Bench manager m-plane endpoints: (console_out to bind, console_in to send to).
Endpoint = tuple[str, int]
MGR_A: tuple[Endpoint, Endpoint] = (("127.0.0.1", 2401), ("127.0.0.1", 2400))
MGR_B: tuple[Endpoint, Endpoint] = (("127.0.0.1", 2411), ("127.0.0.1", 2410))
U32_MOD = 2**32

TX_COUNTERS = (
    "dropped_invalid_frame",
    "dropped_tx_queue",
    "dropped_wifi",
    "ether_pkt",
    "air_pkt",
    "ts",
)
RX_COUNTERS = (
    "dropped_filter_mismatched",
    "dropped_rx_queue",
    "dropped_no_peer",
    "dropped_send_failed",
    "ether_pkt",
    "air_pkt",
    "ts",
)
TX_OCCUPANCY = ("tx_queue_sz", "in_flight")
RX_OCCUPANCY = ("rx_queue_sz",)

REQUIRED_TX_FOR_METRICS = ("ether_pkt", "air_pkt")
REQUIRED_RX_FOR_METRICS = ("ether_pkt", "air_pkt", "dropped_filter_mismatched")


def parse_kv_line(text: str) -> dict[str, int]:
    """Parse a single-line m-plane reply (first token is the command name)."""
    line = text.strip().splitlines()[0] if text.strip() else ""
    out: dict[str, int] = {}
    for m in re.finditer(r"(?:^|\s)([a-z_]+)=(-?\d+)", line):
        out[m.group(1)] = int(m.group(2))
    return out


def firmware_has_metrics(snap: dict[str, Any]) -> bool:
    tx = snap.get("tx", {})
    rx = snap.get("rx", {})
    return all(k in tx for k in REQUIRED_TX_FOR_METRICS) and all(
        k in rx for k in REQUIRED_RX_FOR_METRICS
    )


def _section_delta(
    before: dict[str, int],
    after: dict[str, int],
    counter_keys: tuple[str, ...],
    occupancy_keys: tuple[str, ...],
) -> tuple[dict[str, int], bool]:
    rebooted = False
    if "ts" in before and "ts" in after and after["ts"] < before["ts"]:
        rebooted = True
    out: dict[str, int] = {}
    for key in counter_keys:
        if key not in before or key not in after:
            continue
        if key == "ts":
            if rebooted:
                out["ts_delta_us"] = 0
            else:
                out["ts_delta_us"] = after["ts"] - before["ts"]
            continue
        out[key] = (after[key] - before[key]) % U32_MOD
    for key in occupancy_keys:
        if key in after:
            out[key] = after[key]
    return out, rebooted


def delta(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    """Per-counter u32 wrap deltas; occupancy from ``after``; ``rebooted`` if ts went backwards."""
    tx_d, rb_tx = _section_delta(
        before.get("tx", {}),
        after.get("tx", {}),
        TX_COUNTERS,
        TX_OCCUPANCY,
    )
    rx_d, rb_rx = _section_delta(
        before.get("rx", {}),
        after.get("rx", {}),
        RX_COUNTERS,
        RX_OCCUPANCY,
    )
    rebooted = rb_tx or rb_rx
    ts_us = tx_d.get("ts_delta_us")
    if ts_us is None:
        ts_us = rx_d.get("ts_delta_us", 0)
    return {
        "rebooted": rebooted,
        "ts_delta_us": ts_us,
        "tx": tx_d,
        "rx": rx_d,
    }


def _d(section: dict[str, int], key: str) -> int:
    return int(section.get(key, 0))


def stages(
    tx_delta_s: dict[str, Any],
    rx_delta_r: dict[str, Any],
    mgr_s: dict[str, int],
    mgr_r: dict[str, int],
) -> list[tuple[str, int, int]]:
    """Accounting rows: (name, left, right); left − right = loss at that stage."""
    tx = tx_delta_s.get("tx", {})
    rx = rx_delta_r.get("rx", {})
    r_domain_air = _d(rx, "air_pkt") - _d(rx, "dropped_filter_mismatched")
    rows: list[tuple[str, int, int]] = [
        ("mgr→S (Ethernet)", _d(mgr_s, "radio_tx_pkt"), _d(tx, "ether_pkt")),
        ("S inject path", _d(tx, "ether_pkt"), _d(tx, "air_pkt")),
        ("air", _d(tx, "air_pkt"), r_domain_air),
        ("R forward path", r_domain_air, _d(rx, "ether_pkt")),
        ("R→mgr (Ethernet)", _d(rx, "ether_pkt"), _d(mgr_r, "radio_rx_pkt")),
    ]
    return rows


def residual_checks(
    tx_delta_s: dict[str, Any],
    rx_delta_r: dict[str, Any],
) -> list[tuple[str, int]]:
    """Return (label, residual) where 0 ≈ balanced."""
    tx = tx_delta_s.get("tx", {})
    rx = rx_delta_r.get("rx", {})
    tx_q = _d(tx, "tx_queue_sz") + _d(tx, "in_flight")
    inject_residual = (
        _d(tx, "ether_pkt")
        - _d(tx, "air_pkt")
        - (
            _d(tx, "dropped_invalid_frame")
            + _d(tx, "dropped_tx_queue")
            + _d(tx, "dropped_wifi")
        )
    )
    rx_q = _d(rx, "rx_queue_sz") + 1
    rx_residual = (
        _d(rx, "air_pkt")
        - (
            _d(rx, "dropped_filter_mismatched")
            + _d(rx, "dropped_rx_queue")
            + _d(rx, "dropped_no_peer")
            + _d(rx, "dropped_send_failed")
            + _d(rx, "ether_pkt")
        )
    )
    return [
        (f"S inject residual (±{tx_q} queued)", inject_residual),
        (f"R forward residual (±{rx_q} queued)", rx_residual),
    ]


def format_stages(
    label: str,
    rows: list[tuple[str, int, int]],
    residuals: list[tuple[str, int]] | None = None,
    extra_lines: list[str] | None = None,
) -> str:
    lines = [f"\n-- {label} radio drop stages (left−right = drop at stage) --"]
    for name, left, right in rows:
        drop = left - right
        lines.append(f"  {name:22s}  {left:7d} → {right:7d}   drop {drop:7d}")
    if residuals:
        lines.append("")
        for name, value in residuals:
            flag = " OK" if abs(value) <= 2 else " ** uncounted? **"
            lines.append(f"  check {name}: {value:+d}{flag}")
    if extra_lines:
        lines.extend(extra_lines)
    return "\n".join(lines)


def mgr_request(
    bind: tuple[str, int],
    dest: tuple[str, int],
    cmd: str,
    timeout: float = 2.0,
) -> str:
    """Send one manager m-plane command; the reply arrives on `bind` (console_out)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.bind(bind)
        sock.sendto(cmd.encode(), dest)
        return sock.recvfrom(65535)[0].decode(errors="replace")
    finally:
        sock.close()


def read_via_manager(
    bind: tuple[str, int],
    dest: tuple[str, int],
    timeout: float = 3.0,
) -> dict[str, Any]:
    """Radio counters through the manager's `radio_stats`: {'manager', 'tx', 'rx'}."""
    text = mgr_request(bind, dest, "radio_stats", timeout)
    if text.startswith("NOK"):
        raise OSError(f"manager radio_stats failed: {text.strip()}")
    out: dict[str, Any] = {"manager": f"{dest[0]}:{dest[1]}", "tx": {}, "rx": {}}
    for line in text.splitlines():
        if line.startswith("tx_info"):
            out["tx"] = parse_kv_line(line)
        elif line.startswith("rx_info"):
            out["rx"] = parse_kv_line(line)
    return out


def mgr_get_metrics(
    bind: tuple[str, int],
    dest: tuple[str, int],
    timeout: float = 2.0,
) -> dict[str, int]:
    text = mgr_request(bind, dest, "get_metrics", timeout)
    out: dict[str, int] = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        try:
            out[key.strip()] = int(val.strip())
        except ValueError:
            pass
    return out


def mgr_delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]:
    keys = ("radio_tx_pkt", "radio_rx_pkt")
    return {k: after.get(k, 0) - before.get(k, 0) for k in keys}


def snapshot_managers(
    mgr_a: tuple[Endpoint, Endpoint] = MGR_A,
    mgr_b: tuple[Endpoint, Endpoint] = MGR_B,
    *,
    quiet: bool = False,
) -> dict[str, Any]:
    """Radio counters and manager metrics for both sides, via the managers only."""

    def side(name: str, ends: tuple[Endpoint, Endpoint]) -> tuple[dict[str, Any], dict[str, int]]:
        bind, dest = ends
        radio: dict[str, Any] = {"tx": {}, "rx": {}}
        metrics: dict[str, int] = {}
        try:
            radio = read_via_manager(bind, dest)
        except OSError as err:
            if not quiet:
                print(f"warning: manager {name} radio_stats failed: {err}", file=sys.stderr)
        try:
            metrics = mgr_get_metrics(bind, dest)
        except OSError as err:
            if not quiet:
                print(f"warning: manager {name} get_metrics failed: {err}", file=sys.stderr)
        return radio, metrics

    radio_a, metrics_a = side("A", mgr_a)
    radio_b, metrics_b = side("B", mgr_b)
    return {"a": radio_a, "b": radio_b, "mgr_a": metrics_a, "mgr_b": metrics_b}


def save_snapshot(path: Path, data: dict[str, Any]) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def load_snapshot(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def print_diff_run(
    before: dict[str, Any],
    after: dict[str, Any],
    direction: str,
    usb_air: int | None = None,
) -> None:
    if direction == "ab":
        tx_ip, rx_ip = "a", "b"
        mgr_s_key, mgr_r_key = "mgr_a", "mgr_b"
        label = "A→B"
    elif direction == "ba":
        tx_ip, rx_ip = "b", "a"
        mgr_s_key, mgr_r_key = "mgr_b", "mgr_a"
        label = "B→A"
    else:
        raise SystemExit(f"unknown --dir {direction!r} (use ab or ba)")

    if not firmware_has_metrics(before[tx_ip]) or not firmware_has_metrics(after[tx_ip]):
        print(
            "radio drop counters unavailable (older firmware); "
            "tx_info/rx_info missing ether_pkt/air_pkt fields"
        )
        return
    if not firmware_has_metrics(before[rx_ip]) or not firmware_has_metrics(after[rx_ip]):
        print("radio drop counters unavailable on peer radio")
        return

    d_tx = delta(before[tx_ip], after[tx_ip])
    d_rx = delta(before[rx_ip], after[rx_ip])
    if d_tx["rebooted"] or d_rx["rebooted"]:
        print(f"warning: radio reboot detected during {label}; interval discarded")
        return

    dm_s = mgr_delta(before.get(mgr_s_key, {}), after.get(mgr_s_key, {}))
    dm_r = mgr_delta(before.get(mgr_r_key, {}), after.get(mgr_r_key, {}))
    rows = stages(d_tx, d_rx, dm_s, dm_r)
    extras: list[str] = []
    if usb_air is not None and usb_air > 0:
        extras.append(
            f"\n  USB monitor on-air (independent): {usb_air}  "
            f"(S air_pkt Δ={_d(d_tx['tx'], 'air_pkt')})"
        )
    ts_us = d_tx.get("ts_delta_us") or 1
    extras.append(
        f"  radio interval Δts={ts_us} µs "
        f"(rates use per-radio clock; do not compare ts across radios)"
    )
    print(
        format_stages(
            label,
            rows,
            residual_checks(d_tx, d_rx),
            extras,
        )
    )


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--save", metavar="FILE", help="write JSON snapshot (both radios)")
    p.add_argument("--diff", nargs=2, metavar=("BEFORE", "AFTER"), help="print stage table")
    p.add_argument("--dir", choices=("ab", "ba"), help="direction for --diff")
    p.add_argument(
        "--mgr-a-bind",
        default="127.0.0.1:2401",
        help="manager A console_out, bound for replies (default 127.0.0.1:2401)",
    )
    p.add_argument(
        "--mgr-a-dest",
        default="127.0.0.1:2400",
        help="manager A console_in (default 127.0.0.1:2400)",
    )
    p.add_argument("--mgr-b-bind", default="127.0.0.1:2411")
    p.add_argument("--mgr-b-dest", default="127.0.0.1:2410")
    args = p.parse_args()

    def parse_ep(s: str) -> tuple[str, int]:
        host, port_s = s.rsplit(":", 1)
        return host, int(port_s)

    if args.diff:
        if not args.dir:
            p.error("--diff requires --dir ab|ba")
        print_diff_run(
            load_snapshot(Path(args.diff[0])),
            load_snapshot(Path(args.diff[1])),
            args.dir,
        )
        return 0

    data = snapshot_managers(
        (parse_ep(args.mgr_a_bind), parse_ep(args.mgr_a_dest)),
        (parse_ep(args.mgr_b_bind), parse_ep(args.mgr_b_dest)),
    )
    if args.save:
        save_snapshot(Path(args.save), data)
    else:
        print(json.dumps(data, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
