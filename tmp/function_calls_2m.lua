-- function_calls_2m.lua — 2M iterations for profiling comparison
local function add(a, b)
    return a + b
end
local i = 0
local sum = 0
while i < 2000000 do
    sum = add(sum, i)
    i = i + 1
end
print(sum)
