-- PINTAR - finger painting, with every finger
-- @name Pintar
-- @canvas 360x640
--
-- Two things this shows that the other samples do not:
--
--   - A script may choose its canvas. This one asks for 360x640 in the
--     line above, which the OS shows at x2 on the 720x1280 screen: strokes
--     twice as fine as on the default 240x426 x3. Lying down it keeps the
--     same canvas, centred, and the painting survives the turn untouched,
--     because a canvas of a fixed size never changes under a script.
--   - Nothing is cleared. The buffer lasts between frames, so what a finger
--     paints stays, and a frame costs only what the fingers touched.
--
-- Each finger paints on its own (aos.fingers(): the ids say whose stroke a
-- point belongs to), so two fingers draw two lines. The strip at the bottom
-- is the palette; its last square wipes the paper.

local PAPER = 0xF4F0E6
local INK   = { 0x1C1C1E, 0xFF453A, 0xFF9F0A, 0xFFD60A,
                0x30D158, 0x0A84FF, 0xBF5AF2, 0x8E6E53 }
local ink   = INK[1]
local SW    = 0                -- swatch width, from the canvas
local PAL_Y, PAL_H = 0, 44     -- the palette strip
local R     = 3                -- the brush's radius
local last  = {}               -- finger id -> {x, y}

-- A filled triangle, a row at a time: enough for a cat's ears.
local function tri(x0, y0, x1, y1, x2, y2, c)
    if y1 < y0 then x0, y0, x1, y1 = x1, y1, x0, y0 end
    if y2 < y0 then x0, y0, x2, y2 = x2, y2, x0, y0 end
    if y2 < y1 then x1, y1, x2, y2 = x2, y2, x1, y1 end
    for y = y0, y2 do
        local xa = x0 + (x2 - x0) * (y - y0) / math.max(1, y2 - y0)
        local xb
        if y < y1 then xb = x0 + (x1 - x0) * (y - y0) / math.max(1, y1 - y0)
        else xb = x1 + (x2 - x1) * (y - y1) / math.max(1, y2 - y1) end
        if xa > xb then xa, xb = xb, xa end
        aos.rect(xa // 1, y, (xb - xa) // 1 + 1, 1, c)
    end
end

-- The house's mascot, sitting in the corner of a clean sheet: a black kitten.
local function kitten(cx, cy)
    local K = 0x111114
    aos.disc(cx, cy + 34, 26, K)                          -- body
    aos.rect(cx + 18, cy + 50, 30, 7, K)                  -- tail
    aos.disc(cx + 46, cy + 44, 6, K)
    aos.disc(cx, cy, 20, K)                               -- head
    tri(cx - 19, cy - 6, cx - 15, cy - 30, cx - 3, cy - 16, K)
    tri(cx + 19, cy - 6, cx + 15, cy - 30, cx + 3, cy - 16, K)
    aos.disc(cx - 8, cy - 2, 4, 0xC7E86A)                 -- eyes
    aos.disc(cx + 8, cy - 2, 4, 0xC7E86A)
    aos.rect(cx - 8, cy - 5, 1, 6, K)
    aos.rect(cx + 8, cy - 5, 1, 6, K)
    aos.rect(cx - 1, cy + 6, 3, 2, 0xE58CA0)              -- nose
end

local function palette()
    aos.rect(0, PAL_Y, aos.W, PAL_H, 0xD8D2C4)
    for i = 1, #INK + 1 do
        local x = (i - 1) * SW
        local c = INK[i] or PAPER
        aos.rect(x + 4, PAL_Y + 6, SW - 8, PAL_H - 12, c)
        if INK[i] == ink then
            aos.frame(x + 1, PAL_Y + 3, SW - 2, PAL_H - 6, 0x000000)
        end
    end
    local x = #INK * SW                                   -- the wipe: an X
    aos.line(x + 12, PAL_Y + 12, x + SW - 13, PAL_Y + PAL_H - 13, 0x8E8E93)
    aos.line(x + SW - 13, PAL_Y + 12, x + 12, PAL_Y + PAL_H - 13, 0x8E8E93)
end

local function sheet()
    aos.clear(PAPER)
    kitten(aos.W - 70, 90)
    aos.text(12, 16, "PINTA CON LOS DEDOS", 0x8E8E93, 2)
    palette()
end

function init()
    -- The bottom 18 rows (36 on the screen) are where the swipe home starts:
    -- the palette sits just above them.
    SW = aos.W // (#INK + 1)
    PAL_Y = aos.H - 18 - PAL_H
    sheet()
end

local function dab(x0, y0, x1, y1)
    local dx, dy = x1 - x0, y1 - y0
    local n = math.max(1, math.floor(math.sqrt(dx * dx + dy * dy) / 2))
    for i = 0, n do
        aos.disc((x0 + dx * i / n) // 1, (y0 + dy * i / n) // 1, R, ink)
    end
end

function draw()
    local n, id1, x1, y1, id2, x2, y2 = aos.fingers()
    local seen = {}
    local function paint(id, x, y)
        if not id then return end
        seen[id] = true
        if y >= PAL_Y - R then last[id] = nil return end  -- not on the palette
        local p = last[id]
        if p then dab(p[1], p[2], x, y) else dab(x, y, x, y) end
        last[id] = { x, y }
    end
    if n >= 1 then paint(id1, x1, y1) end
    if n >= 2 then paint(id2, x2, y2) end
    for id in pairs(last) do
        if not seen[id] then last[id] = nil end           -- that finger lifted
    end
end

function touch(x, y, ev)
    if ev ~= "down" or y < PAL_Y then return end
    local i = x // SW + 1
    if i <= #INK then
        ink = INK[i]
        palette()
        aos.beep(1000 + i * 60, 15)
    else
        sheet()
        aos.beep(500, 40)
    end
end
