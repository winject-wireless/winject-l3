#!/usr/bin/env bash
# Capture winject A->B on T3U monitor while running bw_test. Usage:
#   sudo ./tools/t3u_a2b_capture.sh 20 [/tmp/out.pcap]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/scripts/ensure_manager.sh"
ensure_winject_manager "$ROOT"

PWR="${1:?power dBm}"
PCAP="${2:-/tmp/t3u_a2b_${PWR}dbm.pcap}"
MON="${T3U_MON:-wlx3c789537952a}"

nmcli device set "$MON" managed no 2>/dev/null || true
ip link set "$MON" down 2>/dev/null || true
iw dev "$MON" set type monitor 2>/dev/null || true
ip link set "$MON" up
iw dev "$MON" set freq 2412 HT20

LOG="$(mktemp -d /tmp/t3u_cap_XXXX)"
patch_conf() {
  local dev="$1" in="$2" out="$3"
  sed -e "s/^radio\\.device.*/radio.device        = ${dev}/" \
      -e "s/^radio\\.channel.*/radio.channel       = 1/" \
      -e "s/^radio\\.modulation.*/radio.modulation    = OFDM_24M/" \
      -e "s/^radio\\.power.*/radio.power         = ${PWR}/" \
      -e "s/^radio\\.cca.*/radio.cca           = 0/" \
      "$in"
  printf 'winject.console_in    = 127.0.0.1:%s\n' "$out"
}

patch_conf 192.168.253.11 "$ROOT/configuration/winject-tests/esp32/lat_udp_a.cfg" 2400 >"$LOG/a.conf"
patch_conf 192.168.253.12 "$ROOT/configuration/winject-tests/esp32/lat_udp_b.cfg" 2410 >"$LOG/b.conf"

pkill -f "tcpdump -i ${MON}" 2>/dev/null || true
stop_winject_managers
"$MANAGER" "$LOG/a.conf" >"$LOG/a.log" 2>&1 &
"$MANAGER" "$LOG/b.conf" >"$LOG/b.log" 2>&1 &
wait_manager_radio "$LOG/a.log"
wait_manager_radio "$LOG/b.log"

rm -f "$PCAP"
CAP_SEC=$((5 + 2))
timeout --foreground "${CAP_SEC}s" tcpdump -i "$MON" -w "$PCAP" -s 256 \
  "wlan addr3 ca:fe:ba:be:12:34" 2>"$LOG/tcpdump.err" &
TD=$!
sleep 0.8
python3 "$ROOT/tools/bw_test.py" --udp --no-cca --modulation OFDM_24M --channel 1 \
  --duration 5 --drain 1 --kbps 15000 --test-ab 2>"$LOG/bw.err" | tee "$LOG/bw.out"
wait "$TD" 2>/dev/null || true
chmod a+r "$PCAP" 2>/dev/null || true
if [[ ! -s "$PCAP" ]] && [[ -s "$LOG/tcpdump.err" ]]; then
  echo "tcpdump stderr:" >&2
  tail -5 "$LOG/tcpdump.err" >&2
fi
stop_winject_managers || true

echo "pcap=$PCAP"
grep "A->B sent" "$LOG/bw.out" || true
if [[ -f /tmp/analyze_t3u_winject.py ]]; then
  if [[ -s "$PCAP" ]]; then
    python3 /tmp/analyze_t3u_winject.py "$PCAP" || true
  else
    echo "T3U filtered pcap empty (0 winject decodes on air at this power/placement)"
  fi
fi
