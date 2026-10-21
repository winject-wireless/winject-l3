#!/usr/bin/env bash
# Confirm wt0 OFDM high-TX cliff vs CCK on Path B (2 m OTA geometry).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
# shellcheck source=../scripts/ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"
LOG=/tmp/ofdm_pa_confirm.log
: >"$LOG"

run_one() {
  local pwr="$1" mod="$2" kbps="$3" label="$4"
  echo "=== $label pwr=$pwr mod=$mod kbps=$kbps ===" | tee -a "$LOG"
  python3 tools/radio_wt_power_sweep.py \
    --min-power "$pwr" --max-power "$pwr" \
    --kbps "$kbps" --modulation "$mod" \
    -o /tmp/_pwr_one.md 2>&1 | grep -E '^[0-9]+,' | tee -a "$LOG"
  stop_winject_managers
}

echo "# OFDM PA cliff confirm $(date -Iseconds)" | tee "$LOG"

echo "--- cliff scan OFDM_24M 15M (12-15 dBm) ---" | tee -a "$LOG"
for p in 12 13 14 15; do
  run_one "$p" OFDM_24M 15000 "cliff"
done

echo "--- 20 dBm OFDM vs CCK (8M offer) A->B loss from sweep row ---" | tee -a "$LOG"
run_one 20 OFDM_24M 15000 "ofdm20"
run_one 20 CCK_11M_L 8000 "cck20"

echo "--- B->A @ 20 dBm OFDM (wt1 TX) ---" | tee -a "$LOG"
stop_winject_managers
# use power_sweep internal would do both dirs; quick bw_test for BA only
ensure_winject_manager "$ROOT"
TMPD=$(mktemp -d /tmp/pa_confirm_XXXX)
patch() {
  sed -e "s/^radio\\.device.*/radio.device        = ${1}/" \
      -e "s/^radio\\.power.*/radio.power         = 20/" \
      -e "s/^radio\\.channel.*/radio.channel       = 1/" \
      -e "s/^radio\\.modulation.*/radio.modulation    = OFDM_24M/" \
      -e "s/^radio\\.cca.*/radio.cca           = 0/" \
      -e "s/^radio\\.skip_console.*/radio.skip_console  = 0/" \
      "$2"
  printf 'manager.console_in    = 127.0.0.1:%s\n' "$3"
}
patch 192.168.253.11 configuration/winject-tests/esp32/lat_udp_a.cfg 2400 >"$TMPD/a.conf"
patch 192.168.253.12 configuration/winject-tests/esp32/lat_udp_b.cfg 2410 >"$TMPD/b.conf"
"$MANAGER" "$TMPD/a.conf" >"$TMPD/a.log" 2>&1 &
"$MANAGER" "$TMPD/b.conf" >"$TMPD/b.log" 2>&1 &
wait_manager_radio "$TMPD/a.log"
wait_manager_radio "$TMPD/b.log"
python3 tools/bw_test.py --udp --no-cca --modulation OFDM_24M --channel 1 \
  --duration 5 --drain 1 --kbps 15000 --test-ba 2>&1 | grep -E 'B->A sent' | tee -a "$LOG"
stop_winject_managers

echo "done log=$LOG"
