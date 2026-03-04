#!/bin/bash
set -euo pipefail

BBL="${1:-./build_rel/bbl}"
LUAJIT="${2:-luajit}"
DOTNET="${3:-}"
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -z "$DOTNET" ]; then
    DOTNET_DIR="$DIR/csharp/BblBench"
    if [ -d "$DOTNET_DIR" ]; then
        dotnet publish -c Release --no-restore -o /tmp/bblbench "$DOTNET_DIR" > /dev/null 2>&1 || true
        [ -x /tmp/bblbench/BblBench ] && DOTNET=/tmp/bblbench/BblBench
    fi
fi

BENCHMARKS="loop_arith function_calls gc_pressure table_heavy recursion method_dispatch string_build string_parse closure_capture"

median3() {
    local cmd="$1"
    local t1 t2 t3
    t1=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | sed 's/.*0m//;s/s//' )
    t2=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | sed 's/.*0m//;s/s//' )
    t3=$( { time eval "$cmd" > /dev/null 2>&1; } 2>&1 | grep real | sed 's/.*0m//;s/s//' )
    echo "$t1 $t2 $t3" | tr ' ' '\n' | sort -n | sed -n '2p'
}

to_ms() { python3 -c "print(int(float('$1')*1000))"; }

ratio_cell() {
    local a=$1 b=$2
    python3 -c "
r = $a / $b
if r < 0.8: print(f'🟢 {r:.1f}')
elif r <= 1.2: print(f'🟡 {r:.1f}')
else: print(f'🔴 {r:.1f}')
"
}

declare -A BBL_MS LUA_MS NET_MS

for bench in $BENCHMARKS; do
    BBL_MS[$bench]=$(to_ms "$(median3 "timeout 30 $BBL $DIR/${bench}.bbl")")

    luaf="$DIR/lua/${bench}.lua"
    if [ -f "$luaf" ]; then
        LUA_MS[$bench]=$(to_ms "$(median3 "timeout 30 $LUAJIT $luaf")")
    else
        LUA_MS[$bench]="-"
    fi

    if [ -n "$DOTNET" ]; then
        NET_MS[$bench]=$(to_ms "$(median3 "timeout 30 $DOTNET $bench")")
    else
        NET_MS[$bench]="-"
    fi
done

echo ""
echo "| Benchmark | BBL | .NET 9 | ratio | LuaJIT | ratio |"
echo "|---|---|---|---|---|---|"
for bench in $BENCHMARKS; do
    bbl=${BBL_MS[$bench]}
    net=${NET_MS[$bench]}
    lua=${LUA_MS[$bench]}

    if [ "$net" != "-" ]; then
        net_ratio=$(ratio_cell "$bbl" "$net")
    else
        net_ratio="-"
    fi

    if [ "$lua" != "-" ]; then
        lua_ratio=$(ratio_cell "$bbl" "$lua")
    else
        lua_ratio="-"
    fi

    echo "| $bench | ${bbl} ms | ${net} ms | $net_ratio | ${lua} ms | $lua_ratio |"
done
echo ""
