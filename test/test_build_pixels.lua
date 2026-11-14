--[[
    test_build_pixels.lua - draws five captured HUD states (fixtures/layers/<state>.layers) through one
    module DLL built from the nui library (nuitest.cpp + src), on cr_testhost's real D3D8 device with frames
    through the patched EndScene wrapper, and writes each back buffer to <out>_<state>_s<scale>.raw.
    build_build_test.sh runs it once with the reference build and once with the library and compares the
    files byte for byte.

      lua test_build_pixels.lua <FFXiMain copy> <dir of cr_testhost.dll> <module dll> <fixtures dir> <install> <out prefix>
      NUI_BUILD_PICTURE=<png>  the picture behind texture handle 2 (default: fixtures/pic.png; the
                               captured layers drew an icon atlas of the same handle)
]]
local COPY, HOSTDIR, DLL, FIX, INSTALL, OUT = arg[1], arg[2], arg[3], arg[4], arg[5], arg[6]
package.loadlib(COPY, 'luaopen_none')                 -- FFXiMain.dll in the process (the wrapper the module hooks)
package.cpath = HOSTDIR .. '/?.dll;' .. package.cpath
local T = require('cr_testhost')
local N = assert(package.loadlib(DLL, 'luaopen__nuitest'))()
local ok, msg = N.start(INSTALL)
assert(ok, 'start: ' .. tostring(msg))
assert(T.make_device())
local site = N.status().site

local function exists(p) local f = io.open(p, 'rb') if f then f:close() return true end return false end
local pic = os.getenv('NUI_BUILD_PICTURE')
if not pic or pic == '' then
    pic = FIX .. '/pic.png'
end
local h = assert(N.tex_load(pic, 4096, 4096))
assert(h == 2, 'the picture is not handle 2')

local function read_state(name)
    local f = assert(io.open(FIX .. '/layers/' .. name .. '.layers', 'rb'))
    local all = f:read('*a')
    f:close()
    local st = {layers = {}}
    local pos = 1
    local line, e = all:match('^(SCREEN[^\n]*)\n()', pos)
    local w, hh, auto, sf = line:match('SCREEN (%S+) (%S+) (%S+) (%S+)')
    st.screen = {tonumber(w), tonumber(hh), auto == '1', tonumber(sf)}
    pos = e
    while pos <= #all do
        local hdr, e2 = all:match('^(LAYER[^\n]*)\n()', pos)
        if not hdr then break end
        local t = {}
        for x in hdr:gmatch('%S+') do t[#t + 1] = x end
        local n = tonumber(t[11])
        st.layers[#st.layers + 1] = {id = tonumber(t[2]), z = tonumber(t[3]), ox = tonumber(t[4]), oy = tonumber(t[5]),
            clip = t[6] == '1' and {tonumber(t[7]), tonumber(t[8]), tonumber(t[9]), tonumber(t[10])} or nil,
            text = all:sub(e2, e2 + n - 1)}
        pos = e2 + n + 1
    end
    return st
end

local n = 0
for _, name in ipairs({'still', 'busy', 'flashing', 'next', 'soak'}) do
    local st = read_state(name)
    for id = 1, 16 do N.layer(id) end
    for _, l in ipairs(st.layers) do
        assert(N.layer(l.id, l.z, l.text))
        if l.clip then N.layer_pos(l.id, l.ox, l.oy, l.clip[1], l.clip[2], l.clip[3], l.clip[4])
        else N.layer_pos(l.id, l.ox, l.oy) end
    end
    for scale = 1, 2 do
        N.screen(st.screen[1] / scale, st.screen[2] / scale, st.screen[3], st.screen[4])
        for _ = 1, 3 do T.fill(0x101418) T.frame(site) end    -- the first frames make the textures
        assert(T.dump(('%s_%s_s%d.raw'):format(OUT, name, scale)))
        n = n + 1
    end
    local s = N.status()
    print(('  %s: %d layers, %d quads, %d batches, last draw %.0f us'):format(name, #st.layers, s.quads or 0, s.batches or 0, s.last_us or 0))
end
for id = 1, 16 do N.layer(id) end
N.stop()
print(('  %d back buffers written (%s, picture %s)'):format(n, DLL:match('[^/\\]+[/\\][^/\\]+$') or DLL, pic:match('[^/\\]+$')))
