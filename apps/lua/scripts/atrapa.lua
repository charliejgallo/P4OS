-- ATRAPA - catch the stars, by touch alone
-- @name Atrapa
-- @orientation portrait
--
-- A whole game in a page, for a board with no buttons: the basket goes
-- where the finger is (aos.touch()), anywhere on the screen, so the thumb
-- never covers what falls. Three stars missed and it is over; a tap starts
-- again.
--
-- The sky is drawn once and frozen (aos.background()), so the stars, the
-- basket and the score are drawn every frame and never erased: the app puts
-- the sky back under them. And "@orientation portrait" above makes its
-- launcher entry turn the screen upright, because a falling game wants the
-- height.

local W, H = aos.W, aos.H
local BASKET_W, BASKET_Y = 44, 0
local bx = W // 2
local stars, score, lives, best = {}, 0, 3, 0
local state = "title"          -- "title", "play", "over"
local spawn, speed = 0, 1

local function sky()
    -- a gradient in bands, darker at the top, and a few fixed dots
    for y = 0, H - 1, 6 do
        local t = y / H
        local c = (math.floor(8 + 20 * t) << 16) | (math.floor(12 + 30 * t) << 8)
                  | math.floor(40 + 50 * t)
        aos.rect(0, y, W, 6, c)
    end
    math.randomseed(7)
    for i = 1, 60 do
        aos.pixel(math.random(0, W - 1), math.random(0, H - 60), 0x6070A0)
    end
    aos.rect(0, H - 14, W, 14, 0x0C1020)
    aos.background()
end

local function reset()
    stars, score, lives, spawn, speed = {}, 0, 3, 0, 1
end

function init()
    BASKET_Y = H - 40          -- above the strip where the swipe home starts
    sky()
end

function tick(dt)
    local x, y, down = aos.touch()
    if down then bx = math.max(BASKET_W // 2, math.min(W - BASKET_W // 2, x)) end
    if state ~= "play" then return end

    spawn = spawn - dt
    if spawn <= 0 then
        spawn = math.max(260, 900 - score * 12)
        stars[#stars + 1] = { x = math.random(10, W - 10), y = -8,
                              v = (0.08 + math.random() * 0.06) * speed }
    end
    speed = 1 + score / 40
    for i = #stars, 1, -1 do
        local s = stars[i]
        s.y = s.y + s.v * dt
        if s.y >= BASKET_Y - 4 and s.y <= BASKET_Y + 6 and math.abs(s.x - bx) <= BASKET_W // 2 + 4 then
            table.remove(stars, i)
            score = score + 1
            aos.beep(1400 + (score % 8) * 90, 18)
        elseif s.y > H then
            table.remove(stars, i)
            lives = lives - 1
            aos.beep(240, 90)
            if lives <= 0 then
                state = "over"
                if score > best then best = score end
            end
        end
    end
end

local function centre(s, y, c, k)
    aos.text((W - #s * 6 * k) // 2, y, s, c, k)
end

function draw()
    for i = 1, #stars do
        local s = stars[i]
        aos.disc(s.x, s.y // 1, 5, 0xFFD740)
        aos.disc(s.x, s.y // 1, 2, 0xFFFFFF)
    end
    aos.rect(bx - BASKET_W // 2, BASKET_Y, BASKET_W, 10, 0x30D158)
    aos.rect(bx - BASKET_W // 2 + 4, BASKET_Y + 10, BASKET_W - 8, 4, 0x1A7F36)

    aos.text(8, 24, string.format("%d", score), 0xFFFFFF, 3)
    for i = 1, lives do aos.disc(W - 12 * i, 34, 4, 0xFF453A) end

    if state == "title" then
        centre("ATRAPA", H // 3, 0xFFD740, 4)
        centre("TOCA PARA JUGAR", H // 3 + 50, 0xB0B8D0, 2)
    elseif state == "over" then
        centre("FIN", H // 3, 0xFF453A, 5)
        centre(string.format("RECORD %d", best), H // 3 + 56, 0xB0B8D0, 2)
        centre("TOCA PARA SEGUIR", H // 3 + 84, 0xB0B8D0, 2)
    end
end

function touch(x, y, ev)
    if ev == "down" and state ~= "play" then
        reset()
        state = "play"
        aos.beep(900, 30)
    end
end
