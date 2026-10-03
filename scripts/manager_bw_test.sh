#!/usr/bin/env bash
# Start winject-manager on both radios and run bw_test through managers.
#
# UDP forwarding (lat_udp_*.cfg by default; --bw uses bw_*.cfg).
#
# Each manager programs its radio (PHY, CCA, domain filter) at startup; the
# script never talks to a radio directly. Two bus pairs: b2/a1 for A→B,
# c3/d4 for B→A. Edit the cfg files for radio IPs / host local_ip, or pass
# --a/--b/--host (or legacy positional RADIO_A RADIO_B HOST).
#
# Usage (default --modulation OFDM_24M when omitted — peer promisc RX path):
#   ./scripts/manager_bw_test.sh --a 192.168.253.11 --b 192.168.253.12 --no-cca
#   ./scripts/manager_bw_test.sh --modulation OFDM_24M
#   ./scripts/manager_bw_test.sh --bw --modulation OFDM_24M
#   ./scripts/manager_bw_test.sh 192.168.253.11 192.168.253.12 192.168.253.106

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"

CONF_UDP_A="$ROOT/configuration/winject-tests/esp32/lat_udp_a.cfg"
CONF_UDP_B="$ROOT/configuration/winject-tests/esp32/lat_udp_b.cfg"
CONF_TCP_A="$ROOT/configuration/winject-tests/esp32/bw_a.cfg"
CONF_TCP_B="$ROOT/configuration/winject-tests/esp32/bw_b.cfg"
LOG_DIR="${TMPDIR:-/tmp}/winject-manager-$$"
mkdir -p "$LOG_DIR"

RADIO_A="192.168.253.11"
RADIO_B="192.168.253.12"
HOST_IP="192.168.253.106"
HOST_SET=0
USE_BW_CFG=0
BW_ARGS=()
# Radio settings written into the manager configs (empty = keep the cfg's).
CFG_DOMAIN=""
CFG_CHANNEL=""
CFG_MODULATION=""
CFG_POWER=""
CFG_CCA=""
MODULATION_SET=0
CHANNEL_SET=0
ALL_MODULATIONS=0
MGR_MAX_DATA_PER_TICK=""
MGR_MAX_RATE_KBPS=""
BENCH_PROFILE=""
if [[ "${1:-}" == "--" ]]; then
  shift
fi

# Legacy positional IPs: RADIO_A RADIO_B [HOST_IP] [bw args...]
if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  RADIO_A="$1"
  shift
  if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    RADIO_B="$1"
    shift
  fi
  if [[ $# -ge 1 && "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    HOST_IP="$1"
    HOST_SET=1
    shift
  fi
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      exec python3 "$ROOT/tools/bw_test.py" --help
      ;;
    --bw)
      USE_BW_CFG=1
      shift
      ;;
    --udp)
      USE_BW_CFG=0
      shift
      ;;
    --tcp)
      echo "manager TCP upstreams were removed; use UDP (--udp) or your own reliability layer" >&2
      exit 1
      ;;
    --a)
      RADIO_A="${2:?--a needs an IP}"
      shift 2
      ;;
    --a=*)
      RADIO_A="${1#--a=}"
      shift
      ;;
    --b)
      RADIO_B="${2:?--b needs an IP}"
      shift 2
      ;;
    --b=*)
      RADIO_B="${1#--b=}"
      shift
      ;;
    --host)
      HOST_IP="${2:?--host needs an IP}"
      HOST_SET=1
      shift 2
      ;;
    --host=*)
      HOST_IP="${1#--host=}"
      HOST_SET=1
      shift
      ;;
    --domain)
      CFG_DOMAIN="${2:?--domain needs a value}"
      BW_ARGS+=(--domain "$2")
      shift 2
      ;;
    --domain=*)
      CFG_DOMAIN="${1#--domain=}"
      BW_ARGS+=(--domain "${1#--domain=}")
      shift
      ;;
    --channel)
      CHANNEL_SET=1
      CFG_CHANNEL="${2:?--channel needs a value}"
      BW_ARGS+=(--channel "$2")
      shift 2
      ;;
    --channel=*)
      CHANNEL_SET=1
      CFG_CHANNEL="${1#--channel=}"
      BW_ARGS+=(--channel "${1#--channel=}")
      shift
      ;;
    --modulation)
      MODULATION_SET=1
      CFG_MODULATION="${2:?--modulation needs a value}"
      BW_ARGS+=(--modulation "$2")
      shift 2
      ;;
    --modulation=*)
      MODULATION_SET=1
      CFG_MODULATION="${1#--modulation=}"
      BW_ARGS+=(--modulation "${1#--modulation=}")
      shift
      ;;
    --all)
      ALL_MODULATIONS=1
      BW_ARGS+=("$1")
      shift
      ;;
    --no-cca)
      CFG_CCA=0
      BW_ARGS+=("$1")
      shift
      ;;
    --cca)
      CFG_CCA=1
      BW_ARGS+=("$1")
      shift
      ;;
    --power)
      CFG_POWER="${2:?--power needs a value}"
      shift 2
      ;;
    --power=*)
      CFG_POWER="${1#--power=}"
      shift
      ;;
    --max-data-per-tick)
      MGR_MAX_DATA_PER_TICK="${2:?--max-data-per-tick needs a value}"
      shift 2
      ;;
    --max-data-per-tick=*)
      MGR_MAX_DATA_PER_TICK="${1#--max-data-per-tick=}"
      shift
      ;;
    --max-rate-kbps)
      MGR_MAX_RATE_KBPS="${2:?--max-rate-kbps needs a value}"
      shift 2
      ;;
    --max-rate-kbps=*)
      MGR_MAX_RATE_KBPS="${1#--max-rate-kbps=}"
      shift
      ;;
    --profile)
      BENCH_PROFILE="${2:?--profile needs a value}"
      shift 2
      ;;
    --profile=*)
      BENCH_PROFILE="${1#--profile=}"
      shift
      ;;
    *)
      BW_ARGS+=("$1")
      shift
      ;;
  esac
done

# Legacy OFDM only for ESP peer RX until HT promisc delivers our MPDUs (winject.md).
if [[ "$MODULATION_SET" -eq 0 && "$ALL_MODULATIONS" -eq 0 ]]; then
  CFG_MODULATION=OFDM_24M
  BW_ARGS+=(--modulation OFDM_24M)
fi

# Bench default: channel 1 (omit only with explicit --channel).
if [[ "$CHANNEL_SET" -eq 0 ]]; then
  CFG_CHANNEL=1
  BW_ARGS+=(--channel 1)
fi

# Saturated peer bw tests: keep the medium clear unless --cca was given.
CFG_CCA="${CFG_CCA:-0}"
# A --modulation list is swept by bw_test; the managers start on the first one.
CFG_MODULATION="${CFG_MODULATION%%,*}"

if [[ "$HOST_SET" -eq 0 ]]; then
  HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 22)); print(s.getsockname()[0]); s.close()")"
fi

ensure_winject_manager "$ROOT"


if [[ "$USE_BW_CFG" -eq 1 ]]; then
  CONF_A="$CONF_TCP_A"
  CONF_B="$CONF_TCP_B"
else
  CONF_A="$CONF_UDP_A"
  CONF_B="$CONF_UDP_B"
fi
PATH_FLAG=--udp
PATH_LABEL=udp

patch_conf() {
  local file="$1" device="$2" gci_in="$3" gci_out="$4"
  local -a sed_args=(
    -e "s/^winject\.device.*/winject.device        = ${device}/"
    -e "s/^winject\.local_ip.*/winject.local_ip      = ${HOST_IP}/"
    -e "s/^winject\.skip_console.*/winject.skip_console  = 0/"
  )
  if [[ -n "$CFG_DOMAIN" ]]; then
    sed_args+=(-e "s/^winject\.domain.*/winject.domain        = ${CFG_DOMAIN}/")
  fi
  if [[ -n "$CFG_CHANNEL" ]]; then
    sed_args+=(-e "s/^winject\.channel.*/winject.channel       = ${CFG_CHANNEL}/")
  fi
  if [[ -n "$CFG_MODULATION" ]]; then
    sed_args+=(-e "s/^winject\.modulation.*/winject.modulation    = ${CFG_MODULATION}/")
  fi
  if [[ -n "$CFG_POWER" ]]; then
    sed_args+=(-e "s/^winject\.power.*/winject.power         = ${CFG_POWER}/")
  fi
  sed "${sed_args[@]}" "$file"
  if [[ -n "$CFG_CCA" ]]; then
    printf 'winject.cca           = %s\n' "$CFG_CCA"
  fi
  printf 'manager.console_in    = 127.0.0.1:%s\n' "$gci_in"
  printf 'manager.console_out   = 127.0.0.1:%s\n' "$gci_out"
}

CONF_A_RUN="$LOG_DIR/winject_a.conf"
CONF_B_RUN="$LOG_DIR/winject_b.conf"
patch_conf "$CONF_A" "$RADIO_A" 2400 2401 >"$CONF_A_RUN"
patch_conf "$CONF_B" "$RADIO_B" 2410 2411 >"$CONF_B_RUN"
# 10 Mbps profile sweep: manager tick cap 4 on A side (default in tree).
if [[ "$BENCH_PROFILE" == "10mbps" && -z "$MGR_MAX_DATA_PER_TICK" ]]; then
  if grep -q '^winject\.max_data_per_tick' "$CONF_A_RUN"; then
    sed -i 's/^winject\.max_data_per_tick.*/winject.max_data_per_tick = 4/' "$CONF_A_RUN"
  else
    printf 'winject.max_data_per_tick = 4\n' >>"$CONF_A_RUN"
  fi
fi
if [[ "$BENCH_PROFILE" == "15mbps" ]]; then
  if [[ -z "$MGR_MAX_RATE_KBPS" ]]; then
    sed -i 's/^winject\.max_rate_kbps.*/winject.max_rate_kbps = 15000/' "$CONF_A_RUN"
  fi
  if [[ -z "$MGR_MAX_DATA_PER_TICK" ]]; then
    if grep -q '^winject\.max_data_per_tick' "$CONF_A_RUN"; then
      sed -i 's/^winject\.max_data_per_tick.*/winject.max_data_per_tick = 4/' "$CONF_A_RUN"
    else
      printf 'winject.max_data_per_tick = 4\n' >>"$CONF_A_RUN"
    fi
  fi
fi
if [[ -n "$MGR_MAX_RATE_KBPS" ]]; then
  sed -i "s/^winject\.max_rate_kbps.*/winject.max_rate_kbps = ${MGR_MAX_RATE_KBPS}/" "$CONF_A_RUN"
fi
if [[ -n "$MGR_MAX_DATA_PER_TICK" ]]; then
  if grep -q '^winject\.max_data_per_tick' "$CONF_A_RUN"; then
    sed -i "s/^winject\.max_data_per_tick.*/winject.max_data_per_tick = ${MGR_MAX_DATA_PER_TICK}/" "$CONF_A_RUN"
  else
    printf 'winject.max_data_per_tick = %s\n' "$MGR_MAX_DATA_PER_TICK" >>"$CONF_A_RUN"
  fi
fi

cleanup() {
  if [[ -n "${PID_A:-}" ]]; then kill "$PID_A" 2>/dev/null || true
  fi
  if [[ -n "${PID_B:-}" ]]; then kill "$PID_B" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Match the process name only: -f would also match shells whose command line
# mentions winject-manager (and kill the caller).
pkill -x winject-manager 2>/dev/null || true
sleep 1

echo "managers [$PATH_LABEL]: A=$RADIO_A B=$RADIO_B host=$HOST_IP logs=$LOG_DIR"
"$MANAGER" "$CONF_A_RUN" >"$LOG_DIR/manager_a.log" 2>&1 &
PID_A=$!
"$MANAGER" "$CONF_B_RUN" >"$LOG_DIR/manager_b.log" 2>&1 &
PID_B=$!

echo "waiting for managers..."
for i in $(seq 1 20); do
  if grep -q "manager running" "$LOG_DIR/manager_a.log" \
    && grep -q "manager running" "$LOG_DIR/manager_b.log"; then
    break
  fi
  sleep 0.25
done
if ! grep -q "manager running" "$LOG_DIR/manager_a.log"; then
  echo "manager A upstream setup failed; tail $LOG_DIR/manager_a.log"
  tail -20 "$LOG_DIR/manager_a.log"
  exit 1
fi
if ! grep -q "manager running" "$LOG_DIR/manager_b.log"; then
  echo "manager B upstream setup failed; tail $LOG_DIR/manager_b.log"
  tail -20 "$LOG_DIR/manager_b.log"
  exit 1
fi

export WINJECT_RADIO_SNAP_DIR="$LOG_DIR"
echo "running bw_test $PATH_FLAG --a $RADIO_A --b $RADIO_B --drop-stages ${BW_ARGS[*]}"
echo "radio counter snapshots → $LOG_DIR/radio_*.json"
# Do not exec: the EXIT trap must run to kill managers.
set +e
python3 "$ROOT/tools/bw_test.py" \
  "$PATH_FLAG" \
  --a "$RADIO_A" \
  --b "$RADIO_B" \
  --drop-stages \
  "${BW_ARGS[@]}"
status=$?
set -e
exit "$status"
