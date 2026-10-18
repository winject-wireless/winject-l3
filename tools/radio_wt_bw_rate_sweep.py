#!/usr/bin/env python3
"""Sweep payload offer (kbps) between two ESP32 radios via running managers.

Writes CSV rows:
  requested_rate, a_to_b_rate, a_to_b_loss, a_to_b_rssi, b_to_a_rate, b_to_a_loss, b_to_a_rssi
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


def run_bw_leg(extra: list[str]) -> str:
    cmd = [
        sys.executable,
        str(ROOT / "tools" / "bw_test.py"),
        "--udp",
        "--no-cca",
        "--modulation",
        "OFDM_24M",
        "--channel",
        "1",
        "--duration",
        "5",
        "--drain",
        "1",
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
    kbps = float(m.group(3))
    loss = float(m.group(4))
    return kbps, loss


def start_managers(a: str, b: str, power: int) -> subprocess.Popen[bytes]:
    script = f"""
set -euo pipefail
ROOT="{ROOT}"
source "$ROOT/scripts/ensure_manager.sh"
ensure_winject_manager "$ROOT"
LOG_DIR="${{TMPDIR:-/tmp}}/winject-bw-sweep-$$"
mkdir -p "$LOG_DIR"
RADIO_A="{a}"
RADIO_B="{b}"
HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 22)); print(s.getsockname()[0]); s.close()")"
CONF_A="$ROOT/configuration/winject-tests/esp32/lat_udp_a.cfg"
CONF_B="$ROOT/configuration/winject-tests/esp32/lat_udp_b.cfg"
patch_conf() {{
  local file="$1" device="$2" gci_in="$3"
  sed -e "s/^winject\\.device.*/winject.device        = ${{device}}/" \\
      -e "s/^winject\\.skip_console.*/winject.skip_console  = 0/" \\
      -e "s/^winject\\.channel.*/winject.channel       = 1/" \\
      -e "s/^winject\\.modulation.*/winject.modulation    = OFDM_24M/" \\
      -e "s/^winject\\.power.*/winject.power         = {power}/" \\
      "$file"
  printf 'winject.cca           = 0\\n'
  printf 'manager.console_in    = 127.0.0.1:%s\\n' "$gci_in"
}}
patch_conf "$CONF_A" "$RADIO_A" 2400 >"$LOG_DIR/winject_a.conf"
patch_conf "$CONF_B" "$RADIO_B" 2410 >"$LOG_DIR/winject_b.conf"
pkill -x winject-manager 2>/dev/null || true
sleep 1
"$MANAGER" "$LOG_DIR/winject_a.conf" >"$LOG_DIR/manager_a.log" 2>&1 &
PID_A=$!
"$MANAGER" "$LOG_DIR/winject_b.conf" >"$LOG_DIR/manager_b.log" 2>&1 &
PID_B=$!
for i in $(seq 1 40); do
  if grep -q "manager running" "$LOG_DIR/manager_a.log" \\
    && grep -q "manager running" "$LOG_DIR/manager_b.log"; then
    echo "managers ready log_dir=$LOG_DIR pid_a=$PID_A pid_b=$PID_B"
    exit 0
  fi
  sleep 0.25
done
echo "manager startup failed" >&2
tail -30 "$LOG_DIR/manager_a.log" >&2 || true
tail -30 "$LOG_DIR/manager_b.log" >&2 || true
exit 1
"""
    proc = subprocess.run(["bash", "-c", script], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stdout + proc.stderr)
    print(proc.stdout.strip(), flush=True)
    # Keep managers running in this shell's children — they are orphaned but stay up.
    return proc


def stop_managers() -> None:
    subprocess.run(["pkill", "-x", "winject-manager"], check=False)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--a", default="192.168.253.11")
    p.add_argument("--b", default="192.168.253.12")
    p.add_argument("--min-kbps", type=int, default=1000)
    p.add_argument("--max-kbps", type=int, default=18000)
    p.add_argument("--step-kbps", type=int, default=1000)
    p.add_argument(
        "--power",
        type=int,
        default=20,
        help="winject.power / radio_tx tx_power (dBm) on both radios",
    )
    p.add_argument("-o", "--output", type=Path, required=True)
    args = p.parse_args()

    rates = list(range(args.min_kbps, args.max_kbps + 1, args.step_kbps))
    stop_managers()
    start_managers(args.a, args.b, args.power)
    time.sleep(1.5)

    rows: list[str] = []
    header = (
        "requested_rate, a_to_b_rate, a_to_b_loss, a_to_b_rssi, "
        "b_to_a_rate, b_to_a_loss, b_to_a_rssi"
    )
    rows.append(header)

    try:
        for i, kbps in enumerate(rates):
            skip = [] if i == 0 else ["--skip-config"]
            out_ab = run_bw_leg(
                [
                    *skip,
                    "--kbps",
                    str(kbps),
                    "--test-ab",
                    "--a",
                    args.a,
                    "--b",
                    args.b,
                ]
            )
            rssi_ab = parse_rssi(MGR_B)
            ab = parse_leg(RE_AB, out_ab)
            if ab is None:
                raise RuntimeError(f"parse A->B failed at {kbps} kbps\n{out_ab[-1500:]}")

            out_ba = run_bw_leg(
                [
                    "--skip-config",
                    "--kbps",
                    str(kbps),
                    "--test-ba",
                    "--a",
                    args.a,
                    "--b",
                    args.b,
                ]
            )
            rssi_ba = parse_rssi(MGR_A)
            ba = parse_leg(RE_BA, out_ba)
            if ba is None:
                raise RuntimeError(f"parse B->A failed at {kbps} kbps\n{out_ba[-1500:]}")

            row = (
                f"{kbps}, {ab[0]:.1f}, {ab[1]:.1f}, "
                f"{rssi_ab if rssi_ab is not None else ''}, "
                f"{ba[0]:.1f}, {ba[1]:.1f}, "
                f"{rssi_ba if rssi_ba is not None else ''}"
            )
            rows.append(row)
            print(row, flush=True)
    finally:
        stop_managers()

    out_path = args.output
    out_path.parent.mkdir(parents=True, exist_ok=True)
    header = rows[0]
    data_rows = rows[1:]
    table_lines = [
        "| " + header.replace(", ", " | ") + " |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in data_rows:
        table_lines.append("| " + row.replace(", ", " | ") + " |")
    table = "\n".join(table_lines) + "\n"
    md = (
        "# radio-wt0 / radio-wt1 bandwidth sweep\n\n"
        f"- **Radios:** radio-wt0 (`{args.a}`) → A, radio-wt1 (`{args.b}`) → B\n"
        f"- **TX power:** {args.power} dBm (both radios)\n"
        "- **Modulation:** OFDM_24M, channel 1, CCA off\n"
        "- **Duration:** 5 s per direction (+ 1 s drain each leg)\n"
        f"- **Offers:** {args.min_kbps}–{args.max_kbps} kbps step {args.step_kbps}\n"
        "- **Tool:** `winject-l3` `tools/bw_test.py` via paired `winject-manager`\n\n"
        f"{table}"
    )
    out_path.write_text(md, encoding="utf-8")
    print(f"wrote {out_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
