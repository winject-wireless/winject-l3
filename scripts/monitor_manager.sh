#!/usr/bin/env bash
# Tail winject-manager logs and show periodic stats / errors.
#
# Usage:
#   ./scripts/monitor_manager.sh                    # latest /tmp log dir
#   ./scripts/monitor_manager.sh /tmp/winject-manager-12345/manager_a.log
#   ./scripts/monitor_manager.sh --all              # both A and B logs
#   ./scripts/monitor_manager.sh --radio a          # per-second radio Δ counters via manager A

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RADIO_SIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --radio)
      RADIO_SIDE="${2:?--radio needs a or b}"
      shift 2
      ;;
    --radio=*)
      RADIO_SIDE="${1#--radio=}"
      shift
      ;;
    *)
      break
      ;;
  esac
done

if [[ -n "$RADIO_SIDE" ]]; then
  case "$RADIO_SIDE" in
    a|A|b|B) ;;
    *)
      echo "error: --radio must be a or b (got: $RADIO_SIDE)" >&2
      exit 1
      ;;
  esac
  exec python3 - "$RADIO_SIDE" "$ROOT/tools" <<'PY'
import sys
import time

sys.path.insert(0, sys.argv[2])
from radio_stats import MGR_A, MGR_B, delta, read_via_manager

side = sys.argv[1].upper()
bind, dest = MGR_A if side == "A" else MGR_B


def read() -> dict:
    while True:
        try:
            return read_via_manager(bind, dest)
        except OSError as err:
            print(f"  [manager {side} radio_stats failed: {err}; retrying]")
            time.sleep(1.0)


prev = read()
print(f"radio {side} (manager {dest[0]}:{dest[1]}): polling radio_stats every 1s (Ctrl+C to stop)")
while True:
    time.sleep(1.0)
    cur = read()
    d = delta(prev, cur)
    if d["rebooted"]:
        print("  [radio rebooted — resetting baseline]")
        prev = cur
        continue
    tx = d["tx"]
    rx = d["rx"]
    parts = []
    for key in (
        "ether_pkt",
        "air_pkt",
        "dropped_invalid_frame",
        "dropped_tx_queue",
        "dropped_wifi",
    ):
        v = tx.get(key)
        if v:
            parts.append(f"Δtx.{key}={v}")
    for key in (
        "ether_pkt",
        "air_pkt",
        "dropped_filter_mismatched",
        "dropped_rx_queue",
        "dropped_no_peer",
        "dropped_send_failed",
    ):
        v = rx.get(key)
        if v:
            parts.append(f"Δrx.{key}={v}")
    ts_us = d.get("ts_delta_us") or 0
    if parts:
        print(f"  Δts={ts_us}µs  " + "  ".join(parts))
    prev = cur
PY
fi

if [[ "${1:-}" == "--all" ]]; then
  LOG_A="$(ls -td /tmp/winject-manager-*/manager_a.log 2>/dev/null | head -1)"
  LOG_B="$(ls -td /tmp/winject-manager-*/manager_b.log 2>/dev/null | head -1)"
  if [[ -z "$LOG_A" || -z "$LOG_B" ]]; then
    echo "no /tmp/winject-manager-* logs found; start manager_bw_test.sh first" >&2
    exit 1
  fi
  echo "watching A=$LOG_A  B=$LOG_B"
  exec tail -F "$LOG_A" "$LOG_B" | rg --line-buffered 'stats |ERR |WRN '
fi

LOG="${1:-}"
if [[ -z "$LOG" ]]; then
  LOG="$(ls -td /tmp/winject-manager-*/manager_a.log 2>/dev/null | head -1 || true)"
fi
if [[ -z "$LOG" || ! -f "$LOG" ]]; then
  echo "usage: $0 [manager_a.log]  or  $0 --all  or  $0 --radio a|b" >&2
  echo "no log found under /tmp/winject-manager-*" >&2
  exit 1
fi

echo "watching $LOG (stats + ERR/WRN)"
exec tail -F "$LOG" | rg --line-buffered 'stats |ERR |WRN '
