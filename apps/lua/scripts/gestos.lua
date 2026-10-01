-- @name Gestos
--
-- The two ways a script gets more than one finger:
--
--   gesture(ev, x, y, a, b, c)  what the fingers MEAN, already filtered:
--                               a pinch zooms this little star field about
--                               the point between the fingers, one finger
--                               drags it, a double tap brings it home.
--   aos.fingers()               where each finger IS, with an id that stays
--                               while it stays down: the rings.
--
-- The world is drawn again every frame (aos.clear), so it costs all the rows;
-- for a zoomable world that is the honest price.
--
-- P4OS: the field is sized from the canvas, and the view is kept centred
-- when the screen turns (resize). The back swipe still works from the left
-- edge: a drag that starts anywhere else is the script's.

local BG   = 0x05050C
local zoom, ox, oy = 1.0, 0, 0     -- screen = world * zoom + (ox, oy)
local stars = {}
local FW, FH                       -- the world's size
local last = ""

local function home()
    zoom = 1.0
    ox, oy = (aos.W - FW) / 2, (aos.H - FH) / 2
end

function init()
    FW, FH = aos.W * 3, aos.H * 3 // 2
    local n = FW * FH // 4000      -- the watch's density: 90 in 580x620
    for i = 1, n do
        stars[i] = { x = math.random(0, FW), y = math.random(0, FH),
                     c = ({ 0xFFFFFF, 0xFFD740, 0x69F0AE, 0x00E5FF })[math.random(1, 4)] }
    end
    home()
end

function resize(w, h)
    home()
end

function gesture(ev, x, y, a, b, c)
    if ev == "pinch" then
        local nz = zoom * a
        if nz < 0.4 then nz = 0.4 end
        if nz > 6 then nz = 6 end
        local k = nz / zoom
        ox = x - (x - ox) * k + b          -- the point under the fingers stays
        oy = y - (y - oy) * k + c
        zoom = nz
    elseif ev == "drag" then
        ox, oy = ox + a, oy + b
    elseif ev == "double" then
        home()
    end
    if ev ~= "pinch" and ev ~= "drag" then last = string.upper(ev) end
end

function draw()
    aos.clear(BG)
    local r = math.floor(zoom * 1.5)
    for i = 1, #stars do
        local s = stars[i]
        local sx, sy = math.floor(s.x * zoom + ox), math.floor(s.y * zoom + oy)
        if sx > -4 and sx < aos.W + 4 and sy > 18 and sy < aos.H + 4 then
            if r < 1 then aos.pixel(sx, sy, s.c) else aos.disc(sx, sy, r, s.c) end
        end
    end
    local n, id1, x1, y1, id2, x2, y2 = aos.fingers()
    if n >= 1 then aos.ring(x1, y1, 16, 0x2EC4FF) end
    if n >= 2 then aos.ring(x2, y2, 16, 0xFF3B6B) end
    aos.rect(0, 0, aos.W, 16, 0x101020)
    aos.text(4, 5, string.upper(string.format("x%.1f %s  %d DEDOS", zoom, last, n)),
             0x8090B0, 1)
end
