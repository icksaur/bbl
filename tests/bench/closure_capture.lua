local makers = {}
for i = 0, 9999 do
    local x = i
    local y = i * 2
    makers[i] = function() return x + y end
end
local sum = 0
for i = 0, 9999 do
    sum = sum + makers[i]()
end
print(sum)
