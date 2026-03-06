#!/bin/bash
set -euo pipefail
BBL="${1:-./build/bbl}"
PASS=0
FAIL=0

lsp_request() {
    local json="$1"
    printf "Content-Length: %d\r\n\r\n%s" ${#json} "$json"
}

run_lsp() {
    timeout 5 "$BBL" --lsp
}

check() {
    local name="$1" input="$2" expected="$3"
    local output
    output=$(echo -n "$input" | run_lsp 2>/dev/null || true)
    if echo "$output" | grep -q "$expected"; then
        echo "  PASS  $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name"
        echo "    expected to find: $expected"
        echo "    got: $(echo "$output" | head -c 200)"
        FAIL=$((FAIL + 1))
    fi
}

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
SHUT='{"jsonrpc":"2.0","id":99,"method":"shutdown"}'
EXIT='{"jsonrpc":"2.0","method":"exit"}'

echo "=== LSP tests ==="

# Test 1: Initialize returns capabilities
INPUT="$(lsp_request "$INIT")$(lsp_request "$EXIT")"
check "initialize" "$INPUT" "completionProvider"

# Test 2: Parse error diagnostics
OPEN='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t.bbl","languageId":"bbl","version":1,"text":"(print"}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "diagnostics" "$INPUT" "expected.*)"

# Test 3: Clean file has empty diagnostics
OPEN_OK='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t.bbl","languageId":"bbl","version":1,"text":"(print 1)"}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_OK")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "no_errors" "$INPUT" '"diagnostics":\[\]'

# Test 4: Keyword completions after (
COMP='{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///t.bbl"},"position":{"line":0,"character":1}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_OK")$(lsp_request "$COMP")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "keyword_completion" "$INPUT" '"label":"if"'

# Test 5: Hover on builtin
HOVER='{"jsonrpc":"2.0","id":4,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///t.bbl"},"position":{"line":0,"character":1}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_OK")$(lsp_request "$HOVER")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "hover_builtin" "$INPUT" "print.*output"

# Test 6: Table key completions after :
OPEN_TBL='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t2.bbl","languageId":"bbl","version":1,"text":"(= p (table \"name\" \"hero\" \"hp\" 100))\n(p:length)"}}}'
COMP_TBL='{"jsonrpc":"2.0","id":5,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///t2.bbl"},"position":{"line":1,"character":3}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_TBL")$(lsp_request "$COMP_TBL")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "table_method_completion" "$INPUT" '"label":"length"'

# Test 7: Table dot completions show keys
OPEN_DOT='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t3.bbl","languageId":"bbl","version":1,"text":"(= p (table \"name\" \"hero\" \"hp\" 100))\n(print p.name)"}}}'
COMP_DOT='{"jsonrpc":"2.0","id":6,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///t3.bbl"},"position":{"line":1,"character":9}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_DOT")$(lsp_request "$COMP_DOT")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "table_dot_completion" "$INPUT" '"label":"name"'

# Test 8: Hover shows table info
HOVER_TBL='{"jsonrpc":"2.0","id":7,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///t2.bbl"},"position":{"line":0,"character":4}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_TBL")$(lsp_request "$HOVER_TBL")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "hover_table" "$INPUT" "table.*entries"

# Test 9: Function hover shows arity
OPEN_FN='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t4.bbl","languageId":"bbl","version":1,"text":"(= greet (fn (name age) (print name age)))\n(greet \"hi\" 5)"}}}'
HOVER_FN='{"jsonrpc":"2.0","id":8,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///t4.bbl"},"position":{"line":0,"character":4}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_FN")$(lsp_request "$HOVER_FN")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
check "hover_function" "$INPUT" "fn.*2.*args"

# Test 10: Colon only shows callable keys
OPEN_CALL='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///t5.bbl","languageId":"bbl","version":1,"text":"(= obj (table \"x\" 1 \"go\" (fn (self) 42)))\n(obj:go)"}}}'
COMP_CALL='{"jsonrpc":"2.0","id":9,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///t5.bbl"},"position":{"line":1,"character":5}}}'
INPUT="$(lsp_request "$INIT")$(lsp_request "$OPEN_CALL")$(lsp_request "$COMP_CALL")$(lsp_request "$SHUT")$(lsp_request "$EXIT")"
# Should have "go" but NOT "x" (x is not callable)
output=$(echo -n "$INPUT" | run_lsp 2>/dev/null || true)
if echo "$output" | grep -q '"label":"go"' && ! echo "$output" | grep -q '"label":"x"'; then
    echo "  PASS  colon_callable_only"
    PASS=$((PASS + 1))
else
    echo "  FAIL  colon_callable_only"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ]
