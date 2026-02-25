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

# Phase 6: args test (needs extra arguments)
check_args() {
    local actual
    actual=$("$BBL" "$DIR/args.bbl" hello 2>/dev/null)
    if [ "$actual" = "1 hello" ]; then
        echo "  PASS  args"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  args"
        echo "    expected: '1 hello'"
        echo "    got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}
check_args

# Phase 6: REPL test (pipe input)
check_repl() {
    local actual
    actual=$(echo '(+ 1 2)' | "$BBL" 2>/dev/null)
    # REPL output includes prompts: "> 3\n> "
    if echo "$actual" | grep -q "3"; then
        echo "  PASS  repl"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  repl"
        echo "    expected output to contain '3'"
        echo "    got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}
check_repl

# Phase 6: multiple -e flags
check_multi_e() {
    local actual
    actual=$("$BBL" -e '(def x 10)' -e '(print (* x x))' 2>/dev/null)
    if [ "$actual" = "100" ]; then
        echo "  PASS  multi_e"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  multi_e"
        echo "    expected: '100'"
        echo "    got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}
check_multi_e

# Phase 6: BBL_PATH test
check_bbl_path() {
    local actual
    actual=$(BBL_PATH="$DIR" "$BBL" -e '(execfile "hello.bbl")' 2>/dev/null)
    if [ "$actual" = "Hello, world!" ]; then
        echo "  PASS  bbl_path"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  bbl_path"
        echo "    expected: 'Hello, world!'"
        echo "    got:      '$actual'"
        FAIL=$((FAIL + 1))
    fi
}
check_bbl_path

echo ""
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ]
