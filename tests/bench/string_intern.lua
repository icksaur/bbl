local i = 0
local s = ""
while i < 10000 do
    s = s .. "x"
    i = i + 1
end
print(#s)
