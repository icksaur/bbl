-- string_intern.lua — string creation at scale
-- Equivalent to string_intern.bbl
local i = 0
local s = ""
while i < 10000 do
    s = s .. "x"
    i = i + 1
end
print(#s)
