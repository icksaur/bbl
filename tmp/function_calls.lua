-- function_calls.lua — many function calls
-- Equivalent to function_calls.bbl
local function add(a, b)
    return a + b
end
local i = 0
local sum = 0
while i < 500000 do
    sum = add(sum, i)
    i = i + 1
end
print(sum)
