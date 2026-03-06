#!/bin/bash
set -euo pipefail
BBL="${1:-./build/bbl}"
PASS=0
FAIL=0

FORMS="+ - * / % = == != < > <= >= if loop each fn do with try exec execfile struct binary vector table sizeof int not and or band bor bxor bnot shl shr"

echo "=== compiler arity fuzz ==="
for form in $FORMS; do
    for n in 0 1 2 3; do
        args=$(printf ' 1%.0s' $(seq 1 $n) 2>/dev/null || true)
        input="($form$args)"
        result=$(timeout 2 "$BBL" -e "$input" 2>&1; echo "EXIT:$?")
        code=$(echo "$result" | grep -oP 'EXIT:\K\d+')
        if [ "$code" = "134" ] || [ "$code" = "139" ]; then
            echo "  CRASH  $input (exit $code)"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
done

echo ""
echo "Passed: $PASS  Crashed: $FAIL"
[ "$FAIL" -eq 0 ]
