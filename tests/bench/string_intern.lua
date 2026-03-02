local i = 0
local s = ""
while i < 100000 do
    s = s .. "x"
    i = i + 1
end
print(#s)
