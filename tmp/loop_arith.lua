-- loop_arith.lua — tight arithmetic loop
-- Equivalent to loop_arith.bbl
local n = 1000000
local sum = 0
local i = 0
while i < n do
    sum = sum + i
    i = i + 1
end
print(sum)
