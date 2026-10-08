#!/usr/bin/env bash
# Start and stop the two local winject-radio-realtek instances used by the
# realtek bench scenario (configuration/winject-tests/realtek/radio_{a,b}.cfg).
# Source this file, like ensure_manager.sh.
#
# Usage:
#   realtek_radios_start <repo_root> <log_dir>
#   realtek_radios_stop
# repo_root is the winject-l3 checkout.
# WINJECT_RADIO_REALTEK=<dir> is the winject-radio-realtek checkout
#   (default <repo_root>/../winject-radio-realtek); it is built incrementally
#   and supplies configuration/txpower.csv.
# WINJECT_RADIO_REALTEK_BIN=<binary> skips the build and uses that binary.
#
# The radios need root (monitor mode, raw sockets), so they run under sudo.
# realtek_radios_stop hands the dongles back to NetworkManager in managed mode.

REALTEK_IFACES=()
REALTEK_STARTED=0
# Set by realtek_cross_radios_start (ssh target to stop radio B on exit).
REALTEK_CROSS_B_SSH=""
REALTEK_CROSS_B_LOG=""

# Stock 88XXau binds as rtl88XXau; winject-radio-realtek needs rtl88xxau_wfb (svpcom).
realtek_ensure_wfb_driver() {
    if ! sudo modprobe 88XXau_wfb; then
        echo "error: modprobe 88XXau_wfb failed (install svpcom rtl8812au DKMS)" >&2
        return 1
    fi
    local stock=/sys/bus/usb/drivers/rtl88XXau
    local wfb=/sys/bus/usb/drivers/rtl88xxau_wfb
    if [[ ! -d "$wfb" ]]; then
        echo "error: USB driver rtl88xxau_wfb not registered" >&2
        return 1
    fi
    if [[ ! -d "$stock" ]]; then
        return 0
    fi
    local dev id ifn
    for dev in "$stock"/*:*; do
        [[ -e "$dev" ]] || continue
        id="$(basename "$dev")"
        if [[ -d "$dev/net" ]]; then
            for ifn in "$dev/net"/*; do
                [[ -e "$ifn" ]] || continue
                ifn="$(basename "$ifn")"
                sudo nmcli device set "$ifn" managed no 2>/dev/null || true
                sudo ip link set "$ifn" down 2>/dev/null || true
            done
        fi
        if ! echo "$id" | sudo tee "$stock/unbind" >/dev/null; then
            echo "error: unbind $id from rtl88XXau failed" >&2
            return 1
        fi
        if ! echo "$id" | sudo tee "$wfb/bind" >/dev/null; then
            echo "error: bind $id to rtl88xxau_wfb failed" >&2
            return 1
        fi
    done
    sudo modprobe -r 88XXau 2>/dev/null || true
}

realtek_resolve_paths() {
    local root="${1:?winject-l3 root required}"
    REALTEK_ROOT="${WINJECT_RADIO_REALTEK:-$root/../winject-radio-realtek}"
    REALTEK_BIN="${WINJECT_RADIO_REALTEK_BIN:-$REALTEK_ROOT/build/src/radio/winject-radio-realtek}"
    REALTEK_TXPOWER="$REALTEK_ROOT/configuration/txpower.csv"
}

realtek_build_binary() {
    local root="${1:?winject-l3 root required}"
    realtek_resolve_paths "$root"
    if [[ -n "${WINJECT_RADIO_REALTEK_BIN:-}" ]]; then
        return 0
    fi
    if [[ ! -f "$REALTEK_ROOT/CMakeLists.txt" ]]; then
        echo "error: winject-radio-realtek not found at $REALTEK_ROOT (set WINJECT_RADIO_REALTEK)" >&2
        return 1
    fi
    echo "building winject-radio-realtek in $REALTEK_ROOT/build..." >&2
    if [[ ! -f "$REALTEK_ROOT/build/CMakeCache.txt" ]]; then
        cmake -S "$REALTEK_ROOT" -B "$REALTEK_ROOT/build" -DCMAKE_BUILD_TYPE=Release || return 1
    fi
    cmake --build "$REALTEK_ROOT/build" -j"$(nproc)" --target winject-radio-realtek || return 1
}

realtek_wait_radio_log() {
    local log_file="$1" label="$2"
    local ifname="" _
    for _ in $(seq 1 60); do
        ifname="$(sed -nE 's/.*\| INF \| ([^ :]+): channel=.*/\1/p' "$log_file" | head -1)"
        if [[ -n "$ifname" ]] || grep -q "| ERR |" "$log_file"; then
            break
        fi
        sleep 0.25
    done
    if [[ -z "$ifname" ]]; then
        echo "realtek radio $label failed to start; tail $log_file" >&2
        tail -20 "$log_file" >&2
        return 1
    fi
    echo "realtek radio $label up on $ifname"
    REALTEK_IFACES+=("$ifname")
}

realtek_start_local_radio() {
    local which="$1" conf_src="$2" conf_run="$3" log_file="$4"
    cp "$conf_src" "$conf_run"
    printf 'radio.txpower    = %s\n' "$REALTEK_TXPOWER" >>"$conf_run"
    sudo rm -rf "/tmp/winject-radio-$which"
    sudo bash -c "'$REALTEK_BIN' --config '$conf_run' >'$log_file' 2>&1 &"
    realtek_wait_radio_log "$log_file" "$which"
}

# Radio A on this host, radio B on a peer (SSH). Managers on this host use
# winject.device 127.0.0.1 (--a) and the remote LAN IP (--b).
realtek_cross_radios_start() {
    local root="${1:?winject-l3 root required}"
    local log_dir="${2:?log dir required}"
    local radio_b_ip="${3:?radio B LAN IP required}"
    local radio_b_ssh="${4:-ubuntu@${radio_b_ip}}"
    local conf_dir="$root/configuration/winject-tests/realtek"
    local remote_cfg="/tmp/winject-radio-b-cross.cfg"
    local remote_log="/tmp/winject-radio-b-cross.log"

    realtek_build_binary "$root" || return 1
    if [[ ! -x "$REALTEK_BIN" ]]; then
        echo "error: $REALTEK_BIN is not executable" >&2
        return 1
    fi
    if [[ ! -f "$REALTEK_TXPOWER" ]]; then
        echo "error: $REALTEK_TXPOWER not found" >&2
        return 1
    fi
    realtek_ensure_wfb_driver || return 1

    REALTEK_STARTED=1
    REALTEK_CROSS_B_SSH="$radio_b_ssh"
    REALTEK_CROSS_B_LOG="$remote_log"

    realtek_start_local_radio a "$conf_dir/radio_a.cfg" "$log_dir/radio_a.cfg" \
        "$log_dir/radio_a.log" || return 1

    ssh -o BatchMode=yes "$radio_b_ssh" 'command -v cmake >/dev/null' 2>/dev/null || {
        echo "error: ssh to $radio_b_ssh failed (BatchMode); set --radio-b-ssh user@host" >&2
        return 1
    }
    scp -q "$conf_dir/radio_b.cfg" "${radio_b_ssh}:${remote_cfg}.in"
    ssh "$radio_b_ssh" "set -euo pipefail
      radio_root=\"\${WINJECT_RADIO_REALTEK:-$REALTEK_ROOT}\"
      bin=\"\${WINJECT_RADIO_REALTEK_BIN:-\$radio_root/build/src/radio/winject-radio-realtek}\"
      if [[ ! -x \"\$bin\" ]]; then
        cmake -S \"\$radio_root\" -B \"\$radio_root/build\" -DCMAKE_BUILD_TYPE=Release
        cmake --build \"\$radio_root/build\" -j\$(nproc) --target winject-radio-realtek
      fi
      sudo modprobe 88XXau_wfb 2>/dev/null || true
      sudo pkill -TERM -x winject-radio-r 2>/dev/null || true
      sleep 0.5
      sed 's/^net\\.bind.*/net.bind         = 0.0.0.0/' ${remote_cfg}.in > ${remote_cfg}
      echo 'radio.txpower    = '\$radio_root'/configuration/txpower.csv' >> ${remote_cfg}
      sudo rm -rf /tmp/winject-radio-b
      sudo \"\$bin\" --config ${remote_cfg} >${remote_log} 2>&1 &
    " || return 1

    local ifname_b="" _
    for _ in $(seq 1 60); do
        ifname_b="$(ssh "$radio_b_ssh" "sed -nE 's/.*\\| INF \\| ([^ :]+): channel=.*/\\1/p' ${remote_log} | head -1" 2>/dev/null || true)"
        if [[ -n "$ifname_b" ]]; then
            break
        fi
        sleep 0.25
    done
    if [[ -z "$ifname_b" ]]; then
        echo "realtek radio b failed on $radio_b_ssh; remote log:" >&2
        ssh "$radio_b_ssh" "tail -20 ${remote_log}" >&2 || true
        return 1
    fi
    echo "realtek radio b up on $ifname_b ($radio_b_ip)"
    scp -q "${radio_b_ssh}:${remote_log}" "$log_dir/radio_b.log" 2>/dev/null || true
}

realtek_radios_start() {
    local root="${1:?winject-l3 root required}"
    local log_dir="${2:?log dir required}"
    local conf_dir="$root/configuration/winject-tests/realtek"

    realtek_build_binary "$root" || return 1
    if [[ ! -x "$REALTEK_BIN" ]]; then
        echo "error: $REALTEK_BIN is not executable" >&2
        return 1
    fi
    if [[ ! -f "$REALTEK_TXPOWER" ]]; then
        echo "error: $REALTEK_TXPOWER not found" >&2
        return 1
    fi
    if pgrep -x winject-radio-r >/dev/null; then
        echo "error: winject-radio-realtek already running" >&2
        return 1
    fi

    realtek_ensure_wfb_driver || return 1

    REALTEK_STARTED=1
    local r
    for r in a b; do
        realtek_start_local_radio "$r" "$conf_dir/radio_${r}.cfg" \
            "$log_dir/radio_${r}.cfg" "$log_dir/radio_${r}.log" || return 1
    done
}

realtek_radios_stop() {
    if [[ -n "${REALTEK_CROSS_B_SSH:-}" ]]; then
        ssh -o ConnectTimeout=5 "${REALTEK_CROSS_B_SSH}" \
            'sudo pkill -TERM -x winject-radio-r 2>/dev/null || true' || true
        REALTEK_CROSS_B_SSH=""
        REALTEK_CROSS_B_LOG=""
    fi
    if [[ "$REALTEK_STARTED" -eq 0 ]]; then
        return 0
    fi
    REALTEK_STARTED=0
    sudo pkill -TERM -x winject-radio-r 2>/dev/null || true
    sleep 1
    local i _
    for i in "${REALTEK_IFACES[@]}"; do
        sudo ip link set "$i" down || true
        for _ in 1 2 3; do
            sudo iw dev "$i" set type managed 2>/dev/null && break
            sleep 0.5
        done
        sudo ip link set "$i" up || true
        sudo nmcli device set "$i" managed yes || true
    done
    REALTEK_IFACES=()
}
