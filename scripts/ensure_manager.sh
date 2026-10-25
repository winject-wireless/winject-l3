#!/usr/bin/env bash
# Build winject-manager from this winject-l3 checkout so tests run the latest
# source. Incremental: a no-op when the build tree is already up to date.
# Usage: ensure_winject_manager <repo_root> [build_manager_arm|build_manager_x86]
# repo_root is the winject-l3 checkout.
# WINJECT_MANAGER=<binary> skips the build and uses that binary as-is.
# WINJECT_MANAGER_BUILD=<dir> overrides the build tree.
ensure_winject_manager() {
    local root="${1:?winject-l3 root required}"
    local tree="${2:-}"

    if [[ -z "$tree" ]]; then
        case "$(uname -m)" in
            aarch64|arm64) tree=build_manager_arm ;;
            *)             tree=build_manager_x86 ;;
        esac
    fi

    MANAGER_BUILD="${WINJECT_MANAGER_BUILD:-$root/$tree}"
    MANAGER="${WINJECT_MANAGER:-$MANAGER_BUILD/winject-manager}"

    _manager_matches_host() {
        [[ -x "$MANAGER" ]] || return 1
        case "$(uname -m)" in
            aarch64|arm64)
                file "$MANAGER" | grep -qE 'ARM aarch64|aarch64' ;;
            x86_64|amd64)
                file "$MANAGER" | grep -qE 'x86-64|80386' ;;
            *)
                return 1 ;;
        esac
    }

    if [[ -n "${WINJECT_MANAGER:-}" ]]; then
        if ! _manager_matches_host; then
            echo "error: WINJECT_MANAGER=$MANAGER is not an executable for $(uname -m)" >&2
            return 1
        fi
        echo "using WINJECT_MANAGER=$MANAGER (not rebuilt)" >&2
        return 0
    fi

    echo "building winject-manager in $MANAGER_BUILD..." >&2
    if [[ ! -f "$MANAGER_BUILD/CMakeCache.txt" ]]; then
        cmake -S "$root/src/manager" -B "$MANAGER_BUILD" -DCMAKE_BUILD_TYPE=Release || return 1
    fi
    cmake --build "$MANAGER_BUILD" -j"$(nproc)" || return 1

    if ! _manager_matches_host; then
        echo "error: $MANAGER is not built for $(uname -m)" >&2
        return 1
    fi
}

# SIGTERM every winject-manager and wait until it has exited. The process
# closes its radio m-plane session on the way out. This does not send
# anything to a radio.
stop_winject_managers() {
    local timeout_s="${1:-15}"
    local i
    if ! pgrep -x winject-manager >/dev/null 2>&1; then
        return 0
    fi
    echo "stopping winject-manager..." >&2
    pkill -TERM -x winject-manager 2>/dev/null || true
    for i in $(seq 1 $((timeout_s * 10))); do
        if ! pgrep -x winject-manager >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "error: winject-manager did not exit within ${timeout_s}s" >&2
    pgrep -a -x winject-manager >&2 || true
    return 1
}

# Block until the manager log shows it programmed the radio, which is when
# that process holds the m-plane.
wait_manager_radio() {
    local log="${1:?manager log required}"
    local timeout_s="${2:-30}"
    local i
    for i in $(seq 1 $((timeout_s * 4))); do
        if [[ -f "$log" ]] && grep -q "radio programmed" "$log"; then
            return 0
        fi
        sleep 0.25
    done
    echo "error: manager did not acquire the radio; tail $log" >&2
    if [[ -f "$log" ]]; then
        tail -30 "$log" >&2 || true
    fi
    return 1
}
