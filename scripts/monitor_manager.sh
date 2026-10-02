#!/usr/bin/env bash
# Tail winject-manager logs and show periodic stats / errors.
#
# Usage:
#   ./scripts/monitor_manager.sh                    # latest /tmp log dir
#   ./scripts/monitor_manager.sh /tmp/winject-manager-12345/manager_a.log
#   ./scripts/monitor_manager.sh --all              # both A and B logs
#   ./scripts/monitor_manager.sh --radio 192.168.253.11   # per-second radio Δ counters

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RADIO_IP=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --radio)
      RADIO_IP="${2:?--radio needs an IP}"
      shift 2
      ;;
    --radio=*)
      RADIO_IP="${1#--radio=}"
      shift
      ;;
    *)
      break
      ;;
  esac
done

if [[ -n "$RADIO_IP" ]]; then
  exec python3 - "$RADIO_IP" "$ROOT/tools" <<'PY'
import sys
import time

sys.path.insert(0, sys.argv[2])
from radio_stats import delta, read

ip = sys.argv[1]
prev = read(ip)
print(f"radio {ip}: polling tx_info/rx_info every 1s (Ctrl+C to stop)")
while True:
    time.sleep(1.0)
    cur = read(ip)
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
  echo "usage: $0 [manager_a.log]  or  $0 --all  or  $0 --radio IP" >&2
  echo "no log found under /tmp/winject-manager-*" >&2
  exit 1
fi

echo "watching $LOG (stats + ERR/WRN)"
exec tail -F "$LOG" | rg --line-buffered 'stats |ERR |WRN '
