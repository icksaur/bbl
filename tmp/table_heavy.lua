-- table_heavy.lua — table insert and lookup at scale
-- Equivalent to table_heavy.bbl
local t = {}
local i = 0
while i < 1000 do
    t[i] = i * i
    i = i + 1
end
-- Read back all entries
i = 0
local sum = 0
while i < 1000 do
    sum = sum + t[i]
    i = i + 1
end
print(sum)
