#!/usr/bin/env bash
# Format check + winject-tests (CTest). Run from repo root.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found" >&2
    exit 1
fi

collect_sources() {
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git ls-files 'src/manager/*.[ch]' 'src/manager/**/*.[ch]' \
            'src/test/*.[ch]' 'src/test/**/*.[ch]' \
            'src/bfcext/*.[ch]' 'src/bfcext/**/*.[ch]' 2>/dev/null || true
    else
        find src/manager src/test src/bfcext \
            \( -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name '*.hpp' \) \
            2>/dev/null | sort
    fi
}

should_skip() {
    local f="$1"
    case "$f" in
        src/manager/fec/IsalEcNeon.c) return 0 ;;
    esac
    return 1
}

echo "== clang-format check =="
fail=0
while IFS= read -r f; do
    [[ -z "$f" ]] && continue
    [[ -f "$f" ]] || continue
    if should_skip "$f"; then
        continue
    fi
    if ! clang-format --dry-run --Werror "$f" 2>/dev/null; then
        echo "format check failed: $f" >&2
        fail=1
    fi
done < <(collect_sources)

if [[ "$fail" -ne 0 ]]; then
    echo "Run: clang-format -i on listed files" >&2
    exit 1
fi

BUILD_DIR="${WINJECT_TEST_BUILD_DIR:-build_test_ci}"
echo "== configure/build tests ($BUILD_DIR) =="
cmake -S src/test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "== ctest =="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo "check_guidelines: OK"
