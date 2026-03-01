-- loop_arith_5m.lua — 5M iterations for profiling comparison
local n = 5000000
local sum = 0
local i = 0
while i < n do
    sum = sum + i
    i = i + 1
end
print(sum)
