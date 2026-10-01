-- HOLA - the smallest thing that is still a script
-- @name Hola
--
-- Every callback is optional: this one only has draw() and touch(), and a
-- variable that survives between frames because it lives in the chunk. The
-- sizes are read from aos.W and aos.H every frame, so it needs no resize()
-- to live through the screen turning.

local n = 0

function draw()
    n = n + 1
    local r = aos.W // 8 + math.sin(n * 0.05) * (aos.W // 12) // 1
    aos.clear(0x001018)
    aos.disc(aos.W // 2, aos.H // 2, r, 0x00E5FF)
    local s = "HOLA DESDE LUA"
    aos.text((aos.W - #s * 12) // 2, 24, s, 0xFFFFFF, 2)
    aos.text(10, aos.H - 28, string.format("CUADRO %d", n), 0x7F8C9F, 1)
end

function touch(x, y, ev)
    if ev == "down" then aos.beep(1200, 25) end
end
