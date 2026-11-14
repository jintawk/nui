--[[
    render.lua - draw the example scenes offline and write them out as images.

    The same library code that runs in game runs here: a real Direct3D 8 device on a hidden window,
    a copy of FFXiMain.dll loaded so its EndScene wrapper is there to hook, and frames pushed
    through that wrapper. What comes back is the same pixels the game would show. That is how the
    screenshots in the README were made, and it is how you can iterate on a window's layout without
    starting the game.

        lua example/render.lua <out dir> [scene ...]

    Needs, through the environment:
        NUI_DEMO_DLL    the module built by build.sh      (default example/out/_nuidemo.dll)
        NUI_TEST_HOST   directory holding cr_testhost.dll (built by test/build_test.sh)
        NUI_FFXI_COPY   an unpacked copy of FFXiMain.dll  (never the installed one)
        NUI_FFXI_DIR    your FINAL FANTASY XI install directory (read-only: fonts, window art, icons)
        NUI_BG          optional raw BGRA 3840x2160 backdrop to draw over
]]
local out = arg[1] or 'out'
local HERE = (arg[0] or 'render.lua'):match('^(.*[/\\])') or ''
package.path = HERE .. '?.lua;' .. package.path

local DLL = os.getenv('NUI_DEMO_DLL') or (HERE .. 'out/_nuidemo.dll')
local HOST = os.getenv('NUI_TEST_HOST') or error('set NUI_TEST_HOST to the directory holding cr_testhost.dll')
local COPY = os.getenv('NUI_FFXI_COPY') or error('set NUI_FFXI_COPY to an unpacked FFXiMain.dll')
local DIR = os.getenv('NUI_FFXI_DIR') or error('set NUI_FFXI_DIR to your FINAL FANTASY XI install')
local BG = os.getenv('NUI_BG')

-- The client's own code has to be in the process: its DllMain unpacks .text, and that is where the
-- EndScene wrapper nui patches lives.
package.loadlib(COPY, 'luaopen_none')
package.cpath = HOST .. '/?.dll;' .. package.cpath
local T = require('cr_testhost')
assert(T.make_device())

local N = assert(package.loadlib(DLL, 'luaopen__nuidemo'))()
N.screen(3840, 2160, false, 1)        -- Lua draws in screen pixels, 1:1, as on a 4K client
N.wintype(6)                          -- the game's window type; 1..8, follows the player's setting in game
local ok, msg = N.start(DIR)
assert(ok, tostring(msg))

local scenes = require('scenes')
local ctx = {
    N = N,
    metrics = N.metrics(),
    pic = assert(N.tex_load(HERE .. 'media/logo.png', 512, 512)),
}

local list = assert(io.open(out .. '/shots.txt', 'w'))
local wanted = {}
for i = 2, #arg do wanted[arg[i]] = true end

for _, s in ipairs(scenes) do
    if not next(wanted) or wanted[s.name] then
        local worst = 0
        for i = 1, s.frames or 1 do          -- a scene with `frames` is an animation; one .raw per frame
            N.wintype(s.wintype and (type(s.wintype) == 'function' and s.wintype(i) or s.wintype) or 6)
            local b = require('layer').new()
            s.draw(b, ctx, i)
            assert(N.layer(1, 0, b:out()))
            for _ = 1, 2 do                  -- the first frame uploads the textures, the second draws them
                if s.bg ~= false and BG then T.bg(BG) else T.fill(s.fill or 0xFF0E1218) end
                T.frame(N.status().site)
            end
            local r = s.rect
            local name = s.frames and ('%s_%03d'):format(s.name, i) or s.name
            T.dump_rect(out .. '/' .. name .. '.raw', r[1], r[2], r[3], r[4])
            list:write(('%s %d %d %d %d\n'):format(name, r[1], r[2], r[3], r[4]))
            worst = math.max(worst, N.status().last_us or 0)
            N.layer(1, 0, '')
        end
        print(('%-14s %3d frame(s)  worst %6.0f us'):format(s.name, s.frames or 1, worst))
    end
end
list:close()
N.stop()
