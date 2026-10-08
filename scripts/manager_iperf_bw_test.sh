#!/usr/bin/env bash
# Start winject-manager on both radios and measure UDP goodput with iperf (v2)
# through manager UDP forwarding.
#
# Scenarios (--scenario, configs in configuration/winject-tests/<scenario>/):
#   esp32    two ESP32 radios on Ethernet, managers use bw_{a,b}.cfg (default)
#   realtek  two RTL8812AU dongles on this host: starts winject-radio-realtek
#            with radio_{a,b}.cfg first (scripts/realtek_radios.sh), then the
#            managers with bw_{a,b}.cfg
#
# Topology (A→B):
#   iperf -c 127.0.0.1:29000 -u  → manager A UDP_SERVER → air
#     → manager B UDP_CLIENT → iperf -s -u -p 9002
# iperf UDP reports return over the same path (server → client upstream → air
# → peer manager UDP_SERVER last-sender). Trust iperf_srv_*.log if the client
# "Server Report" line looks wrong.
#
# Requires: iperf (classic iperf2) on PATH — not iperf3.
#
# Usage:
#   ./scripts/manager_iperf_bw_test.sh
#   ./scripts/manager_iperf_bw_test.sh --a 192.168.127.181 --b 192.168.128.119
#   ./scripts/manager_iperf_bw_test.sh --dir ab --time 20 --bitrate 8M
#   ./scripts/manager_iperf_bw_test.sh --dir both --no-cca
#   ./scripts/manager_iperf_bw_test.sh 192.168.253.11 192.168.253.12 192.168.253.106
#   ./scripts/manager_iperf_bw_test.sh -- -l 1400
#   ./scripts/manager_iperf_bw_test.sh --scenario realtek --bitrate 25M

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=ensure_manager.sh
source "$ROOT/scripts/ensure_manager.sh"
# shellcheck source=realtek_radios.sh
source "$ROOT/scripts/realtek_radios.sh"
SCENARIO="esp32"
RADIO_SET=0
LOG_DIR="${TMPDIR:-/tmp}/winject-iperf-$$"
mkdir -p "$LOG_DIR"

RADIO_A="192.168.253.11"
RADIO_B="192.168.253.12"
HOST_IP="192.168.253.106"
HOST_SET=0
# Radio PHY written into the manager configs; empty = keep the config's value
# (CCA: keep the radio's).
CHANNEL=""
MODULATION=""
POWER=""
CCA=""

# Ports must match configuration/winject-tests/<scenario>/bw_{a,b}.cfg
PORT_SEND_AB=29000   # manager A UDP_SERVER (client connects here for A→B)
PORT_SEND_BA=29001   # manager B UDP_SERVER (client connects here for B→A)
PORT_RECV_AB=9002    # manager B UDP_CLIENT target (iperf server for A→B)
PORT_RECV_BA=9001    # manager A UDP_CLIENT target (iperf server for B→A)

DIR="both"           # ab | ba | both | bidir
TIME=10
BITRATE="20M"        # iperf UDP requires -b; default matches typical bench headroom
INTERVAL=1
IPERF_EXTRA=()
# Largest user UDP payload the manager accepts (k_stream_payload_max).
readonly MAX_STREAM_PAYLOAD=1445

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- iperf-client-args...]

Start both winject-managers and run iperf (v2) in UDP mode over the manager
forward path. Extra args after -- are passed to iperf -c (not -s).

Scenario:
  --scenario S      esp32 | realtek (default: $SCENARIO); configs come from
                    configuration/winject-tests/<S>/. realtek also starts the
                    two local winject-radio-realtek instances (needs sudo)

Radio / host (esp32):
  --a IP            radio A Ethernet IP (default: $RADIO_A)
  --b IP            radio B Ethernet IP (default: $RADIO_B)
  --host IP         host IP radios send upstream_tx to (auto-detect if omitted)
  --no-cca          disable CCA on both radios (not with realtek: no CCA control)
  --cca             enable CCA on both radios (default: keep the radio's)

Radio PHY:
  --channel N       radio channel (default: from bw_{a,b}.cfg)
  --modulation M    radio modulation, e.g. OFDM_24M (default: from bw_{a,b}.cfg)
  --power DBM       radio TX power (default: from bw_{a,b}.cfg)

The managers program the radios (PHY, CCA, domain filter) at startup; the
script never talks to a radio's console directly.

Test selection:
  --dir DIR         ab | ba | both | bidir  (default: both)
                    ab/ba/both = sequential unidirectional runs
                    bidir = iperf -d on the A→B path (simultaneous up/down)
  -t, --time SEC    test duration seconds (default: $TIME)
  -b, --bitrate R   UDP target bitrate (default: $BITRATE)
  -i, --interval SEC reporting interval (default: $INTERVAL)

Examples:
  $(basename "$0") --dir ab --time 15
  $(basename "$0") --dir both --bitrate 6M --no-cca
  $(basename "$0") --dir bidir --time 20
  $(basename "$0") -- -l 1400
  $(basename "$0") --scenario realtek --bitrate 25M
  $(basename "$0") --scenario realtek --modulation OFDM_MCS7_SGI --bitrate 30M
EOF
}

if [[ "${1:-}" == "--" ]]; then
  shift
fi

# Legacy positional IPs: RADIO_A RADIO_B [HOST_IP] [args...]
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
      usage
      exit 0
      ;;
    --)
      shift
      IPERF_EXTRA+=("$@")
      break
      ;;
    --scenario)
      SCENARIO="${2:?--scenario needs esp32|realtek}"
      shift 2
      ;;
    --scenario=*)
      SCENARIO="${1#--scenario=}"
      shift
      ;;
    --a)
      RADIO_A="${2:?--a needs an IP}"
      RADIO_SET=1
      shift 2
      ;;
    --a=*)
      RADIO_A="${1#--a=}"
      RADIO_SET=1
      shift
      ;;
    --b)
      RADIO_B="${2:?--b needs an IP}"
      RADIO_SET=1
      shift 2
      ;;
    --b=*)
      RADIO_B="${1#--b=}"
      RADIO_SET=1
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
    --no-cca)
      CCA=0
      shift
      ;;
    --cca)
      CCA=1
      shift
      ;;
    --channel)
      CHANNEL="${2:?--channel needs a number}"
      shift 2
      ;;
    --channel=*)
      CHANNEL="${1#--channel=}"
      shift
      ;;
    --modulation)
      MODULATION="${2:?--modulation needs a name}"
      shift 2
      ;;
    --modulation=*)
      MODULATION="${1#--modulation=}"
      shift
      ;;
    --power)
      POWER="${2:?--power needs dBm}"
      shift 2
      ;;
    --power=*)
      POWER="${1#--power=}"
      shift
      ;;
    --dir)
      DIR="${2:?--dir needs ab|ba|both|bidir}"
      shift 2
      ;;
    --dir=*)
      DIR="${1#--dir=}"
      shift
      ;;
    -t|--time)
      TIME="${2:?--time needs seconds}"
      shift 2
      ;;
    --time=*)
      TIME="${1#--time=}"
      shift
      ;;
    -b|--bitrate)
      BITRATE="${2:?--bitrate needs a rate}"
      shift 2
      ;;
    --bitrate=*)
      BITRATE="${1#--bitrate=}"
      shift
      ;;
    -i|--interval)
      INTERVAL="${2:?--interval needs seconds}"
      shift 2
      ;;
    --interval=*)
      INTERVAL="${1#--interval=}"
      shift
      ;;
    *)
      IPERF_EXTRA+=("$1")
      shift
      ;;
  esac
done

validate_iperf_length() {
  local i=0
  while [[ $i -lt ${#IPERF_EXTRA[@]} ]]; do
    local arg="${IPERF_EXTRA[$i]}"
    local val=""
    if [[ "$arg" == -l ]]; then
      val="${IPERF_EXTRA[$((i + 1))]:?-l needs a length}"
      i=$((i + 2))
    elif [[ "$arg" == -l=* ]]; then
      val="${arg#-l=}"
      i=$((i + 1))
    else
      i=$((i + 1))
      continue
    fi
    if [[ ! "$val" =~ ^[0-9]+$ ]]; then
      echo "error: iperf -l length must be numeric (got: $val)" >&2
      exit 1
    fi
    if (( val > MAX_STREAM_PAYLOAD )); then
      echo "error: iperf -l $val exceeds manager k_stream_payload_max ($MAX_STREAM_PAYLOAD); use -l $MAX_STREAM_PAYLOAD or less" >&2
      exit 1
    fi
  done
}

has_iperf_length=0
for arg in "${IPERF_EXTRA[@]}"; do
  if [[ "$arg" == -l || "$arg" == -l=* ]]; then
    has_iperf_length=1
    break
  fi
done
if [[ $has_iperf_length -eq 0 ]]; then
  IPERF_EXTRA=(-l 1400 "${IPERF_EXTRA[@]}")
fi
validate_iperf_length

case "$SCENARIO" in
  esp32) ;;
  realtek)
    # Both radios run on this host; ports differ per radio (bw_{a,b}.cfg).
    if [[ "$RADIO_SET" -eq 1 ]]; then
      echo "error: --a/--b do not apply to --scenario realtek (radios run on this host)" >&2
      exit 1
    fi
    if [[ "$CCA" == 0 ]]; then
      echo "error: --no-cca is not supported by the Realtek radio" >&2
      exit 1
    fi
    RADIO_A=127.0.0.1
    RADIO_B=127.0.0.1
    HOST_IP=127.0.0.1
    HOST_SET=1
    ;;
  *)
    echo "error: --scenario must be esp32 or realtek (got: $SCENARIO)" >&2
    exit 1
    ;;
esac
CONF_A="$ROOT/configuration/winject-tests/$SCENARIO/bw_a.cfg"
CONF_B="$ROOT/configuration/winject-tests/$SCENARIO/bw_b.cfg"

case "$DIR" in
  ab|ba|both|bidir) ;;
  *)
    echo "error: --dir must be ab, ba, both, or bidir (got: $DIR)" >&2
    exit 1
    ;;
esac

IPERF_BIN=""
if command -v iperf >/dev/null 2>&1; then
  IPERF_BIN=iperf
elif command -v iperf2 >/dev/null 2>&1; then
  IPERF_BIN=iperf2
fi
if [[ -z "$IPERF_BIN" ]]; then
  echo "error: iperf (v2) not found on PATH (install iperf, not iperf3)" >&2
  exit 1
fi
if command -v iperf3 >/dev/null 2>&1 && [[ "$IPERF_BIN" == iperf ]]; then
  if iperf --version 2>/dev/null | grep -qi iperf3; then
    echo "error: 'iperf' on PATH is iperf3; install classic iperf2 (Debian: iperf)" >&2
    exit 1
  fi
fi

if [[ "$HOST_SET" -eq 0 ]]; then
  HOST_IP="$(python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(('$RADIO_A', 22)); print(s.getsockname()[0]); s.close()")"
fi

ensure_winject_manager "$ROOT"

patch_conf() {
  local file="$1" device="$2" gci_in="$3"
  local -a sed_args=(
    -e "s/^winject\.device.*/winject.device        = ${device}/"
  )
  if [[ -n "$CHANNEL" ]]; then
    sed_args+=(-e "s/^winject\.channel.*/winject.channel       = ${CHANNEL}/")
  fi
  if [[ -n "$MODULATION" ]]; then
    sed_args+=(-e "s/^winject\.modulation.*/winject.modulation    = ${MODULATION}/")
  fi
  if [[ -n "$POWER" ]]; then
    sed_args+=(-e "s/^winject\.power.*/winject.power         = ${POWER}/")
  fi
  sed "${sed_args[@]}" "$file"
  if [[ -n "$CCA" ]]; then
    printf 'winject.cca           = %s\n' "$CCA"
  fi
  printf 'manager.console_in    = 127.0.0.1:%s\n' "$gci_in"
}

CONF_A_RUN="$LOG_DIR/winject_a.conf"
CONF_B_RUN="$LOG_DIR/winject_b.conf"
patch_conf "$CONF_A" "$RADIO_A" 2400 >"$CONF_A_RUN"
patch_conf "$CONF_B" "$RADIO_B" 2410 >"$CONF_B_RUN"

radio_snapshot() {
  local tag="$1"
  # Through the managers' m-plane (console ports set in patch_conf).
  python3 "$ROOT/tools/radio_stats.py" \
    --mgr-a 127.0.0.1:2400 \
    --mgr-b 127.0.0.1:2410 \
    --save "$LOG_DIR/radio_${tag}.json" || true
}

radio_diff() {
  local tag="$1"
  local before="$LOG_DIR/radio_${tag}_before.json"
  local after="$LOG_DIR/radio_${tag}_after.json"
  if [[ -f "$before" && -f "$after" ]]; then
    python3 "$ROOT/tools/radio_stats.py" --diff "$before" "$after" --dir "$tag" || true
  fi
}

PID_A=""
PID_B=""
PID_IPERF=""

stop_server() {
  if [[ -n "${PID_IPERF:-}" ]]; then
    kill "$PID_IPERF" 2>/dev/null || true
    PID_IPERF=""
    sleep 0.2
  fi
}

cleanup() {
  stop_server
  if [[ -n "${PID_A:-}" ]]; then kill "$PID_A" 2>/dev/null || true; fi
  if [[ -n "${PID_B:-}" ]]; then kill "$PID_B" 2>/dev/null || true; fi
  realtek_radios_stop
}
trap cleanup EXIT INT TERM

# Match the process name only: -f would also match shells whose command line
# mentions winject-manager (and kill the caller).
pkill -x winject-manager 2>/dev/null || true
for p in "$PORT_RECV_AB" "$PORT_RECV_BA" "$PORT_SEND_AB" "$PORT_SEND_BA"; do
  fuser -k "${p}/udp" 2>/dev/null || true
done
sleep 1

if [[ "$SCENARIO" == realtek ]]; then
  realtek_radios_start "$ROOT" "$LOG_DIR"
fi

echo "managers [$SCENARIO]: A=$RADIO_A B=$RADIO_B host=$HOST_IP logs=$LOG_DIR"
"$MANAGER" "$CONF_A_RUN" >"$LOG_DIR/manager_a.log" 2>&1 &
PID_A=$!
"$MANAGER" "$CONF_B_RUN" >"$LOG_DIR/manager_b.log" 2>&1 &
PID_B=$!

echo "waiting for managers..."
for _ in $(seq 1 20); do
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

wait_udp_listen() {
  local port="$1" deadline=$((SECONDS + 5))
  while (( SECONDS < deadline )); do
    if ss -lun 2>/dev/null | grep -qE ":${port}\\s"; then
      return 0
    fi
    sleep 0.1
  done
  echo "error: no UDP listener on 127.0.0.1:$port" >&2
  return 1
}

build_client_args() {
  local -a args=(-u -t "$TIME" -i "$INTERVAL" -b "$BITRATE")
  if [[ ${#IPERF_EXTRA[@]} -gt 0 ]]; then
    args+=("${IPERF_EXTRA[@]}")
  fi
  printf '%s\n' "${args[@]}"
}

start_server() {
  local tag="$1" port="$2"
  stop_server
  sleep 0.3
  "$IPERF_BIN" -s -u -B 127.0.0.1 -p "$port" >"$LOG_DIR/iperf_srv_${tag}.log" 2>&1 &
  PID_IPERF=$!
  wait_udp_listen "$port"
}

print_server_summary() {
  local tag="$1"
  local log="$LOG_DIR/iperf_srv_${tag}.log"
  if [[ ! -f "$log" ]]; then
    return
  fi
  local line
  line="$(grep -E '^\[[[:space:]]*[0-9]+\][[:space:]]+0\.0\.0-' "$log" | tail -1 || true)"
  if [[ -n "$line" ]]; then
    echo "iperf server summary ($tag): $line"
  else
    echo "iperf server summary ($tag): (see $log)"
    tail -5 "$log" || true
  fi
}

run_client() {
  local label="$1" client_port="$2" server_port="$3" tag="$4"
  shift 4
  local -a client_args=("$@")
  start_server "$tag" "$server_port"
  echo
  echo "========== iperf UDP $label =========="
  echo "client -c 127.0.0.1 -p $client_port → manager → iperf -s -u -p $server_port"
  radio_snapshot "${tag}_before"
  set +e
  "$IPERF_BIN" -c 127.0.0.1 -p "$client_port" "${client_args[@]}" \
    | tee "$LOG_DIR/iperf_cli_${tag}.log"
  local rc=${PIPESTATUS[0]}
  set -e
  radio_snapshot "${tag}_after"
  radio_diff "$tag"
  print_server_summary "$tag"
  if [[ "$rc" -ne 0 ]]; then
    echo "iperf client failed (rc=$rc)"
    echo "--- server ---"
    tail -30 "$LOG_DIR/iperf_srv_${tag}.log" || true
  fi
  stop_server
  sleep 0.3
  return "$rc"
}

FAIL=0
mapfile -t CLIENT_ARGS < <(build_client_args)

if [[ "$DIR" == "ab" || "$DIR" == "both" ]]; then
  if ! run_client "A→B" "$PORT_SEND_AB" "$PORT_RECV_AB" ab "${CLIENT_ARGS[@]}"; then
    FAIL=1
  fi
fi

if [[ "$DIR" == "ba" || "$DIR" == "both" ]]; then
  if ! run_client "B→A" "$PORT_SEND_BA" "$PORT_RECV_BA" ba "${CLIENT_ARGS[@]}"; then
    FAIL=1
  fi
fi

if [[ "$DIR" == "bidir" ]]; then
  start_server bidir "$PORT_RECV_AB"
  BIDIR_ARGS=("${CLIENT_ARGS[@]}" -d)
  echo
  echo "========== iperf UDP A↔B bidir (-d) =========="
  echo "client -c 127.0.0.1 -p $PORT_SEND_AB → manager → iperf -s -u -p $PORT_RECV_AB"
  set +e
  "$IPERF_BIN" -c 127.0.0.1 -p "$PORT_SEND_AB" "${BIDIR_ARGS[@]}" \
    | tee "$LOG_DIR/iperf_cli_bidir.log"
  rc=${PIPESTATUS[0]}
  set -e
  if [[ "$rc" -ne 0 ]]; then
    echo "iperf client failed (rc=$rc)"
    FAIL=1
  fi
  stop_server
fi

echo
echo "=== iperf done (logs $LOG_DIR) ==="
exit "$FAIL"
