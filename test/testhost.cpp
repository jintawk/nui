// testhost.cpp - cr_testhost.dll, a Lua module for the nui library's tests and the mock-up renders of
// the offline renders of addons built on it; never shipped. The name is kept from before it
// moved here: the consumers' Lua tests require('cr_testhost'). bg(path) puts a 4K screenshot (raw BGRA)
// in the back buffer.
// Gives the stock Lua interpreter a real D3D8 device on a hidden window and a way to run a frame
// through FFXiMain's (patched) EndScene wrapper with a fake renderer whose +0x0C is that device,
// so a consumer DLL runs exactly as in game: UiHook thunk -> dispatcher -> its draw -> real EndScene.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using lua_State = struct lua_State;
using lua_CFunction = int(__cdecl*)(lua_State*);
void(__cdecl* createtable)(lua_State*, int, int);
void(__cdecl* pushcclosure)(lua_State*, lua_CFunction, int);
void(__cdecl* setfield)(lua_State*, int, char const*);
void(__cdecl* pushnumber)(lua_State*, double);
void(__cdecl* pushstring)(lua_State*, char const*);
void(__cdecl* pushboolean)(lua_State*, int);
double(__cdecl* tonumber)(lua_State*, int);
char const*(__cdecl* tolstring)(lua_State*, int, std::size_t*);

template <typename T>
bool bind(HMODULE m, T& fn, char const* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

constexpr int W = 3840, H = 2160;
IDirect3D8* g_d3d = nullptr;
IDirect3DDevice8* g_dev = nullptr;
struct FakeRenderer { unsigned char pad[0x0C]; void* device; unsigned char rest[0x40]; } g_renderer;

// make_device() -> true | false, why
int __cdecl l_make_device(lua_State* L) {
    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cr_testhost";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("cr_testhost", "nui", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    HMODULE d3d8 = LoadLibraryA("d3d8.dll");
    using Create8 = IDirect3D8*(WINAPI*)(UINT);
    Create8 create = nullptr;
    if (d3d8) bind(d3d8, create, "Direct3DCreate8");
    g_d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!g_d3d) { pushboolean(L, 0); pushstring(L, "Direct3DCreate8 failed"); return 2; }
    D3DDISPLAYMODE mode {};
    g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    D3DPRESENT_PARAMETERS pp {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = mode.Format;
    pp.hDeviceWindow = hwnd;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    // FPU_PRESERVE (nui 1.1.0): without it D3D8 leaves the thread's FPU at 24-bit precision and the Lua
    // numbers of a consumer's test above 2^24 (mob ids) collide; build_test.sh checks 17293316 + 1
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev))) {
        pushboolean(L, 0); pushstring(L, "CreateDevice failed"); return 2;
    }
    g_renderer.device = g_dev;
    pushboolean(L, 1);
    return 1;
}

// fill(0xRRGGBB): the back buffer, as if the game had drawn a frame
int __cdecl l_fill(lua_State* L) {
    uint32_t const c = 0xFF000000u | uint32_t(tonumber(L, 1));
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT lr;
    bool ok = back && SUCCEEDED(back->LockRect(&lr, nullptr, 0));
    if (ok) {
        for (int y = 0; y < H; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<char*>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < W; ++x) row[x] = c;
        }
        back->UnlockRect();
    }
    if (back) back->Release();
    pushboolean(L, ok);
    return 1;
}

// bg(path): a 3840 x 2160 raw BGRA screenshot into the back buffer -> true | false
int __cdecl l_bg(lua_State* L) {
    std::size_t n = 0;
    char const* path = tolstring(L, 1, &n);
    FILE* f = path ? std::fopen(path, "rb") : nullptr;
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT lr;
    bool ok = f && back && SUCCEEDED(back->LockRect(&lr, nullptr, 0));
    if (ok) {
        for (int y = 0; y < H && ok; ++y)
            ok = std::fread(static_cast<char*>(lr.pBits) + y * lr.Pitch, 4, W, f) == size_t(W);
        back->UnlockRect();
    }
    if (back) back->Release();
    if (f) std::fclose(f);
    pushboolean(L, ok ? 1 : 0);
    return 1;
}

// frame(site hex string) -> hresult: BeginScene, then the wrapper at `site` with ecx = renderer
int __cdecl l_frame(lua_State* L) {
    std::size_t n = 0;
    char const* s = tolstring(L, 1, &n);
    auto const site = static_cast<std::uintptr_t>(std::strtoul(s ? s : "0", nullptr, 16));
    using Wrapper = long(__fastcall*)(void*, void*);       // fastcall: ecx = renderer, no stack args
    g_dev->BeginScene();
    long const hr = reinterpret_cast<Wrapper>(site)(&g_renderer, nullptr);
    pushnumber(L, double(static_cast<unsigned long>(hr)));
    return 1;
}

// pixel(x, y) -> r, g, b
int __cdecl l_pixel(lua_State* L) {
    int const x = int(tonumber(L, 1)), y = int(tonumber(L, 2));
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT lr;
    uint32_t c = 0;
    if (back && SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        c = reinterpret_cast<uint32_t const*>(static_cast<char const*>(lr.pBits) + y * lr.Pitch)[x];
        back->UnlockRect();
    }
    if (back) back->Release();
    pushnumber(L, (c >> 16) & 0xFF);
    pushnumber(L, (c >> 8) & 0xFF);
    pushnumber(L, c & 0xFF);
    return 3;
}

// dump(path): the whole back buffer as raw BGRA rows (debugging a pixel mismatch)
int __cdecl l_dump(lua_State* L) {
    std::size_t n = 0;
    char const* path = tolstring(L, 1, &n);
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DSURFACE_DESC d {};
    D3DLOCKED_RECT lr;
    bool ok = false;
    if (back && SUCCEEDED(back->GetDesc(&d)) && SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (FILE* f = std::fopen(path, "wb")) {
            for (UINT y = 0; y < d.Height; ++y) std::fwrite(static_cast<char const*>(lr.pBits) + y * lr.Pitch, 4, d.Width, f);
            std::fclose(f);
            ok = true;
        }
        back->UnlockRect();
    }
    if (back) back->Release();
    pushboolean(L, ok ? 1 : 0);
    return 1;
}

// dump_rect(path, x0, y0, x1, y1): part of the back buffer as raw BGRA rows -> true | false
int __cdecl l_dump_rect(lua_State* L) {
    std::size_t n = 0;
    char const* path = tolstring(L, 1, &n);
    int x0 = int(tonumber(L, 2)), y0 = int(tonumber(L, 3)), x1 = int(tonumber(L, 4)), y1 = int(tonumber(L, 5));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT lr;
    bool ok = false;
    if (back && x1 > x0 && y1 > y0 && SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (FILE* f = std::fopen(path, "wb")) {
            for (int y = y0; y < y1; ++y)
                std::fwrite(static_cast<char const*>(lr.pBits) + y * lr.Pitch + x0 * 4, 4, std::size_t(x1 - x0), f);
            std::fclose(f);
            ok = true;
        }
        back->UnlockRect();
    }
    if (back) back->Release();
    pushboolean(L, ok ? 1 : 0);
    return 1;
}

// stray(0xRRGGBB, x0, y0, x1, y1) -> count, bx0, by0, bx1, by1: pixels anywhere on the whole back
// buffer, outside the rectangle, that are not the background colour (and their bounding box)
int __cdecl l_stray(lua_State* L) {
    uint32_t const bg = uint32_t(tonumber(L, 1)) & 0xFFFFFFu;
    int const x0 = int(tonumber(L, 2)), y0 = int(tonumber(L, 3)), x1 = int(tonumber(L, 4)), y1 = int(tonumber(L, 5));
    IDirect3DSurface8* back = nullptr;
    g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back);
    D3DLOCKED_RECT lr;
    double n = 0;
    int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
    if (back && SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        for (int y = 0; y < H; ++y) {
            auto const* row = reinterpret_cast<uint32_t const*>(static_cast<char const*>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < W; ++x) {
                if (x >= x0 && x < x1 && y >= y0 && y < y1) continue;
                if ((row[x] & 0xFFFFFFu) != bg) {
                    n += 1;
                    if (x < bx0) bx0 = x;
                    if (y < by0) by0 = y;
                    if (x > bx1) bx1 = x;
                    if (y > by1) by1 = y;
                }
            }
        }
        back->UnlockRect();
    }
    if (back) back->Release();
    pushnumber(L, n);
    pushnumber(L, bx0);
    pushnumber(L, by0);
    pushnumber(L, bx1);
    pushnumber(L, by1);
    return 5;
}

// set_game_state(): odd render states a game might leave before EndScene
int __cdecl l_set_game_state(lua_State* L) {
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    g_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_REVSUBTRACT);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL);
    D3DVIEWPORT8 vp {10, 20, 300, 200, 0.0f, 1.0f};
    g_dev->SetViewport(&vp);
    (void)L;
    return 0;
}

// game_state() -> a string of the states above, to compare before / after a frame
int __cdecl l_game_state(lua_State* L) {
    DWORD v[10] = {};
    g_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &v[0]);
    g_dev->GetRenderState(D3DRS_SRCBLEND, &v[1]);
    g_dev->GetRenderState(D3DRS_DESTBLEND, &v[2]);
    g_dev->GetRenderState(D3DRS_ZENABLE, &v[3]);
    g_dev->GetRenderState(D3DRS_CULLMODE, &v[4]);
    g_dev->GetRenderState(D3DRS_FOGENABLE, &v[5]);
    g_dev->GetRenderState(D3DRS_BLENDOP, &v[6]);
    g_dev->GetTextureStageState(0, D3DTSS_COLOROP, &v[7]);
    g_dev->GetTextureStageState(0, D3DTSS_MAGFILTER, &v[8]);
    g_dev->GetVertexShader(&v[9]);
    D3DVIEWPORT8 vp {};
    g_dev->GetViewport(&vp);
    IDirect3DBaseTexture8* t = nullptr;
    g_dev->GetTexture(0, &t);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "rs %lu %lu %lu %lu %lu %lu %lu tss %lu %lu vs %lu vp %lu,%lu %lux%lu tex %p",
                  v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], vp.X, vp.Y, vp.Width, vp.Height,
                  static_cast<void*>(t));
    if (t) t->Release();
    pushstring(L, buf);
    return 1;
}

// fake_config(ptr_hex, id, value): a config manager whose entry `id` holds `value`, stored into the
// client's global at ptr (a .data slot of the loaded FFXiMain copy), as the running game has it.
// fake_ui(ptr_hex, w, h): the same for the UI-size object (+0x18 width, +0x1A height).
// Addresses travel as hex strings (from before the device kept the FPU's precision, nui 1.1.0).
struct FakeCfg { std::uint32_t vtbl; std::uint32_t entries; } g_cfg;
unsigned char g_entries[0x2C * 256];
unsigned char g_uiobj[0x40];

std::uint32_t hex_arg(lua_State* L, int i) {
    std::size_t n = 0;
    char const* s = tolstring(L, i, &n);
    return s ? static_cast<std::uint32_t>(std::strtoul(s, nullptr, 16)) : 0;
}

int __cdecl l_fake_config(lua_State* L) {
    std::uint32_t const ptr = hex_arg(L, 1);
    int const id = static_cast<int>(tonumber(L, 2));
    std::int32_t const value = static_cast<std::int32_t>(tonumber(L, 3));
    if (!ptr || id < 0 || id >= 256) { pushboolean(L, 0); return 1; }
    std::memset(g_entries, 0, sizeof(g_entries));
    for (std::int32_t i = 0; i < 256; ++i) std::memcpy(g_entries + i * 0x2C + 4, &i, 4);
    std::memcpy(g_entries + id * 0x2C + 8, &value, 4);
    g_cfg.entries = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_entries));
    std::uint32_t const mgr = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_cfg));
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr)), &mgr, 4);
    pushboolean(L, 1);
    return 1;
}

int __cdecl l_fake_ui(lua_State* L) {
    std::uint32_t const ptr = hex_arg(L, 1);
    auto const w = static_cast<std::uint16_t>(tonumber(L, 2)), h = static_cast<std::uint16_t>(tonumber(L, 3));
    if (!ptr) { pushboolean(L, 0); return 1; }
    std::memcpy(g_uiobj + 0x18, &w, 2);
    std::memcpy(g_uiobj + 0x1A, &h, 2);
    std::uint32_t const obj = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_uiobj));
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ptr)), &obj, 4);
    pushboolean(L, 1);
    return 1;
}

// A pre-release UiHook ABI 1 module (hook_mod.cpp built with -DHOOKMOD_ABI1: mods/mod_x0.dll, from the
// frozen header test/abi1/UiHook_abi1.h), for the consumers' "ABI 1 is refused / blocks" checks now that
// no addon is built against ABI 1 any more.
//   abi1_start(path) -> slot (>= 0, it patched or joined) or a negative code (-1 not hooked) | nil if it
//                       did not load;  abi1_calls() -> its draws so far;  abi1_site() -> its bus's site (hex);
//   abi1_stop()
HMODULE g_abi1 = nullptr;
volatile long g_abi1_calls = 0;
void __cdecl abi1_sink(int, void*, void*) { ++g_abi1_calls; }

template <typename T>
T abi1_fn(char const* name) {
    return g_abi1 ? reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(g_abi1, name))) : nullptr;
}

int __cdecl l_abi1_start(lua_State* L) {
    std::size_t n = 0;
    char const* path = tolstring(L, 1, &n);
    if (!g_abi1 && path) g_abi1 = LoadLibraryA(path);
    using start_fn = int(__cdecl*)(void(__cdecl*)(int, void*, void*), int);
    start_fn const start = abi1_fn<start_fn>("m_start");
    if (!start) return 0;
    pushnumber(L, start(&abi1_sink, 1));
    return 1;
}
int __cdecl l_abi1_stop(lua_State*) {
    using stop_fn = void(__cdecl*)();
    if (stop_fn const stop = abi1_fn<stop_fn>("m_stop")) stop();
    return 0;
}
int __cdecl l_abi1_calls(lua_State* L) {
    pushnumber(L, double(g_abi1_calls));
    return 1;
}
int __cdecl l_abi1_site(lua_State* L) {
    using bus_fn = void*(__cdecl*)();
    bus_fn const bus = abi1_fn<bus_fn>("m_bus");
    // ABI 1's UiBus: nine 4-byte fields, block, dispatch_ptr, then site (test/abi1/UiHook_abi1.h)
    unsigned char const* b = bus ? static_cast<unsigned char const*>(bus()) : nullptr;
    std::uint32_t site = 0;
    if (b) std::memcpy(&site, b + 44, 4);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(site));
    pushstring(L, buf);
    return 1;
}

// private_kb() -> this process's private bytes (PrivateUsage) in KiB: memory measurements of modules
int __cdecl l_private_kb(lua_State* L) {
    struct Pmc {
        DWORD cb, faults;
        SIZE_T peak_ws, ws, peak_paged, paged, peak_nonpaged, nonpaged, pagefile, peak_pagefile, private_usage;
    } pmc {};
    pmc.cb = sizeof(pmc);
    using get_fn = BOOL(WINAPI*)(HANDLE, Pmc*, DWORD);
    get_fn const get = reinterpret_cast<get_fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo")));
    pushnumber(L, get && get(GetCurrentProcess(), &pmc, sizeof(pmc)) ? double(pmc.private_usage) / 1024.0 : -1.0);
    return 1;
}

int __cdecl l_device(lua_State* L) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(g_dev)));
    pushstring(L, buf);
    return 1;
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl luaopen_cr_testhost(lua_State* L) {
    HMODULE m = GetModuleHandleA("lua5.1.dll");
    if (!m) m = GetModuleHandleA("lua51.dll");
    if (!m || !bind(m, createtable, "lua_createtable") || !bind(m, pushcclosure, "lua_pushcclosure") ||
        !bind(m, setfield, "lua_setfield") || !bind(m, pushnumber, "lua_pushnumber") ||
        !bind(m, pushstring, "lua_pushstring") || !bind(m, pushboolean, "lua_pushboolean") ||
        !bind(m, tonumber, "lua_tonumber") || !bind(m, tolstring, "lua_tolstring"))
        return 0;
    createtable(L, 0, 8);
    struct Fn { char const* name; lua_CFunction f; };
    Fn const fns[] = {{"make_device", l_make_device}, {"fill", l_fill}, {"frame", l_frame}, {"pixel", l_pixel},
                      {"set_game_state", l_set_game_state}, {"game_state", l_game_state}, {"device", l_device},
                      {"dump", l_dump}, {"dump_rect", l_dump_rect}, {"stray", l_stray}, {"bg", l_bg}, {"fake_config", l_fake_config}, {"fake_ui", l_fake_ui},
                      {"abi1_start", l_abi1_start}, {"abi1_stop", l_abi1_stop}, {"abi1_calls", l_abi1_calls},
                      {"abi1_site", l_abi1_site}, {"private_kb", l_private_kb}};
    for (Fn const& fn : fns) {
        pushcclosure(L, fn.f, 0);
        setfield(L, -2, fn.name);
    }
    return 1;
}
