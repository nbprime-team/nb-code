-- hello.lua -- the Lua highlighter, in a file this small
local function greet(name)
    return "hello, " .. (name or "world")
end

print(greet("nbCode"))   -- run it with primeLua
