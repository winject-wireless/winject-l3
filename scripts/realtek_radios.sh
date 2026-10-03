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

realtek_radios_start() {
    local root="${1:?winject-l3 root required}"
    local log_dir="${2:?log dir required}"
    local radio_root="${WINJECT_RADIO_REALTEK:-$root/../winject-radio-realtek}"
    local bin="${WINJECT_RADIO_REALTEK_BIN:-$radio_root/build/src/radio/winject-radio-realtek}"
    local txpower="$radio_root/configuration/txpower.csv"
    local conf_dir="$root/configuration/winject-tests/realtek"

    if [[ -z "${WINJECT_RADIO_REALTEK_BIN:-}" ]]; then
        if [[ ! -f "$radio_root/CMakeLists.txt" ]]; then
            echo "error: winject-radio-realtek not found at $radio_root (set WINJECT_RADIO_REALTEK)" >&2
            return 1
        fi
        echo "building winject-radio-realtek in $radio_root/build..." >&2
        if [[ ! -f "$radio_root/build/CMakeCache.txt" ]]; then
            cmake -S "$radio_root" -B "$radio_root/build" -DCMAKE_BUILD_TYPE=Release || return 1
        fi
        cmake --build "$radio_root/build" -j"$(nproc)" --target winject-radio-realtek || return 1
    fi
    if [[ ! -x "$bin" ]]; then
        echo "error: $bin is not executable" >&2
        return 1
    fi
    if [[ ! -f "$txpower" ]]; then
        echo "error: $txpower not found" >&2
        return 1
    fi
    # The process name is truncated to 15 characters.
    if pgrep -x winject-radio-r >/dev/null; then
        echo "error: winject-radio-realtek already running" >&2
        return 1
    fi

    realtek_ensure_wfb_driver || return 1

    REALTEK_STARTED=1
    local r
    for r in a b; do
        local conf="$log_dir/radio_$r.cfg"
        cp "$conf_dir/radio_$r.cfg" "$conf"
        printf 'radio.txpower    = %s\n' "$txpower" >>"$conf"
        sudo rm -rf "/tmp/winject-radio-$r"
        sudo bash -c "'$bin' --config '$conf' >'$log_dir/radio_$r.log' 2>&1 &"
    done

    # Bring-up logs "<ifname>: channel=... tx_power=... modulation=..." when
    # the radio is ready (monitor mode can take a few retries).
    for r in a b; do
        local ifname="" _
        for _ in $(seq 1 60); do
            ifname="$(sed -nE 's/.*\| INF \| ([^ :]+): channel=.*/\1/p' "$log_dir/radio_$r.log" | head -1)"
            if [[ -n "$ifname" ]] || grep -q "| ERR |" "$log_dir/radio_$r.log"; then
                break
            fi
            sleep 0.25
        done
        if [[ -z "$ifname" ]]; then
            echo "realtek radio $r failed to start; tail $log_dir/radio_$r.log" >&2
            tail -20 "$log_dir/radio_$r.log" >&2
            return 1
        fi
        REALTEK_IFACES+=("$ifname")
        echo "realtek radio $r up on $ifname"
    done
}

realtek_radios_stop() {
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
