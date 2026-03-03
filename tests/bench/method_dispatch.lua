local v = {1, 2, 3, 4, 5}
local t = {a = 1, b = 2, c = 3}
local s = "hello world"
local sum = 0
local keys = {"a", "b", "c"}
for i = 0, 99999 do
    sum = sum + v[(i % 5) + 1]
    sum = sum + t[keys[(i % 3) + 1]]
    sum = sum + #s
    i = i
end
print(sum)
