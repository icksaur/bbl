#!/bin/bash
# Functional test for child-state (web worker) threading
set -euo pipefail
BBL="${1:-./build/bbl}"
DIR="$(cd "$(dirname "$0")" && pwd)"
WORKER_DIR="/tmp/bbl_func_workers"
mkdir -p "$WORKER_DIR"
PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual
    actual=$("$BBL" "$DIR/functional/workers/${name}.bbl" 2>/dev/null)
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

# Create worker scripts
cat > "$WORKER_DIR/echo.bbl" << 'EOF'
(= msg (recv))
(post (table "reply" (msg:get "data")))
EOF

cat > "$WORKER_DIR/adder.bbl" << 'EOF'
(= msg (recv))
(= a (msg:get "a"))
(= b (msg:get "b"))
(post (table "sum" (+ a b)))
EOF

cat > "$WORKER_DIR/counter.bbl" << 'EOF'
(= msg (recv))
(= n (msg:get "n"))
(= i 0)
(= sum 0)
(loop (< i n)
    (= sum (+ sum i))
    (= i (+ i 1)))
(post (table "result" sum))
EOF

echo "=== worker tests ==="
check worker_echo "hello"
check worker_roundtrip "150"
check worker_join "done"

echo ""
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
