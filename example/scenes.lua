--[[
    scenes.lua - what the README's screenshots show.

    Each scene is a table: a name, the rectangle of the back buffer to write out, and a draw
    function handed a fresh layer builder. Nothing here is special to the offline renderer: the
    same draw function, handed the same builder, is what an addon passes to nui.layer() in game.
]]
local ITEMS = {4096, 4097, 4098, 4099, 4101, 4102, 4103}   -- the eight crystals, by item id

-- colours, RRGGBBAA
local WHITE, DIM, GOLD = 'FFFFFFFF', 'A8B0BCFF', 'FFD98AFF'
local GREEN, BLUE, YELLOW, RED = '7CD97CFF', '7FB2FFFF', 'FFD24AFF', 'FF6B5AFF'

local S = {}

local function shot(t) S[#S + 1] = t end

-- 1. The smallest useful thing: a window and a line of text.
shot {
    name = 'hello', rect = {24, 24, 624, 214},
    draw = function(b)
        b:window(40, 40, 560, 150, 'nui')
        b:text(60, 78, 'Hello from Direct3D 8.', WHITE)
        b:text(60, 100, 'This window is the game\'s own art, drawn by an addon.', DIM)
        b:text(60, 130, 'The frame, the font and the colours are read out of', DIM)
        b:text(60, 152, 'the client\'s own DAT files at run time.', DIM)
    end,
}

-- 2. The three fonts, all read out of the client's own DAT files.
shot {
    name = 'fonts', rect = {24, 24, 884, 474},
    draw = function(b)
        b:window(40, 40, 820, 410, 'Fonts')
        b:title(60, 96, 34, 'The nameplate serif', GOLD)
        b:text(60, 134, 'the client\'s nameplate font. Size is a parameter, so it scales.', DIM)
        b:title(60, 168, 24, 'Twenty-four pixel caps', WHITE)
        b:title(60, 202, 17, 'Seventeen', WHITE)
        b:text(60, 240, 'Body text is the chat font, drawn at 1:1 and additively, exactly as the', WHITE)
        b:text(60, 258, 'game draws its own. ABCDEFGHIJKLM abcdefghijklm 0123456789', WHITE)
        b:text(60, 276, 'Vana\'diel, Jeuno, Ru\'Lude Gardens, Chateau d\'Oraguille. !?@#%&*()[]', DIM)
        b:rect(60, 302, 840, 303, '55606EFF')
        b:text(60, 318, 'Red', RED)
        b:text(110, 318, 'Gold', GOLD)
        b:text(170, 318, 'Green', GREEN)
        b:text(235, 318, 'Blue', BLUE)
        b:text(290, 318, 'Colours are RRGGBBAA hex.', DIM)
        b:text(60, 346, 'Widths come from the client, so a string measures the same here as it', DIM)
        b:text(60, 364, 'does in game: metrics().moji is the advance table the client uses.', DIM)
        b:text(60, 392, 'The font is a texture, so the vertex colour greys, tints', '8892A0FF')
        b:text(600, 392, 'and fades it.', 'FFFFFF55')
        b:text(60, 416, 'The title bar above is the third font, the one on the game\'s own windows.', DIM)
    end,
}

-- 3. Widgets: the gauge the game draws its own HP/MP/TP with, and item icons from the item DATs.
shot {
    name = 'widgets', rect = {24, 24, 884, 414},
    draw = function(b)
        b:window(40, 40, 820, 350, 'Widgets')
        b:text(60, 78, 'Gauges, cut from the client\'s own gauge art:', DIM)
        local function bar(y, label, frac, colour, val)
            b:text(60, y, label, DIM)
            b:gauge(104, y + 2, 300, 9, frac, colour)
            b:text(420, y, val, WHITE)
        end
        bar(104, 'HP', 0.82, GREEN, '1,340 / 1,634')
        bar(126, 'MP', 0.41, BLUE, '520 / 1,268')
        bar(148, 'TP', 1.00, YELLOW, '3000')
        bar(170, 'HP', 0.14, RED, '229 / 1,634')
        b:text(570, 104, 'One texel column of the game\'s', DIM)
        b:text(570, 122, 'gauge art: a dark groove, the', DIM)
        b:text(570, 140, 'tinted fill, and its own', DIM)
        b:text(570, 158, 'untinted end caps.', DIM)
        b:text(60, 212, 'Item icons, by item id, straight out of the client\'s DAT files:', DIM)
        for i, id in ipairs(ITEMS) do
            b:icon(60 + (i - 1) * 44, 236, 32, id)
        end
        b:text(60, 290, 'The same icons greyed, one flag on the command:', DIM)
        for i, id in ipairs(ITEMS) do
            b:icon(60 + (i - 1) * 44, 314, 32, id, WHITE, true)
        end
        b:text(570, 236, 'Icons are shared: two addons', DIM)
        b:text(570, 254, 'drawing the same item hold', DIM)
        b:text(570, 272, 'one texture between them.', DIM)
    end,
}

-- 4. Pictures: your own art, drawn by handle.
shot {
    name = 'picture', rect = {24, 24, 884, 414},
    draw = function(b, c)
        local p = c.pic
        b:window(40, 40, 820, 350, 'Pictures')
        b:text(60, 78, 'Load a PNG or JPEG, draw any part of it at any size:', DIM)
        b:pic(p, 60, 104, 128, 128, 0, 0, 128, 128)
        b:text(60, 244, '1:1', DIM)
        b:pic(p, 214, 104, 128, 128, 0, 0, 128, 128, WHITE, 'grey')
        b:text(214, 244, 'greyed', DIM)
        b:pic(p, 368, 104, 128, 128, 0, 0, 128, 128, 'FFD98AFF')
        b:text(368, 244, 'tinted', DIM)
        b:pic(p, 522, 104, 128, 128, 0, 0, 128, 128, 'FFFFFF66')
        b:text(522, 244, 'faded', DIM)
        b:pic(p, 676, 104, 128, 128, 32, 32, 40, 40, WHITE, 'linear')
        b:text(676, 244, 'cropped', DIM)
        b:text(60, 280, 'Textures live in a cache shared by every addon in the process, keyed by', DIM)
        b:text(60, 298, 'what is in them. Two addons showing the same map hold one copy of it.', DIM)
        b:text(60, 326, 'Point and linear filtering are per quad, so pixel art stays crisp', DIM)
        b:text(60, 344, 'next to a smoothly scaled photograph.', DIM)
    end,
}

-- 5. The same window in each of the game's eight window styles.
for t = 1, 8 do
    shot {
        name = 'type' .. t, wintype = t, rect = {24, 24, 524, 194},
        draw = function(b)
            b:window(40, 40, 460, 130, 'Window type ' .. t)
            b:text(60, 80, 'Style ' .. t .. ' of 8. The client keeps the', WHITE)
            b:text(60, 100, 'player\'s choice in its config table;', WHITE)
            b:text(60, 120, 'wtread() returns it.', WHITE)
        end,
    }
end

-- 6. Clipping: a list longer than the window it is in.
shot {
    name = 'clip', rect = {24, 24, 584, 384},
    draw = function(b)
        local ROWS = {
            {'Hi-Potion', 4116, 12}, {'Ether', 4128, 3}, {'Antidote', 4148, 24},
            {'Echo Drops', 4151, 11}, {'Holy Water', 4154, 7}, {'Remedy', 4155, 2},
            {'Reraiser', 4172, 1}, {'Vile Elixir', 4174, 4}, {'Gysahl Greens', 4545, 99},
        }
        b:window(40, 40, 520, 320, 'Clipping')
        b:clip(56, 70, 544, 320)
        for i, r in ipairs(ROWS) do
            local y = 46 + i * 36
            if i % 2 == 0 then b:rect(56, y - 4, 544, y + 28, 'FFFFFF0C') end
            b:icon(62, y - 2, 28, r[2])
            b:text(100, y + 2, r[1], WHITE)
            b:text(520, y + 2, 'x' .. r[3], DIM)
        end
        b:pop()
        b:rect(56, 320, 544, 321, '55606EFF')
        b:text(60, 330, 'the list runs on; the clip rectangle ends here', DIM)
    end,
}

-- 7. A HUD, over the game, with the game's own windows switched off.
shot {
    name = 'hud', rect = {16, 16, 1296, 796}, bg = true,
    draw = function(b, c)
        local PARTY = {
            {'Lakshmi', 'RDM/NIN', 1634, 1634, 1268, 1100, 3000},
            {'Tanaka', 'PLD/WAR', 2410, 1702, 420, 210, 1240},
            {'Mireille', 'WHM/SCH', 1288, 1288, 1402, 980, 0},
            {'Oswin', 'DRK/SAM', 2104, 980, 310, 310, 2780},
            {'Perrine', 'BRD/WHM', 1190, 1190, 1066, 1066, 540},
        }
        -- party list
        b:window(40, 40, 420, 300, 'Party')
        for i, p in ipairs(PARTY) do
            local y = 42 + i * 48
            b:text(60, y, p[1], WHITE)
            b:text(170, y, p[2], DIM)
            local hpf = p[4] / p[3]
            b:gauge(60, y + 20, 170, 8, hpf, hpf < 0.25 and RED or hpf < 0.6 and YELLOW or GREEN)
            b:gauge(240, y + 20, 110, 8, p[6] / p[5], BLUE)
            b:gauge(360, y + 20, 80, 8, p[7] / 3000, YELLOW)
        end
        -- target
        b:window(500, 40, 440, 160, 'Target')
        b:title(520, 96, 26, 'Cherufe', GOLD)
        b:text(520, 122, 'Lv. 128   Beastmen   claimed', DIM)
        b:gauge(520, 146, 400, 10, 0.63, RED)
        b:text(520, 170, '63%', WHITE)
        b:icon(856, 164, 26, 4154)
        b:icon(888, 164, 26, 4097, WHITE, true)
        -- recasts
        b:window(980, 40, 300, 160, 'Recasts')
        local RECAST = {{'Chainspell', 0.00, 'ready'}, {'Composure', 0.72, '1:14'}, {'Convert', 0.31, '6:48'}}
        for i, r in ipairs(RECAST) do
            local y = 50 + i * 38
            local ready = r[2] == 0
            b:text(1000, y, r[1], ready and WHITE or DIM)
            b:gauge(1000, y + 20, 180, 8, 1 - r[2], ready and GREEN or 'FFFFFF55')
            b:text(1210, y, r[3], ready and GREEN or DIM)
        end
        -- a tooltip, following a cursor
        b:window(500, 250, 480, 220, '', true)
        b:icon(520, 282, 32, 4174)
        b:title(564, 306, 22, 'Vile Elixir', GOLD)
        b:text(520, 328, 'All Races', DIM)
        b:text(520, 352, 'Restores 50% of HP and MP.', WHITE)
        b:text(520, 372, 'Cannot be used in some areas.', WHITE)
        b:rect(520, 398, 960, 399, '55606EFF')
        b:text(520, 414, 'Auction House', DIM)
        b:text(640, 414, '38,000 gil', GOLD)
        b:text(520, 436, 'Inventory', DIM)
        b:text(640, 436, '4', WHITE)
        b:pic(c.pic, 892, 396, 52, 52, 0, 0, 128, 128, 'FFFFFF44', 'linear')
        -- a line of links
        b:window(40, 520, 1240, 110, 'Links')
        b:text(60, 556, 'Quest:', DIM)
        b:text(112, 556, 'The Rescue Operation', '8FC9FFFF')
        b:rect(112, 572, 272, 573, '8FC9FFAA')
        b:text(300, 556, 'Zone:', DIM)
        b:text(344, 556, 'Bastok Mines', '8FC9FFFF')
        b:rect(344, 572, 444, 573, '8FC9FFAA')
        b:text(470, 556, 'NPC:', DIM)
        b:text(510, 556, 'Zaldon', '8FC9FFFF')
        b:rect(510, 572, 558, 573, '8FC9FFAA')
        b:text(600, 556, 'Reward:', DIM)
        b:icon(662, 550, 24, 4116)
        b:text(692, 556, 'Hi-Potion x3', WHITE)
        b:text(60, 592, 'Links are ordinary text. nui reports the width of a string; the addon does the hit testing.', DIM)
        -- the game's own window art, same frame, drawn small
        b:window(40, 660, 1240, 110, 'Layers')
        b:text(60, 700, 'The five windows above are one layer. It is parsed once into vertices, merged into', DIM)
        b:text(60, 722, 'as few batches as the overlaps allow, and drawn on the frame\'s EndScene call.', DIM)
    end,
}

-- 8. An animation: the same window as the player cycles the game's window style.
shot {
    name = 'cycle', frames = 8, wintype = function(i) return i end,
    rect = {24, 24, 644, 214},
    draw = function(b, c, i)
        b:window(40, 40, 580, 150, 'Window type ' .. i)
        b:text(60, 80, 'The client keeps the player\'s window style', WHITE)
        b:text(60, 102, 'as config id 18. wtread() reads it and', WHITE)
        b:text(60, 124, 'wintype() draws in it.', WHITE)
        b:text(60, 156, 'nui.wintype(' .. i .. ')', GOLD)
    end,
}

-- 9. An animation: live numbers, a frame at a time, the way an addon feeds them in.
shot {
    name = 'live', frames = 48, rect = {24, 24, 744, 364},
    draw = function(b, c, i)
        local t = (i - 1) / 48
        local hp = 0.92 - 0.72 * math.min(1, t * 1.7) + 0.68 * math.max(0, t * 1.7 - 1)
        local mp = 0.74 - 0.5 * t
        local tp = (t * 2.4) % 1
        local cast = math.min(1, (t * 3) % 1.5)
        b:window(40, 40, 680, 300, 'Live')
        b:text(60, 80, 'Lakshmi', WHITE)
        b:text(150, 80, 'RDM/NIN', DIM)
        b:gauge(60, 100, 300, 10, hp, hp < 0.25 and RED or hp < 0.6 and YELLOW or GREEN)
        b:text(380, 97, ('HP %d%%'):format(math.floor(hp * 100 + 0.5)), WHITE)
        b:gauge(60, 126, 300, 10, mp, BLUE)
        b:text(380, 123, ('MP %d%%'):format(math.floor(mp * 100 + 0.5)), WHITE)
        b:gauge(60, 152, 300, 10, tp, tp >= 0.999 and 'FFFFFFFF' or YELLOW)
        b:text(380, 149, ('TP %d'):format(math.floor(tp * 3000)), tp >= 0.999 and GOLD or WHITE)
        b:rect(60, 186, 700, 187, '55606EFF')
        b:text(60, 204, 'Casting', DIM)
        b:gauge(140, 206, 420, 10, cast, cast >= 1 and GREEN or '9FD2FFFF')
        b:text(580, 204, cast >= 1 and 'done' or 'Cure IV', cast >= 1 and GREEN or WHITE)
        for k = 1, 7 do
            local on = ((i + k * 5) % 24) < 14
            b:icon(60 + (k - 1) * 40, 236, 32, ITEMS[k], WHITE, not on)
        end
        b:text(60, 290, 'The addon re-sends this layer when a value changes.', DIM)
        b:text(60, 312, 'Between changes it is redrawn from the parsed form.', DIM)
    end,
}


-- 10. The same HUD as the player changes the game's window style.
shot {
    name = 'theme', frames = 8, wintype = function(i) return i end,
    rect = {24, 24, 1004, 364},
    draw = function(b, c, i)
        b:window(40, 40, 460, 300, 'Party')
        local PARTY = {
            {'Lakshmi', 'RDM/NIN', 0.98, 0.86, 1.00},
            {'Tanaka', 'PLD/WAR', 0.71, 0.50, 0.41},
            {'Mireille', 'WHM/SCH', 1.00, 0.70, 0.00},
            {'Oswin', 'DRK/SAM', 0.47, 1.00, 0.93},
            {'Perrine', 'BRD/WHM', 1.00, 1.00, 0.18},
        }
        for k, p in ipairs(PARTY) do
            local y = 42 + k * 48
            b:text(60, y, p[1], WHITE)
            b:text(170, y, p[2], DIM)
            b:gauge(60, y + 20, 180, 8, p[3], p[3] < 0.5 and YELLOW or GREEN)
            b:gauge(250, y + 20, 120, 8, p[4], BLUE)
            b:gauge(380, y + 20, 100, 8, p[5], YELLOW)
        end
        b:window(540, 40, 440, 160, 'Target')
        b:title(560, 96, 26, 'Cherufe', GOLD)
        b:text(560, 122, 'Lv. 128   Beastmen   claimed', DIM)
        b:gauge(560, 146, 400, 10, 0.63, RED)
        b:text(560, 170, '63%', WHITE)
        b:window(540, 220, 440, 120, 'Window type ' .. i)
        b:text(560, 262, 'Window style ' .. i .. ', read from the client\'s', WHITE)
        b:text(560, 284, 'config table. Both windows above are', WHITE)
        b:text(560, 306, 'drawn from that style\'s own frame art.', WHITE)
    end,
}


return S
