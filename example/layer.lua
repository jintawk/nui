--[[
    layer.lua - a thin Lua helper that builds the drawing-command text nui.layer() takes.

    A layer is plain text: one command per line, fields separated by tabs. You can build it with
    string concatenation if you like; this helper just saves you counting fields. Colours are
    RRGGBBAA hex strings.

        local L = require('layer')
        local b = L.new()
        b:window(40, 40, 420, 120, 'Example')
        b:text(60, 78, 'Hello from Direct3D', 'FFFFFFFF')
        nui.layer(1, 0, b:out())

    Every method is one command; see docs/layer-commands.md for the wire format.
]]
local L = {}
L.__index = L

local fmt, concat = string.format, table.concat

-- round to a whole pixel: the game's art is pixel art, so fractional coordinates blur it
local function px(n) return fmt('%d', n >= 0 and n + 0.5 or n - 0.5) end

function L.new()
    return setmetatable({n = 0}, L)
end

function L:add(...)
    self.n = self.n + 1
    self[self.n] = concat({...}, '\t')
    return self
end

--- A window in the game's own frame art, at whatever size you ask for.
-- active=true draws the brighter "focused" variant the game uses for the window you are typing in.
function L:window(x, y, w, h, title, active)
    return self:add('W', px(x), px(y), px(w), px(h), title or '', active and '1' or '0')
end

--- Body text in the game's chat and menu font ('font    moji'), 1:1, drawn additively like the game's.
function L:text(x, y, s, colour)
    return self:add('T', px(x), px(y), colour or 'FFFFFFFF', s)
end

--- The italic serif the game uses for nameplates and headings. `size` is cap height in pixels, so
-- unlike the body font this one scales smoothly.
function L:title(x, y, size, s, colour)
    return self:add('H', px(x), px(y), fmt('%g', size), colour or 'FFFFFFFF', s)
end

--- The game's window-title font ('menu    menu2fon'), the one on its own log window's title bar.
-- y is the window's top edge; the glyphs are placed on the frame exactly as the game places its own.
function L:wtitle(x, y, s, colour)
    return self:add('M', px(x), px(y), colour or 'C0C0C0FF', s)
end

--- A solid rectangle.
function L:rect(x0, y0, x1, y1, colour)
    return self:add('R', px(x0), px(y0), px(x1), px(y1), colour)
end

--- An HP/MP/TP gauge cut from the game's own 'menu    gauge' art. frac is 0..1; the fill is tinted.
function L:gauge(x, y, w, h, frac, colour)
    return self:add('G', px(x), px(y), px(w), px(h), fmt('%.4f', frac), colour)
end

--- An item icon straight out of the game's item DATs, by item id. 32 is the game's own cell size.
-- grey=true draws it desaturated, for an item you cannot use.
function L:icon(x, y, size, item_id, colour, grey)
    return self:add('I', px(x), px(y), px(size), fmt('%d', item_id), colour or 'FFFFFFFF', grey and '2' or '0')
end

--- Part of a loaded picture (nui.tex_load). Source coordinates are texels, destination is pixels.
-- filter: 'point' keeps pixel art crisp, 'linear' smooths, 'grey'/'linear grey' desaturate.
local FILTER = {point = 0, linear = 1, grey = 2, ['linear grey'] = 3}
function L:pic(handle, x, y, w, h, sx, sy, sw, sh, colour, filter)
    return self:add('Q', fmt('%d', handle), px(x), px(y), px(x + w), px(y + h),
                    px(sx), px(sy), px(sx + sw), px(sy + sh),
                    colour or 'FFFFFFFF', fmt('%d', FILTER[filter or 'point'] or 0))
end

--- Clip everything that follows to a rectangle, until pop(). Nests.
function L:clip(x0, y0, x1, y1) return self:add('C', px(x0), px(y0), px(x1), px(y1)) end
function L:pop() return self:add('c') end

--- A comment line. nui ignores it; handy when you are reading a layer you dumped to disk.
function L:note(s) return self:add('# ' .. s) end

function L:out() return concat(self, '\n', 1, self.n) end

return L
