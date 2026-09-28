#!/usr/bin/env bash
# Merge manager + test CMake compile_commands.json for clangd at repo root.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MANAGER_DIR="${WINJECT_MANAGER_BUILD_DIR:-build_manager_arm}"
TEST_DIR="${WINJECT_TEST_BUILD_DIR:-build_test_arm}"
OUT="${WINJECT_COMPILE_COMMANDS:-$ROOT/compile_commands.json}"

manager_db="$MANAGER_DIR/compile_commands.json"
test_db="$TEST_DIR/compile_commands.json"

for db in "$manager_db" "$test_db"; do
    if [[ ! -f "$db" ]]; then
        echo "Missing $db — configure builds first, e.g.:" >&2
        echo "  cmake -S src/manager -B $MANAGER_DIR -DCMAKE_BUILD_TYPE=Release" >&2
        echo "  cmake -S src/test -B $TEST_DIR -DCMAKE_BUILD_TYPE=Release" >&2
        exit 1
    fi
done

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required to merge compile_commands.json" >&2
    exit 1
fi

jq -s '
  def by_file: map({key: .file, value: .}) | from_entries;
  (.[0] | by_file) as $mgr |
  (reduce (.[1] | .[]) as $e ($mgr;
    if ($e.file | test("/src/test/")) then .[$e.file] = $e
    elif has($e.file) | not then .[$e.file] = $e
    else .
    end)) as $merged |
  $merged | [.[]] | sort_by(.file)
' "$manager_db" "$test_db" >"$OUT"

echo "Wrote $OUT (manager=$MANAGER_DIR, test=$TEST_DIR)"
