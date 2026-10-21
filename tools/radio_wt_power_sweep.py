#!/usr/bin/env python3
"""Sweep TX power (dBm) at a fixed payload offer between two ESP32 radios.

Stops any running winject-manager, then waits until this sweep's managers
have programmed the radios. Does not open a radio console.

Output columns:
  tx_power, a_to_b_rate, a_to_b_loss, a_to_b_rssi,
  b_to_a_rate, b_to_a_loss, b_to_a_rssi
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from radio_stats import MGR_A, MGR_B, mgr_request  # noqa: E402

RE_AB = re.compile(
    r"A->B sent (\d+) recv (\d+)\s+([\d.]+) kbps\s+loss ([\d.]+)%"
)
RE_BA = re.compile(
    r"B->A sent (\d+) recv (\d+)\s+([\d.]+) kbps\s+loss ([\d.]+)%"
)


def parse_rssi(mgr: tuple[str, int]) -> int | None:
    try:
        text = mgr_request(mgr, "radio_info", timeout=3.0)
    except OSError:
        return None
    m = re.search(r"rssi=(-?\d+)", text)
    return int(m.group(1)) if m else None


def run_bw_leg(kbps: int, modulation: str, extra: list[str]) -> str:
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "bw_test.py"),
        "--udp",
        "--no-cca",
        "--modulation",
        modulation,
        "--channel",
        "1",
        "--duration",
        "5",
        "--drain",
        "1",
        "--kbps",
        str(kbps),
        *extra,
    ]
    proc = subprocess.run(
        cmd,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=180,
    )
    out = proc.stdout + proc.stderr
    if proc.returncode not in (0, 1):
        raise RuntimeError(f"bw_test failed rc={proc.returncode}\n{out[-2000:]}")
    return out


def parse_leg(pattern: re.Pattern[str], text: str) -> tuple[float, float] | None:
    m = pattern.search(text)
    if not m:
        return None
    return float(m.group(3)), float(m.group(4))


def start_managers(a: str, b: str, power: int, modulation: str) -> None:
    script = f"""
set -euo pipefail
ROOT="{ROOT}"
source "$ROOT/scripts/ensure_manager.sh"
ensure_winject_manager "$ROOT"
LOG_DIR="${{TMPDIR:-/tmp}}/winject-pwr-sweep-$$"
mkdir -p "$LOG_DIR"
RADIO_A="{a}"
RADIO_B="{b}"
CONF_A="$ROOT/configuration/winject-tests/esp32/lat_udp_a.cfg"
CONF_B="$ROOT/configuration/winject-tests/esp32/lat_udp_b.cfg"
patch_conf() {{
  local file="$1" device="$2" gci_in="$3"
  sed -e "s/^radio\\.device.*/radio.device        = ${{device}}/" \\
      -e "s/^radio\\.channel.*/radio.channel       = 1/" \\
      -e "s/^radio\\.modulation.*/radio.modulation    = {modulation}/" \\
      -e "s/^radio\\.power.*/radio.power         = {power}/" \\
      "$file"
  printf 'radio.cca           = 0\\n'
  printf 'winject.console_in    = 127.0.0.1:%s\\n' "$gci_in"
}}
patch_conf "$CONF_A" "$RADIO_A" 2400 >"$LOG_DIR/winject_a.conf"
patch_conf "$CONF_B" "$RADIO_B" 2410 >"$LOG_DIR/winject_b.conf"
stop_winject_managers
"$MANAGER" "$LOG_DIR/winject_a.conf" >"$LOG_DIR/manager_a.log" 2>&1 &
"$MANAGER" "$LOG_DIR/winject_b.conf" >"$LOG_DIR/manager_b.log" 2>&1 &
wait_manager_radio "$LOG_DIR/manager_a.log"
wait_manager_radio "$LOG_DIR/manager_b.log"
"""
    proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stdout + proc.stderr)


def stop_managers() -> None:
    script = f"""
set -euo pipefail
source "{ROOT}/scripts/ensure_manager.sh"
stop_winject_managers
"""
    proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stdout + proc.stderr)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.11")
    p.add_argument("--b", default="192.168.253.12")
    p.add_argument("--kbps", type=int, default=15000)
    p.add_argument("--modulation", default="OFDM_24M")
    p.add_argument("--min-power", type=int, default=2)
    p.add_argument("--max-power", type=int, default=20)
    p.add_argument("--power-step", type=int, default=1)
    p.add_argument("-o", "--output", type=Path, required=True)
    p.add_argument("--notes", default="", help="extra markdown line under title")
    args = p.parse_args()

    powers = list(
        range(args.min_power, args.max_power + 1, args.power_step)
    )
    header = (
        "tx_power, a_to_b_rate, a_to_b_loss, a_to_b_rssi, "
        "b_to_a_rate, b_to_a_loss, b_to_a_rssi"
    )
    rows: list[str] = [header]

    stop_managers()
    try:
        for idx, power in enumerate(powers):
            print(f"=== power {power} dBm ({idx + 1}/{len(powers)}) ===", flush=True)
            start_managers(args.a, args.b, power, args.modulation)
            skip = [] if idx == 0 else ["--skip-config"]
            common = [
                "--a",
                args.a,
                "--b",
                args.b,
                "--modulation",
                args.modulation,
                *skip,
            ]
            out_ab = run_bw_leg(args.kbps, args.modulation, [*common, "--test-ab"])
            rssi_ab = parse_rssi(MGR_B)
            ab = parse_leg(RE_AB, out_ab)
            if ab is None:
                raise RuntimeError(f"parse A->B @ {power} dBm\n{out_ab[-1500:]}")
            out_ba = run_bw_leg(
                args.kbps, args.modulation, ["--skip-config", *common, "--test-ba"]
            )
            rssi_ba = parse_rssi(MGR_A)
            ba = parse_leg(RE_BA, out_ba)
            if ba is None:
                raise RuntimeError(f"parse B->A @ {power} dBm\n{out_ba[-1500:]}")
            row = (
                f"{power}, {ab[0]:.1f}, {ab[1]:.1f}, "
                f"{rssi_ab if rssi_ab is not None else ''}, "
                f"{ba[0]:.1f}, {ba[1]:.1f}, "
                f"{rssi_ba if rssi_ba is not None else ''}"
            )
            rows.append(row)
            print(row, flush=True)
            stop_managers()
            time.sleep(0.5)
    finally:
        stop_managers()

    table_lines = [
        "| " + header.replace(", ", " | ") + " |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows[1:]:
        table_lines.append("| " + row.replace(", ", " | ") + " |")
    table = "\n".join(table_lines)
    notes = f"\n{args.notes}\n" if args.notes else "\n"
    md = (
        "# radio-wt0 / radio-wt1 TX power sweep\n\n"
        f"- **Offer:** {args.kbps} kbps payload, `{args.modulation}`, channel 1, CCA off\n"
        f"- **TX power:** {args.min_power}–{args.max_power} dBm "
        f"(step {args.power_step}), both radios\n"
        f"- **Radios:** `{args.a}` (A) / `{args.b}` (B)\n"
        f"- **Duration:** 5 s per direction (+ 1 s drain)\n"
        f"{notes}\n"
        f"{table}\n"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(md, encoding="utf-8")
    print(f"wrote {args.output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
