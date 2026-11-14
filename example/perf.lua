--[[
    perf.lua - what a layer costs: to build in Lua, and to draw.

        lua example/perf.lua [frames] [scene]        (default: 600 frames, the 'hud' scene)

    Same environment as render.lua (see example/README.md): NUI_TEST_HOST, NUI_FFXI_COPY,
    NUI_FFXI_DIR, NUI_DEMO_DLL.

    It measures three things separately, because they happen on different threads and only one of
    them can stall the game:

      build   nui.layer(): parsing the command text and turning it into vertices. This runs on
              Windower's Lua thread, which the game thread waits on, so this is the number that
              matters most. Your addon only pays it when the layer's content changes.
      draw    the render thread's pass over the finished vertices, once per displayed frame.
      idle    frames where nothing changed: the layer is replayed from its cache.
]]
local FRAMES = tonumber(arg[1]) or 600
local SCENE = arg[2] or 'hud'
local HERE = (arg[0] or 'perf.lua'):match('^(.*[/\\])') or ''
package.path = HERE .. '?.lua;' .. package.path

local DLL = os.getenv('NUI_DEMO_DLL') or (HERE .. 'out/_nuidemo.dll')
local HOST = assert(os.getenv('NUI_TEST_HOST'), 'set NUI_TEST_HOST')
local COPY = assert(os.getenv('NUI_FFXI_COPY'), 'set NUI_FFXI_COPY')
local DIR = assert(os.getenv('NUI_FFXI_DIR'), 'set NUI_FFXI_DIR')

package.loadlib(COPY, 'luaopen_none')
package.cpath = HOST .. '/?.dll;' .. package.cpath
local T = require('cr_testhost')
assert(T.make_device())
local N = assert(package.loadlib(DLL, 'luaopen__nuidemo'))()
N.screen(3840, 2160, false, 1)
N.wintype(6)
assert(N.start(DIR))

local scenes, Layer = require('scenes'), require('layer')
local scene
for _, s in ipairs(scenes) do if s.name == SCENE then scene = s end end
assert(scene, 'no scene called ' .. SCENE)
local ctx = {N = N, metrics = N.metrics(), pic = assert(N.tex_load(HERE .. 'media/logo.png', 512, 512))}

-- the text an addon would hand to layer(), rebuilt each time as an addon would
local function build_text()
    local b = Layer.new()
    scene.draw(b, ctx, 1)
    return b:out()
end

local site = N.status().site
N.layer(1, 0, build_text())
for _ = 1, 3 do T.fill(0xFF0E1218) T.frame(site) end     -- warm up: uploads, first parse
N.prof()

-- 1. every frame changes: build + draw, the worst case
local t0 = N.us()
for _ = 1, FRAMES do
    N.layer(1, 0, build_text())
    T.fill(0xFF0E1218)
    T.frame(site)
end
local wall_changing = (N.us() - t0) / FRAMES
local p1 = N.prof()

-- 2. nothing changes: the layer is replayed
local t1 = N.us()
for _ = 1, FRAMES do
    T.fill(0xFF0E1218)
    T.frame(site)
end
local wall_idle = (N.us() - t1) / FRAMES
local p2 = N.prof()

-- 3. the Lua side alone, with no drawing at all: how long the text itself takes to produce
local t2 = N.us()
for _ = 1, FRAMES do build_text() end
local text_us = (N.us() - t2) / FRAMES

local st = N.status()
local c = st.cache or {}
print(('scene %s: %d quads, %d batches, %d frames'):format(SCENE, st.quads or 0, st.batches or 0, FRAMES))
print()
print(('  building the command text in Lua      %7.1f us'):format(text_us))
print(('  nui.layer() parse + vertices          %7.1f us avg  %7.1f us worst')
      :format(p1.layer_us_avg or 0, p1.layer_us_max or 0))
print(('  draw, content changing every frame    %7.1f us avg  %7.1f us worst')
      :format(p1.us_draw_avg or 0, p1.us_draw_max or 0))
print(('  draw, nothing changing                %7.1f us avg  %7.1f us worst')
      :format(p2.us_draw_avg or 0, p2.us_draw_max or 0))
print(('  wall clock a frame, changing          %7.1f us'):format(wall_changing))
print(('  wall clock a frame, idle              %7.1f us'):format(wall_idle))
print()
print(('  textures: %d shared, %d private, %d KiB live')
      :format(c.shared_textures or 0, c.private_textures or 0, c.live_kb or 0))
print(('  lock waits over 50 us: %d render, %d worker')
      :format(p1.waits_over_50 or 0, p1.w_waits_over_50 or 0))
N.stop()
