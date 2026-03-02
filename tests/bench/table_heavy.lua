local t = {}
local i = 0
while i < 10000 do
    t[i] = i * i
    i = i + 1
end
i = 0
local sum = 0
while i < 10000 do
    sum = sum + t[i]
    i = i + 1
end
print(sum)
