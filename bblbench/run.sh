#!/usr/bin/env bash
# bblbench runner — verifies BBL scripts produce correct deterministic output
# Usage: ./bblbench/run.sh [path-to-bbl-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BBL="${1:-./build/bbl}"

if [[ ! -x "$BBL" ]]; then
    echo "ERROR: bbl binary not found at $BBL"
    echo "Usage: $0 [path-to-bbl]"
    exit 1
fi

PASS=0
FAIL=0
TOTAL=0

check() {
    local name="$1"
    local script="$2"
    local expected_stdout="$3"
    local expected_file="${4:-}"    # optional: path to expected file output
    local actual_file="${5:-}"      # optional: path to generated file

    TOTAL=$((TOTAL + 1))

    # Run script, capture stdout
    local actual
    if ! actual=$("$BBL" "$script" 2>/dev/null); then
        echo "  FAIL  $name  (script crashed)"
        FAIL=$((FAIL + 1))
        return
    fi

    # Compare stdout
    local expected
    expected=$(cat "$expected_stdout")
    if [[ "$actual" != "$expected" ]]; then
        echo "  FAIL  $name  (stdout mismatch)"
        echo "    expected sha256: $(echo -n "$expected" | sha256sum | cut -d' ' -f1)"
        echo "    actual sha256:   $(echo -n "$actual" | sha256sum | cut -d' ' -f1)"
        FAIL=$((FAIL + 1))
        return
    fi

    # If there's a file to verify, check it too
    if [[ -n "$expected_file" && -n "$actual_file" ]]; then
        if [[ ! -f "$actual_file" ]]; then
            echo "  FAIL  $name  (output file not created: $actual_file)"
            FAIL=$((FAIL + 1))
            return
        fi
        local exp_hash act_hash
        exp_hash=$(sha256sum "$expected_file" | cut -d' ' -f1)
        act_hash=$(sha256sum "$actual_file" | cut -d' ' -f1)
        if [[ "$exp_hash" != "$act_hash" ]]; then
            echo "  FAIL  $name  (file checksum mismatch)"
            echo "    expected: $exp_hash"
            echo "    actual:   $act_hash"
            FAIL=$((FAIL + 1))
            return
        fi
    fi

    echo "  PASS  $name"
    PASS=$((PASS + 1))
}

echo "bblbench — BBL programming benchmark suite"
echo "============================================"
echo ""

# Clean any previous output
rm -f /tmp/bblbench_mult.txt

# 1. File generation: multiplication table (1KB+ file + stdout)
check "file_gen" \
    "$SCRIPT_DIR/1_file_gen.bbl" \
    "$SCRIPT_DIR/expected/1_file_gen.stdout" \
    "$SCRIPT_DIR/expected/1_file_gen.file" \
    "/tmp/bblbench_mult.txt"

# 2. Prime sieve
check "primes" \
    "$SCRIPT_DIR/2_primes.bbl" \
    "$SCRIPT_DIR/expected/2_primes.stdout"

# 3. Insertion sort
check "sort" \
    "$SCRIPT_DIR/3_sort.bbl" \
    "$SCRIPT_DIR/expected/3_sort.stdout"

# 4. Collatz conjecture
check "collatz" \
    "$SCRIPT_DIR/4_collatz.bbl" \
    "$SCRIPT_DIR/expected/4_collatz.stdout"

# 5. Closures & higher-order functions
check "closure" \
    "$SCRIPT_DIR/5_closure.bbl" \
    "$SCRIPT_DIR/expected/5_closure.stdout"

echo ""
echo "Passed: $PASS  Failed: $FAIL  Total: $TOTAL"
[[ $FAIL -eq 0 ]] || exit 1
