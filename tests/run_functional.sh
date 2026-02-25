#!/usr/bin/env bash
set -e

BBL="${1:-./build/bbl}"
DIR="$(cd "$(dirname "$0")/functional" && pwd)"
PASS=0
FAIL=0

check() {
    local name="$1"
    local expected="$2"
    local actual
    actual=$("$BBL" "$DIR/$name.bbl" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        echo "    expected: '$expected'"
        echo "    got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== functional tests ==="
check hello       "Hello, world!"
check arithmetic  "5 20 7 5"
check functions   "42"
check if_loop     "5 yes"
check exec_string "30"
check structs     "structs need C++ registration - skip"
check vectors     "3 4 10 40"
check tables      "hero 100 80 3"
check strings     "true abc 5"
check math        "2 7 256"
check file_io     "hello from bbl"
check binary_data "5"

echo ""
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ]
