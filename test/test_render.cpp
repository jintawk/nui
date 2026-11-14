// test_render.cpp - the shared texture cache through the WHOLE renderer (nui design E, step 5b):
// separate module DLLs built from src (nuitest.cpp, see build_render_test.sh), driven
// through their real Lua API (the stock lua5.1.dll, as Windower drives an addon), drawing through
// UiHook's patch of a real (unpacked copy of) FFXiMain.dll's EndScene wrapper on a real D3D8 device,
// LoadLibrary'd and FreeLibrary'd in any order, with or without stop(). Pixels are read back from the
// device's back buffer; the cache is inspected through its mapping.
//
//   test_render.exe <dll dir> <FFXiMain.dll copy> <ffxi install dir> <fixtures dir>
//                   [--only t1,t2] [--golden golden_fmt.json] [--update-golden] [--d3d8 path\D3D8.dll]
//                   [--big big.png]
//
// Tests (FAIL lines carry the tag the self-test looks for):
//   e1     two modules, one layer each: one atlas / icon / picture entry each, holders {a, b}, the same
//          texture pointer in both backends, refcount 1, each module's pixels as when it draws alone
//   e2     a, b, c: 6 load orders x 6 unload orders, with stop() and with FreeLibrary alone: a frame after
//          every step (every live module's region identical to the reference, EndScene's result and
//          the caller's registers kept, render states restored), then nothing left in the cache and the
//          device's refcount back to its baseline
//   e3c    a module of cache major 99 and one of major 1: each its own mapping, both draw
//   e4     PrivateUsage of module b's first frame while a holds the same layer: shared vs noshare
//   e5     a new device: gen + 1, every texture made again on it, the old device's refcount back
//   e7     per-icon entries: pixels = icon_pixels (the DAT decode prep_assets.py checks), shared,
//          released after the grace time once nobody draws them, found again within it
//   fmt    golden_fmt.json: each kind's pixel hash next to its fmt; pixels changed with the same fmt fail
//   e8     fallbacks: a full table -> private texture, still drawn; noshare draws identically; a
//          lock held by a dead client is broken
//   wt     window types 1-8: the top and bottom rules repeat along the window (no font texels in a
//          rule: the type 1 bug), and each window's crop matches golden_fmt.json
//   fr     1.2.2 window types 1-8: nothing drawn outside the window's rectangle (the bottom rule drew row
//          y + h in 7 types); the bottom rule's light row meets its corners'; type 6's on row h - 2,
//          as the game's log window
//   gr     1.1.0 grey: Q filter 2 / 3 and I fx 2 against the luma of their texels, the colour quad after a
//          grey batch, the batch count, stage 1-3 states and the texture factor handed back, filter 7 as 0
//   fpu    the device keeps the FPU's precision (D3DCREATE_FPU_PRESERVE): a number above 2^24 survives a frame
//   bb     1.2.0 the back-buffer rule: mid-frame calls with a texture target bound draw nothing, the
//          final call draws once; no back-buffer call for 500 ms -> every call draws, until one has it
//   vc     1.2.0 the backend's vertex cache: a changed list, a moved list, a scale change, a texture that
//          arrives under a kept list are all drawn as freshly built; an unchanged layer is not transformed again
//   lp     1.2.0 layer_pos at the same place and clip builds nothing; any change of place or clip rebuilds
//   ic     1.2.0 item icons cut on the worker: new ids, ids back after the grace time: zero file reads on the
//          render thread; an atlas a module held at start() and that went before the first frame: the render
//          thread asks the worker, or after a second reads it itself (counted)
//   lk     1.2.0 a second thread draws frames while 300 layers of 1,500 quads are sent: no torn frame, the
//          render thread never waits over 50 us for the lock (the build is outside it)
//   bench  16 icons in a row (16 textures) vs 16 quads of one texture: batches and microseconds a frame
//   pd     1.2.1 pictures: a first load is read, hashed and sized on the Lua thread and decoded on the
//          decode thread (render-thread frames skip it until then; budget: tex_load <= a quarter of the
//          decode); a picture another module holds is held (nc_hold), not decoded, and survives the
//          sweep that left 1.1.0 blank; wintype the same for the atlas; an offline frame on the Lua thread
//          waits for the decode; freed while decoded, stop() while decoded, an undecodable body, and
//          FreeLibrary while decoded (the thread keeps the module mapped). Needs --big (make_big_png.py)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <d3d8.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/NuiCache/NuiCache.h"
#include "hook_harness.hpp"
#include "png_write.hpp"

// ---- the test's clock (the modules' NUICACHE_NOW, NUI_TEST_CLOCK) --------------------------------

volatile LONG g_now = 1000000;
extern "C" __declspec(dllexport) unsigned long __cdecl test_now() { return static_cast<unsigned long>(g_now); }
// 1.2.5 (ex): the site set here throws std::bad_alloc once, inside the module (src/host.cpp NUI_TEST_THROW);
// 1.2.6 (rv): g_throw_extra more times at the same site
volatile LONG g_throw_site = 0, g_throw_extra = 0;
extern "C" __declspec(dllexport) int __cdecl test_throw(int site) {
    if (InterlockedCompareExchange(&g_throw_site, 0, site) != site) return 0;
    if (g_throw_extra > 0) {
        InterlockedDecrement(&g_throw_extra);
        InterlockedExchange(&g_throw_site, site);
    }
    return 1;
}
// 1.2.6 (tl): every decode on a module's decode thread first sleeps this long (src/host.cpp decode_one)
volatile LONG g_decode_ms = 0;
extern "C" __declspec(dllexport) unsigned long __cdecl test_decode_ms() { return static_cast<unsigned long>(g_decode_ms); }
void advance(LONG ms) { InterlockedExchangeAdd(&g_now, ms); }

// ---- results ------------------------------------------------------------------------------------

std::string g_test;
int g_pass = 0, g_fail = 0;
bool check(bool ok, char const* tag, char const* fmt, ...) {
    if (ok) { ++g_pass; return true; }
    ++g_fail;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("FAIL [%s] %s\n", tag, buf);
    return false;
}

// ---- Lua (the stock interpreter's DLL, as Windower's LuaCore) ------------------------------------

struct lua_State;
typedef int(__cdecl* lua_CFunction)(lua_State*);
struct LuaFns {
    lua_State*(__cdecl* newstate)();
    void(__cdecl* openlibs)(lua_State*);
    int(__cdecl* loadstring)(lua_State*, char const*);
    int(__cdecl* pcall)(lua_State*, int, int, int);
    char const*(__cdecl* tolstring)(lua_State*, int, std::size_t*);
    void(__cdecl* settop)(lua_State*, int);
    int(__cdecl* gettop)(lua_State*);
    void(__cdecl* pushcclosure)(lua_State*, lua_CFunction, int);
    void(__cdecl* setfield)(lua_State*, int, char const*);
} g_l;
lua_State* L = nullptr;
constexpr int kGlobals = -10002;

template <typename T>
bool bindf(HMODULE m, T& fn, char const* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

bool lua_init() {
    HMODULE m = LoadLibraryA("C:\\Program Files (x86)\\Lua\\5.1\\lua5.1.dll");
    if (!m) return false;
    bool ok = bindf(m, g_l.newstate, "luaL_newstate") && bindf(m, g_l.openlibs, "luaL_openlibs") &&
              bindf(m, g_l.loadstring, "luaL_loadstring") && bindf(m, g_l.pcall, "lua_pcall") &&
              bindf(m, g_l.tolstring, "lua_tolstring") && bindf(m, g_l.settop, "lua_settop") &&
              bindf(m, g_l.gettop, "lua_gettop") && bindf(m, g_l.pushcclosure, "lua_pushcclosure") &&
              bindf(m, g_l.setfield, "lua_setfield");
    if (!ok) return false;
    L = g_l.newstate();
    g_l.openlibs(L);
    return true;
}

// Runs a chunk; returns tostring() of its first result ("ERR <msg>" on an error).
std::string lua(char const* fmt, ...) {
    char code[8192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(code, sizeof(code), fmt, ap);
    va_end(ap);
    std::string const wrapped = std::string("return tostring((function() ") + code + "\nend)())";
    g_l.settop(L, 0);
    std::string out;
    if (g_l.loadstring(L, wrapped.c_str()) != 0 || g_l.pcall(L, 0, 1, 0) != 0) {
        char const* e = g_l.tolstring(L, -1, nullptr);
        out = std::string("ERR ") + (e ? e : "?");
    } else {
        char const* s = g_l.tolstring(L, -1, nullptr);
        out = s ? s : "nil";
    }
    g_l.settop(L, 0);
    return out;
}
double luan(char const* fmt, ...) {
    char code[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(code, sizeof(code), fmt, ap);
    va_end(ap);
    return std::atof(lua("%s", code).c_str());
}

// ---- D3D8: a real device on a hidden window ------------------------------------------------------

constexpr int W = 1280, H = 1024;
constexpr std::uint32_t BG = 0x203040;
std::string g_d3d8 = "d3d8.dll";
IDirect3D8* g_d3d = nullptr;
IDirect3DDevice8* g_dev = nullptr;        // the device frames go to now

IDirect3DDevice8* make_device() {
    static bool cls = false;
    if (!cls) {
        WNDCLASSA wc {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "nui_render_test";
        RegisterClassA(&wc);
        cls = true;
    }
    if (!g_d3d) {
        HMODULE m = LoadLibraryA(g_d3d8.c_str());
        using Create8 = IDirect3D8*(WINAPI*)(UINT);
        Create8 create = nullptr;
        if (m) bindf(m, create, "Direct3DCreate8");
        g_d3d = create ? create(D3D_SDK_VERSION) : nullptr;
        if (!g_d3d) return nullptr;
    }
    HWND hwnd = CreateWindowA("nui_render_test", "nui", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                              GetModuleHandleA(nullptr), nullptr);
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
    IDirect3DDevice8* d = nullptr;
    // FPU_PRESERVE (1.1.0): without it D3D8 drops the thread's FPU to 24-bit precision, and the Lua numbers
    // of a test (ids above 2^24) collide; the self-test's NUI_FAULT_NO_FPU_PRESERVE shows t_fpu sees it
#ifndef NUI_FAULT_NO_FPU_PRESERVE
    DWORD const fl = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
#else
    DWORD const fl = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
#endif
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, fl, &pp, &d)))
        return nullptr;
    return d;
}

ULONG refs(IUnknown* u) { u->AddRef(); return u->Release(); }

struct Back {                              // the back buffer, locked read-only (or for a fill)
    IDirect3DSurface8* s = nullptr;
    D3DLOCKED_RECT lr {};
    bool ok = false;
    explicit Back(DWORD flags = D3DLOCK_READONLY) {
        g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &s);
        ok = s && SUCCEEDED(s->LockRect(&lr, nullptr, flags));
    }
    ~Back() {
        if (ok) s->UnlockRect();
        if (s) s->Release();
    }
    std::uint32_t* row(int y) { return reinterpret_cast<std::uint32_t*>(static_cast<char*>(lr.pBits) + y * lr.Pitch); }
};

std::uint32_t g_fill = BG;                 // what frame() fills the back buffer with (fb: black and white)
void fill_bg() {
    Back b(0);
    if (!b.ok) return;
    for (int y = 0; y < H; ++y) {
        std::uint32_t* r = b.row(y);
        for (int x = 0; x < W; ++x) r[x] = 0xFF000000u | g_fill;
    }
}

struct Rect { int x0, y0, x1, y1; };
std::uint64_t hash_rect(Rect r) {
    Back b;
    std::uint64_t h = 0xcbf29ce484222325ull;
    if (!b.ok) return 0;
    for (int y = r.y0; y < r.y1; ++y) {
        std::uint32_t const* row = b.row(y);
        for (int x = r.x0; x < r.x1; ++x) {
            std::uint32_t const c = row[x] & 0xFFFFFFu;
            h = nc_fnv1a64(&c, 3, h);
        }
    }
    return h;
}
int inked(Rect r) {
    Back b;
    int n = 0;
    if (!b.ok) return -1;
    for (int y = r.y0; y < r.y1; ++y) {
        std::uint32_t const* row = b.row(y);
        for (int x = r.x0; x < r.x1; ++x) n += (row[x] & 0xFFFFFFu) != BG;
    }
    return n;
}
std::uint32_t pixel(int x, int y) {
    Back b;
    return b.ok ? (b.row(y)[x] & 0xFFFFFFu) : 0;
}

// odd render states a game might leave before EndScene, and a string of them to compare
void set_game_state() {
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
    g_dev->SetRenderState(D3DRS_FOGENABLE, TRUE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL);
    g_dev->SetTexture(0, nullptr);
}
std::string game_state() {
    DWORD v[9] = {};
    g_dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &v[0]);
    g_dev->GetRenderState(D3DRS_SRCBLEND, &v[1]);
    g_dev->GetRenderState(D3DRS_DESTBLEND, &v[2]);
    g_dev->GetRenderState(D3DRS_ZENABLE, &v[3]);
    g_dev->GetRenderState(D3DRS_CULLMODE, &v[4]);
    g_dev->GetRenderState(D3DRS_FOGENABLE, &v[5]);
    g_dev->GetTextureStageState(0, D3DTSS_COLOROP, &v[6]);
    g_dev->GetTextureStageState(0, D3DTSS_MAGFILTER, &v[7]);
    g_dev->GetVertexShader(&v[8]);
    IDirect3DBaseTexture8* t = nullptr;
    g_dev->GetTexture(0, &t);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%lu %lu %lu %lu %lu %lu %lu %lu %lu %p", v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                  v[7], v[8], static_cast<void*>(t));
    if (t) t->Release();
    return buf;
}

// ---- frames through the patched wrapper ----------------------------------------------------------

std::uintptr_t g_site = 0;

// One game frame: background, BeginScene, the wrapper (UiHook thunk -> dispatcher -> every module's
// draw -> the real EndScene). Checks EndScene's result, the caller's registers and the render states.
bool frame(char const* tag = "frame") {
    advance(17);
    fill_bg();
    set_game_state();
    std::string const before = game_state();
    harness::g_renderer.device = g_dev;
    g_dev->BeginScene();
    harness::Regs const r = harness::run(g_site);
    bool ok = check(r.eax == 0, tag, "EndScene result %08lx", (unsigned long)r.eax);
    ok = check(r.ebx == 0x11111111 && r.esi == 0x22222222 && r.edi == 0x33333333 && r.ebp == 0x44444444 && r.esp_delta == 0,
               tag, "caller registers not kept: ebx %08lx esi %08lx edi %08lx ebp %08lx esp %lu", (unsigned long)r.ebx,
               (unsigned long)r.esi, (unsigned long)r.edi, (unsigned long)r.ebp, (unsigned long)r.esp_delta) && ok;
    ok = check(game_state() == before, tag, "render states not restored: %s -> %s", before.c_str(), game_state().c_str()) && ok;
    return ok;
}

// ---- modules ---------------------------------------------------------------------------------------

std::string g_dir, g_ffxi, g_fix;

struct Mod {
    std::string file, var;
    HMODULE h = nullptr;
};

bool load(Mod& m, char const* file, char const* var) {
    m.file = file;
    m.var = var;
    m.h = LoadLibraryA((g_dir + "\\" + file).c_str());
    if (!check(m.h != nullptr, "load", "cannot load %s (%lu)", file, GetLastError())) return false;
    lua_CFunction open = nullptr;
    bindf(m.h, open, "luaopen__nuitest");
    if (!check(open != nullptr, "load", "%s: no luaopen__nuitest", file)) return false;
    g_l.settop(L, 0);
    g_l.pushcclosure(L, open, 0);
    if (g_l.pcall(L, 0, 1, 0) != 0) return check(false, "load", "%s: luaopen failed", file);
    g_l.setfield(L, kGlobals, var);
    g_l.settop(L, 0);
    return true;
}
// FreeLibrary only: DllMain(DETACH) is all the module gets (host_detach)
void free_lib(Mod& m) {
    lua("%s = nil", m.var.c_str());
    if (m.h) FreeLibrary(m.h);
    m.h = nullptr;
}
std::string start(Mod const& m, char const* flags = nullptr) {
    return lua("local ok, msg = %s.start([[%s]]%s%s%s) return tostring(ok) .. ' ' .. tostring(msg)", m.var.c_str(),
               g_ffxi.c_str(), flags ? ", '" : "", flags ? flags : "", flags ? "'" : "");
}
void stop(Mod const& m) { lua("return %s.stop()", m.var.c_str()); }

// the scene every module draws (a window, chat text, two icons, a picture), at x
constexpr int kY = 60, kWw = 360, kWh = 300;
std::string scene(Mod const& m, int x, int layer = 1) {
    return lua("local M = %s local h, w, hh = M.tex_load([[%s\\pic.png]], 64, 64)\n"
               "if not h then return 'tex_load ' .. tostring(w) end\n"
               "local t = {('W\\t%%d\\t%%d\\t%%d\\t%%d\\tCache'):format(%d, %d, %d, %d),\n"
               "  ('T\\t%%d\\t%%d\\tFFFFFFFF\\tThree Paths'):format(%d + 30, %d + 40),\n"
               "  ('I\\t%%d\\t%%d\\t32\\t4165\\tFFFFFFFF'):format(%d + 30, %d + 80),\n"
               "  ('I\\t%%d\\t%%d\\t18\\t12345\\tFFFFFFFF'):format(%d + 70, %d + 80),\n"
               "  ('Q\\t%%d\\t%%d\\t%%d\\t%%d\\t%%d\\t0\\t0\\t%%d\\t%%d\\tFFFFFFFF\\t1'):format(h, %d + 30, %d + 130, %d + 30 + w, %d + 130 + hh, w, hh)}\n"
               "return M.layer(%d, 0, table.concat(t, '\\n'))",
               m.var.c_str(), g_fix.c_str(), x, kY, kWw, kWh, x, kY, x, kY, x, kY, x, kY, x, kY, layer);
}
Rect region(int x) { return Rect {x - 24, kY - 24, x + kWw + 24, kY + kWh + 24}; }
int const kX[3] = {30, 450, 870};

// ---- the cache's mapping, read directly ------------------------------------------------------------

NcHeader* cache_map(unsigned major) {
    char name[64];
    std::snprintf(name, sizeof(name), "Local\\FFXiNuiCache_v%u_%lu", major, (unsigned long)GetCurrentProcessId());
    HANDLE m = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!m) return nullptr;
    return static_cast<NcHeader*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, NUICACHE_MAP_SIZE));   // leaked, as the modules do
}
std::vector<int> entries(NcHeader* h, std::uint32_t kind, long a = -1) {
    std::vector<int> out;
    if (!h) return out;
    NcEntry* E = nc_detail::entries(h);
    for (int i = 0; i < NUICACHE_ENTRIES; ++i)
        if (E[i].state == 2 && E[i].kind == kind && (a < 0 || E[i].a == std::uint32_t(a))) out.push_back(i);
    return out;
}
LONG bit(Mod const& m) { return LONG(1) << int(luan("return %s.status().cache.slot", m.var.c_str())); }
std::string hex(void* p) { char b[16]; std::snprintf(b, sizeof(b), "%08lx", (unsigned long)reinterpret_cast<std::uintptr_t>(p)); return b; }

// pixels of a cached texture, as the backend uploaded them (ARGB rows)
std::vector<std::uint32_t> tex_pixels(void* obj, int& w, int& h) {
    std::vector<std::uint32_t> px;
    auto* t = static_cast<IDirect3DTexture8*>(obj);
    D3DSURFACE_DESC d {};
    D3DLOCKED_RECT lr {};
    if (!t || FAILED(t->GetLevelDesc(0, &d)) || FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return px;
    w = int(d.Width);
    h = int(d.Height);
    px.resize(std::size_t(w) * h);
    for (int y = 0; y < h; ++y) std::memcpy(&px[std::size_t(y) * w], static_cast<char*>(lr.pBits) + y * lr.Pitch, std::size_t(w) * 4);
    t->UnlockRect(0);
    return px;
}

SIZE_T private_usage() {
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

ULONG g_dev_base = 0;                // the device's refcount with no module texture alive
std::uint64_t g_ref = 0;             // the scene's region hash, drawn by one module alone

bool cache_empty(char const* tag) {
    NcHeader* h = cache_map(1);
    bool ok = check(h && h->live_entries == 0, tag, "%ld live cache entries left", h ? long(h->live_entries) : -1L);
    ULONG const r = refs(g_dev);
    return check(r == g_dev_base, tag, "device refcount %lu, baseline %lu", r, g_dev_base) && ok;
}

// ---- e1 -------------------------------------------------------------------------------------------

void t_e1() {
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    std::string s = start(a);
    check(s.rfind("true", 0) == 0, "e1", "a start: %s", s.c_str());
    g_site = std::strtoul(lua("return A.status().site").c_str(), nullptr, 16);
    if (!check(g_site != 0, "e1", "no site")) return;
    check(scene(a, kX[0]) == "5", "e1", "a scene");
    frame("e1");
    std::uint64_t const alone = hash_rect(region(kX[0]));
    check(inked(region(kX[0])) > 20000, "e1", "a drew %d px", inked(region(kX[0])));
    g_ref = alone;
    s = start(b);
    check(s.rfind("true", 0) == 0, "e1", "b start: %s", s.c_str());
    check(scene(b, kX[1]) == "5", "e1", "b scene");
    frame("e1");
    check(lua("return tostring(A.status().cache.shared) .. tostring(B.status().cache.shared)") == "truetrue", "e1", "both shared");
    NcHeader* h = cache_map(1);
    LONG const both = bit(a) | bit(b);
    for (std::uint32_t kind = NC_ATLAS; kind <= NC_PICTURE; ++kind) {
        std::vector<int> const ids = entries(h, kind);
        int const want = kind == NC_ITEM_ICON ? 2 : 1;
        check(int(ids.size()) == want, "e1", "kind %u: %d entries, want %d", kind, int(ids.size()), want);
        for (int i : ids) {
            NcEntry& e = nc_detail::entries(h)[i];
            check(e.holders == both, "e1", "kind %u entry %d holders %08lx, want %08lx", kind, i, (unsigned long)e.holders,
                  (unsigned long)both);
            check(refs(static_cast<IUnknown*>(e.obj)) == 1, "e1", "kind %u refcount %lu, want 1", kind,
                  refs(static_cast<IUnknown*>(e.obj)));
        }
    }
    std::vector<int> const at = entries(h, NC_ATLAS, 6);
    std::string const pa = lua("return A.status().cache.atlas_ptr"), pb = lua("return B.status().cache.atlas_ptr");
    check(!at.empty() && pa == pb && pa == hex(nc_detail::entries(h)[at[0]].obj), "e1", "atlas pointers a %s b %s entry %s",
          pa.c_str(), pb.c_str(), at.empty() ? "-" : hex(nc_detail::entries(h)[at[0]].obj).c_str());
    check(hash_rect(region(kX[0])) == alone, "e1", "a's pixels changed when b joined");
    check(hash_rect(region(kX[1])) == alone, "e1", "b's pixels differ from a's alone");
    check(luan("return B.status().cache.hits") >= 4, "e1", "b hits %s", lua("return B.status().cache.hits").c_str());
    stop(b);
    frame("e1");
    check(hash_rect(region(kX[0])) == alone && inked(region(kX[1])) == 0, "e1", "after b stopped");
    stop(a);
    free_lib(b);
    free_lib(a);
    cache_empty("e1");
}

// ---- e2 -------------------------------------------------------------------------------------------

void t_e2() {
    char const* files[3] = {"nuitest_a.dll", "nuitest_b.dll", "nuitest_c.dll"};
    char const* vars[3] = {"A", "B", "C"};
    int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    int seqs = 0, frames_bad = 0;
    for (int mode = 0; mode < 2; ++mode) {           // 0: stop() then FreeLibrary; 1: FreeLibrary alone
        for (auto const& lo : perms)
            for (auto const& uo : perms) {
                Mod m[3];
                bool live[3] = {false, false, false};
                bool ok = true;
                auto drawn_right = [&](char const* when) {
                    bool good = true;
                    for (int k = 0; k < 3; ++k) {
                        if (live[k]) good = check(hash_rect(region(kX[k])) == g_ref, "e2", "%s: %s's region wrong (mode %d)", when,
                                                  vars[k], mode) && good;
                        else good = check(inked(region(kX[k])) == 0, "e2", "%s: %s's region not empty", when, vars[k]) && good;
                    }
                    return good;
                };
                for (int step = 0; step < 3 && ok; ++step) {
                    int const k = lo[step];
                    ok = load(m[k], files[k], vars[k]);
                    std::string const s = ok ? start(m[k]) : "";
                    ok = ok && check(s.rfind("true", 0) == 0, "e2", "%s start: %s", vars[k], s.c_str());
                    ok = ok && check(scene(m[k], kX[k]) == "5", "e2", "%s scene", vars[k]);
                    live[k] = ok;
                    if (!frame("e2") || !drawn_right("load")) { ok = false; ++frames_bad; }
                }
                // two references of our own on the atlas: whoever releases the cache's must release it
                // exactly once (a second Release would leave 1 of ours, not 2)
                IUnknown* watch = nullptr;
                {
                    std::vector<int> const at = entries(cache_map(1), NC_ATLAS, 6);
                    if (check(at.size() == 1, "e2", "%d atlas entries with 3 modules", int(at.size()))) {
                        watch = static_cast<IUnknown*>(nc_detail::entries(cache_map(1))[at[0]].obj);
                        watch->AddRef();
                        watch->AddRef();
                    }
                }
                for (int step = 0; step < 3; ++step) {
                    int const k = uo[step];
                    if (!m[k].h) continue;
                    if (mode == 0) stop(m[k]);
                    free_lib(m[k]);
                    live[k] = false;
                    if (!frame("e2") || !drawn_right("unload")) { ok = false; ++frames_bad; }
                }
                if (mode == 1) {
                    // nobody swept the unheld entries: the next module does, once the grace time has passed
                    Mod c;
                    if (load(c, "nuitest_a.dll", "A")) {
                        start(c);
                        advance(NUICACHE_GRACE_MS + 1000);
                        frame("e2");
                        stop(c);
                        free_lib(c);
                    }
                }
                if (watch) {
                    ULONG const left = refs(watch);
                    if (!check(left == 2, "e2", "the atlas was released %d times by the cache, not once (refcount %lu, ours 2)",
                               int(3 - left), left))
                        ok = false;
                    for (ULONG i = 0; i < left && i < 2; ++i) watch->Release();
                }
                if (!cache_empty("e2")) ok = false;
                ++seqs;
                if (!ok) {
                    std::printf("  sequence load %d%d%d unload %d%d%d mode %d failed\n", lo[0], lo[1], lo[2], uo[0], uo[1],
                                uo[2], mode);
                    return;
                }
            }
    }
    std::printf("  %d sequences x 6 frames, %d bad frames\n", seqs, frames_bad);
}

// ---- e3c -------------------------------------------------------------------------------------------

void t_e3c() {
    Mod a, z;
    if (!load(a, "nuitest_a.dll", "A") || !load(z, "nuitest_z.dll", "Z")) return;
    start(a);
    start(z);
    scene(a, kX[0]);
    scene(z, kX[1]);
    frame("e3c");
    NcHeader* h1 = cache_map(1);
    NcHeader* h99 = cache_map(99);
    check(h99 != nullptr && h99 != h1, "e3c", "major 99 has no mapping of its own");
    check(entries(h1, NC_ATLAS).size() == 1 && entries(h99, NC_ATLAS).size() == 1, "e3c", "one atlas in each major: %d, %d",
          int(entries(h1, NC_ATLAS).size()), int(entries(h99, NC_ATLAS).size()));
    check(hash_rect(region(kX[0])) == g_ref && hash_rect(region(kX[1])) == g_ref, "e3c", "both draw the scene");
    stop(z);
    stop(a);
    free_lib(z);
    free_lib(a);
    check(!h99 || h99->live_entries == 0, "e3c", "major 99 entries left");
    cache_empty("e3c");
}

// ---- e4 -------------------------------------------------------------------------------------------

void t_e4() {
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(a);
    scene(a, kX[0]);
    frame("e4");
    frame("e4");
    long long d[2] = {0, 0};
    for (int noshare = 0; noshare < 2; ++noshare) {
        // once to warm up the heap and the driver, then measured: b joins and draws its first frame
        start(b, noshare ? "noshare" : nullptr);
        scene(b, kX[1]);
        frame("e4");
        stop(b);
        // what b alone held (nothing, when it shares a's keys) goes after the grace time
        advance(NUICACHE_GRACE_MS + 1000);
        frame("e4");
        SIZE_T const before = private_usage();
        start(b, noshare ? "noshare" : nullptr);
        scene(b, kX[1]);
        frame("e4");
        d[noshare] = (long long)private_usage() - (long long)before;
        check(hash_rect(region(kX[1])) == g_ref, "e4", "b draws the scene (noshare %d)", noshare);
        stop(b);
    }
    std::printf("  b's first frame: shared %+.2f MiB, noshare %+.2f MiB\n", d[0] / 1048576.0, d[1] / 1048576.0);
    check(d[1] >= (2 << 20), "e4", "noshare delta %.2f MiB < 2 MiB: the measure does not see the atlas", d[1] / 1048576.0);
    check(d[0] * 4 < d[1], "e4", "shared delta %.2f MiB not under 25%% of noshare %.2f MiB", d[0] / 1048576.0, d[1] / 1048576.0);
    stop(a);
    free_lib(b);
    free_lib(a);
    cache_empty("e4");
}

// ---- e5 -------------------------------------------------------------------------------------------

void t_e5() {
    IDirect3DDevice8* d1 = g_dev;
    IDirect3DDevice8* d2 = make_device();
    if (!check(d2 != nullptr, "e5", "second device")) return;
    ULONG const d2_base = refs(d2);
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(a);
    start(b);
    scene(a, kX[0]);
    scene(b, kX[1]);
    frame("e5");
    NcHeader* h = cache_map(1);
    LONG const gen0 = h->gen;
    std::vector<int> const at = entries(h, NC_ATLAS, 6);
    if (!check(at.size() == 1, "e5", "one atlas")) return;
    // keep the old atlas alive ourselves, so a module that misses the change uses a live object
    // (the check below sees it) instead of a freed one
    auto* old = static_cast<IUnknown*>(nc_detail::entries(h)[at[0]].obj);
    old->AddRef();
    std::string const old_ptr = hex(old);
    // the first frame on the new device with no layers (the atlas is still acquired, nothing is drawn):
    // a module that kept the old device's textures shows here, before it could draw with one
    lua("A.layer(1) B.layer(1)");
    g_dev = d2;
    frame("e5");
    check(h->gen == gen0 + 1, "e5", "gen %ld -> %ld", long(gen0), long(h->gen));
    bool const moved = check(lua("return A.status().cache.atlas_ptr") != old_ptr, "e5", "a still uses the old device's atlas") &
                       check(lua("return B.status().cache.atlas_ptr") != old_ptr, "e5", "b still uses the old device's atlas");
    if (moved) {
        scene(a, kX[0]);
        scene(b, kX[1]);
        frame("e5");
    }
    bool all_d2 = true;
    for (std::uint32_t kind = NC_ATLAS; kind <= NC_PICTURE; ++kind)
        for (int i : entries(h, kind)) all_d2 = all_d2 && nc_detail::entries(h)[i].device == d2;
    check(all_d2, "e5", "an entry not on the new device");
    check(hash_rect(region(kX[0])) == g_ref && hash_rect(region(kX[1])) == g_ref, "e5", "both draw right on the new device");
    old->Release();
    check(refs(d1) == g_dev_base, "e5", "old device refcount %lu, baseline %lu", refs(d1), g_dev_base);
    stop(b);
    stop(a);
    free_lib(b);
    free_lib(a);
    check(refs(d2) == d2_base, "e5", "new device refcount %lu, baseline %lu", refs(d2), d2_base);
    g_dev = d1;
    d2->Release();
    // the mapping still names d2 as the device: the next frame on d1 is a device change again
    cache_empty("e5");
}

// ---- e7 + fmt (golden) -------------------------------------------------------------------------------

std::string g_golden_path;
bool g_update = false;
std::map<std::string, std::string> g_golden, g_found;

void golden_load() {
    FILE* f = std::fopen(g_golden_path.c_str(), "rb");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char key[128], val[256];
        if (std::sscanf(line, " \"%127[^\"]\" : \"%255[^\"]\"", key, val) == 2 || std::sscanf(line, " \"%127[^\"]\" : %255[0-9]", key, val) == 2)
            g_golden[key] = val;
    }
    std::fclose(f);
}
void golden_save() {
    FILE* f = std::fopen(g_golden_path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", g_golden_path.c_str()); return; }
    std::fprintf(f, "{\n  \"note\": \"test/test_render.exe --update-golden; a hash may change only with its kind's fmt (src/cache.hpp)\"");
    for (auto const& kv : g_found) {
        bool const num = kv.first.size() > 4 && kv.first.compare(kv.first.size() - 4, 4, ".fmt") == 0;
        std::fprintf(f, ",\n  \"%s\": %s%s%s", kv.first.c_str(), num ? "" : "\"", kv.second.c_str(), num ? "" : "\"");
    }
    std::fprintf(f, "\n}\n");
    std::fclose(f);
    std::printf("  wrote %s (%d keys)\n", g_golden_path.c_str(), int(g_found.size()));
}
std::string hex64(std::uint64_t v) { char b[24]; std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)v); return b; }

// a kind's pixels against the golden: same fmt -> same hash, or the fmt was not bumped
void golden_kind(char const* kind, std::uint32_t fmt, char const* key, std::uint64_t hash) {
    std::string const fk = std::string(kind) + ".fmt";
    g_found[fk] = std::to_string(fmt);
    g_found[key] = hex64(hash);
    if (g_update) return;
    auto gf = g_golden.find(fk), gh = g_golden.find(key);
    if (!check(gf != g_golden.end() && gh != g_golden.end(), "fmt", "%s missing from golden_fmt.json (--update-golden)", key)) return;
    if (gf->second != std::to_string(fmt)) {
        check(false, "fmt", "%s fmt is %u, golden %s: the decode changed on purpose, run --update-golden", kind, fmt, gf->second.c_str());
        return;
    }
    check(gh->second == hex64(hash), "fmt", "%s pixels changed (%s, golden %s) with the same fmt %u: bump kFmt in src/cache.hpp "
          "(or the game's DATs changed: --update-golden after checking)", key, hex64(hash).c_str(), gh->second.c_str(), fmt);
}

int const kIcons[] = {4165, 12345, 16384, 20000, 28000, 13};

void t_e7() {
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(a);
    start(b);
    std::string icons;
    for (int i = 0; i < 6; ++i) {
        char ln[96];
        std::snprintf(ln, sizeof(ln), "%sI\t%d\t%d\t32\t%d\tFFFFFFFF", i ? "\\n" : "", 40 + 40 * i, 700, kIcons[i]);
        icons += ln;
    }
    lua("return A.layer(2, 0, '%s')", icons.c_str());
    lua("return B.layer(2, 0, '%s')", icons.c_str());
    frame("e7");
    NcHeader* h = cache_map(1);
    LONG const both = bit(a) | bit(b);
    std::uint64_t hall = 0xcbf29ce484222325ull;
    int n = 0;
    for (int id : kIcons) {
        bool const has = lua("return A.icon(%d)", id) == "true";
        std::vector<int> const e = entries(h, NC_ITEM_ICON, id);
        if (!has) { check(e.empty(), "e7", "icon %d has no art but an entry", id); continue; }
        ++n;
        if (!check(e.size() == 1, "e7", "icon %d: %d entries", id, int(e.size()))) continue;
        NcEntry& en = nc_detail::entries(h)[e[0]];
        check(en.holders == both && en.w == 32 && en.h == 32, "e7", "icon %d holders %08lx size %ux%u", id, (unsigned long)en.holders,
              en.w, en.h);
        int w = 0, hh = 0;
        std::vector<std::uint32_t> const px = tex_pixels(en.obj, w, hh);
        std::string const want = lua("local s = A.icon_pixels(%d) return s and s:gsub('.', function(c) return ('%%02x'):format(c:byte()) end) or 'nil'", id);
        std::string got;
        for (std::uint32_t p : px) {
            char b4[12];
            std::snprintf(b4, sizeof(b4), "%02x%02x%02x%02x", p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, p >> 24);
            got += b4;
        }
        check(got == want, "e7", "icon %d texture differs from icon_pixels", id);
        hall = nc_fnv1a64(px.data(), px.size() * 4, hall);
    }
    check(n >= 5, "e7", "only %d of the test icons exist", n);
    std::vector<int> const any = entries(h, NC_ITEM_ICON);
    if (!any.empty()) golden_kind("icon", nc_detail::entries(h)[any[0]].fmt, "icon.hash", hall);
    // the picture: fmt and pixels
    scene(a, kX[0]);
    frame("e7");
    std::vector<int> const pe = entries(h, NC_PICTURE);
    if (check(pe.size() == 1, "e7", "%d picture entries", int(pe.size()))) {
        int w = 0, hh = 0;
        std::vector<std::uint32_t> const px = tex_pixels(nc_detail::entries(h)[pe[0]].obj, w, hh);
        check(w == 64 && hh == 48, "e7", "picture %dx%d, want 64x48", w, hh);
        golden_kind("picture", nc_detail::entries(h)[pe[0]].fmt, "picture.hash", nc_fnv1a64(px.data(), px.size() * 4));
    }
    // nobody draws the icons: unheld, alive for the grace time, found again within it, then released
    lua("A.layer(1) A.layer(2) B.layer(2)");
    frame("e7");
    std::vector<int> const unheld = entries(h, NC_ITEM_ICON);
    bool held = false;
    for (int i : unheld) held = held || nc_detail::entries(h)[i].holders != 0;
    check(int(unheld.size()) == n && !held, "e7", "after the layers went: %d entries, held %d", int(unheld.size()), int(held));
    double const creates0 = luan("return A.status().cache.creates");
    lua("return A.layer(2, 0, '%s')", icons.c_str());
    frame("e7");
    check(luan("return A.status().cache.creates") == creates0, "e7", "icons made again within the grace time");
    lua("A.layer(2)");
    frame("e7");
    advance(NUICACHE_GRACE_MS + 100);
    frame("e7");
    check(entries(h, NC_ITEM_ICON).empty(), "e7", "%d icon entries left after the grace time", int(entries(h, NC_ITEM_ICON).size()));
    // every window type's atlas: fmt and pixels
    for (int t = 1; t <= 8; ++t) {
        lua("return A.wintype(%d)", t);
        frame("e7");
        std::vector<int> const ae = entries(h, NC_ATLAS, t);
        if (!check(ae.size() == 1, "fmt", "window type %d: %d atlas entries", t, int(ae.size()))) continue;
        int w = 0, hh = 0;
        std::vector<std::uint32_t> const px = tex_pixels(nc_detail::entries(h)[ae[0]].obj, w, hh);
        char key[32];
        std::snprintf(key, sizeof(key), "atlas.t%d", t);
        golden_kind("atlas", nc_detail::entries(h)[ae[0]].fmt, key, nc_fnv1a64(px.data(), px.size() * 4));
    }
    lua("return A.wintype(6)");
    stop(b);
    stop(a);
    free_lib(b);
    free_lib(a);
    cache_empty("e7");
}

// ---- e8 -------------------------------------------------------------------------------------------

void t_e8() {
    Mod a, c;
    if (!load(a, "nuitest_a.dll", "A")) return;
    // 1. noshare draws identically, with private textures only
    start(a, "noshare");
    scene(a, kX[0]);
    frame("e8");
    check(hash_rect(region(kX[0])) == g_ref, "e8", "noshare pixels differ");
    check(lua("local c = A.status().cache return tostring(c.shared) .. ' ' .. c.shared_textures .. ' ' .. tostring(c.private_textures > 0)")
              == "false 0 true", "e8", "noshare cache status %s", lua("local c = A.status().cache return tostring(c.shared) .. ' ' .. c.why").c_str());
    stop(a);
    // 2. a full table: the new texture is private, and still drawn
    start(a);
    NcHeader* h = cache_map(1);
    std::vector<int> mine;
    for (int i = 0; i < NUICACHE_ENTRIES; ++i) {
        NcEntry& e = nc_detail::entries(h)[i];
        if (e.state != 0) continue;
        e.kind = 99;           // an unknown kind and object type: never matched, never released by anyone
        e.obj_type = 0;
        e.holders = LONG(1) << 30;
        e.state = 2;
        mine.push_back(i);
    }
    scene(a, kX[0]);
    frame("e8");
    check(hash_rect(region(kX[0])) == g_ref, "e8", "full table: not drawn as the reference");
    check(luan("return A.status().cache.private_textures") >= 1, "e8", "full table: no private texture");
    stop(a);
    for (int i : mine) {
        NcEntry& e = nc_detail::entries(h)[i];
        e.holders = 0;
        e.kind = 0;
        e.state = 0;
    }
    // 3. a lock held by a client that is gone is broken
    LONG const breaks = h->lock_breaks;
    h->lock = nc_detail::make_token(31, 777);
    start(a);
    scene(a, kX[0]);
    frame("e8");
    check(h->lock_breaks > breaks && hash_rect(region(kX[0])) == g_ref, "e8", "stale lock: breaks %ld -> %ld", long(breaks), long(h->lock_breaks));
    stop(a);
    free_lib(a);
    (void)c;
    cache_empty("e8");
}

// ---- wt (E11) --------------------------------------------------------------------------------------

void t_wt() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    int const x = 100, y = 100, w = 900, hh = 300;
    for (int t = 1; t <= 8; ++t) {
        lua("A.wintype(%d) return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\t')", t, x, y, w, hh);
        frame("wt");
        // the rules are tiled from a 32 px piece, the body from a 128 px tile with colours that change
        // only from top to bottom: every row repeats every 128 px away from the corners. A rule drawn
        // as one quad (window type 1's recipe spells its pieces "MENU    hfr1") runs across the atlas
        // into the font sheet and does not.
        int bad = 0, rows = 0;
        {
            Back b;
            for (int yy = y - 16; yy < y + hh + 16; ++yy) {
                if (yy > y + 24 && yy < y + hh - 24) continue;
                std::uint32_t const* r = b.row(yy);
                ++rows;
                for (int xx = x + 64; xx + 128 < x + w - 64; ++xx)
                    if ((r[xx] & 0xFFFFFFu) != (r[xx + 128] & 0xFFFFFFu)) { ++bad; break; }
            }
        }
        check(bad == 0, "wt", "window type %d: %d of %d edge rows do not repeat (a rule drawn from the wrong texels)", t, bad, rows);
        char key[32];
        std::snprintf(key, sizeof(key), "frame.t%d", t);
        std::uint64_t const hsh = hash_rect(Rect {x - 24, y - 24, x + w + 24, y + hh + 24});
        g_found[key] = hex64(hsh);
        if (!g_update) {
            auto g = g_golden.find(key);
            check(g != g_golden.end() && g->second == hex64(hsh), "wt", "window type %d crop %s, golden %s", t, hex64(hsh).c_str(),
                  g == g_golden.end() ? "missing" : g->second.c_str());
        }
    }
    lua("A.wintype(6)");
    stop(a);
    free_lib(a);
    cache_empty("wt");
}

// ---- fr (1.2.2): every window type's frame stays inside its rectangle ------------------------------

int luma(std::uint32_t c) { return int((c >> 16) & 255) + int((c >> 8) & 255) + int(c & 255); }

// the brightest row (from y0 .. y1 - 1) at column x: a rule's light line
int rule_row(int x, int y0, int y1) {
    int best = -1, bl = -1;
    for (int yy = y0; yy < y1; ++yy) {
        int const l = luma(pixel(x, yy));
        if (l > bl) { bl = l; best = yy; }
    }
    return best;
}

void t_fr() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    int const x = 100, y = 100, w = 900, hh = 300, m = 8;
    for (int t = 1; t <= 8; ++t) {
        lua("A.wintype(%d) return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\t')", t, x, y, w, hh);
        frame("fr");
        // nothing outside the rectangle: an m px ring around it is background (the bottom rule of before
        // 1.2.2 drew row y + hh in every type but 6)
        int const out = inked(Rect {x - m, y - m, x + w + m, y}) + inked(Rect {x - m, y + hh, x + w + m, y + hh + m}) +
                        inked(Rect {x - m, y, x, y + hh}) + inked(Rect {x + w, y, x + w + m, y + hh});
        check(out == 0, "fr", "window type %d: %d px drawn outside its rectangle, %d of them on row y + h", t, out,
              inked(Rect {x - m, y + hh, x + w + m, y + hh + 1}));
        // the bottom rule's light row in the middle meets its corners' (the side rules fade to alpha 8 / 128
        // and are too faint to locate the same way)
        int const mid = rule_row(x + w / 2, y + hh - 6, y + hh + 2);
        int const cl = rule_row(x + 12, y + hh - 6, y + hh + 2), cr = rule_row(x + w - 12, y + hh - 6, y + hh + 2);
        check(mid == cl && mid == cr, "fr", "window type %d: bottom rule on row h%+d, its corners on h%+d / h%+d", t,
              mid - y - hh, cl - y - hh, cr - y - hh);
        std::printf("  type %d: outside %d px, bottom rule h%+d (corners h%+d h%+d)\n", t, out, mid - y - hh, cl - y - hh,
                    cr - y - hh);
        // type 6 against the game's own log window (4K screenshot 2026.10.09_12.57.05, rows 1818..2143): its
        // rule on 2142, one outer row below it
        if (t == 6)
            check(mid == y + hh - 2, "fr", "window type 6: bottom rule on row h%+d, the game's log window has it on h-2", mid - y - hh);
    }
    lua("A.wintype(6)");
    stop(a);
    free_lib(a);
    cache_empty("fr");
}

// ---- fb (1.2.3): the frame drawn as the game draws its log window ---------------------------------
// Measured on 4K captures of the game's own chat log in all eight window types: every win00 quad's
// alpha is 1.5 x its recipe value (0x80 = 1.5), clamped per pixel after the interpolation; rules and corners add
// to what is under them in seven types ((Cs - 0) x As + Cd, the record's blend bytes) and are blended in type 7.

// a column of pixels drawn over black, then over white
struct TwoFills { std::vector<std::uint32_t> k, w; };
TwoFills two_fills(Rect r) {
    TwoFills out;
    for (std::uint32_t bg : {0x000000u, 0xFFFFFFu}) {
        g_fill = bg;
        frame("fb");
        Back b;
        std::vector<std::uint32_t>& v = bg ? out.w : out.k;
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) v.push_back(b.ok ? b.row(y)[x] & 0xFFFFFFu : 0);
    }
    g_fill = BG;
    return out;
}

void t_fb() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    int const x = 100, y = 100, w = 600, hh = 300;
    // the body's recipe alpha, top -> bottom ('menu    win00' sprite 0); 0 = drawn without blending (2, 4, 6)
    int const top[9] = {0, 32, 0, 32, 0, 32, 0, 96, 32}, bot[9] = {0, 96, 0, 96, 0, 64, 0, 112, 96};
    Rect const r {x + 200, y, x + 201, y + hh};
    for (int t = 1; t <= 8; ++t) {
        lua("A.wintype(%d) return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\t')", t, x, y, w, hh);
        TwoFills const f = two_fills(r);
        int bad = 0, worst = 0;
        for (int yy = 8; yy < hh - 8; yy += 4) {            // clear of the top and bottom rules
            std::uint32_t const k = f.k[std::size_t(yy)], wh = f.w[std::size_t(yy)];
            double const tm = double(int((wh >> 8) & 255) - int((k >> 8) & 255)) / 255.0;   // green: never clamped
            double const al = top[t] ? std::min(1.0, (top[t] + (bot[t] - top[t]) * (yy + 0.5) / hh) * 3.0 / 255.0) : 1.0;
            int const d = int(std::lround(std::fabs(tm - (1.0 - al)) * 255));
            worst = std::max(worst, d);
            bad += d > 3;
        }
        check(bad == 0, "fb", "window type %d: body transmission off the 1.5 x alpha model by up to %d levels (%d rows)", t, worst, bad);
        std::printf("  type %d: body transmission within %d levels of 1 - min(1, 1.5 a)\n", t, worst);
    }
    // the rules add: type 6's top rule is a gold texel row over an opaque black one; added, the black row
    // leaves the body as it is (the game's log window: row y + 1 gold, y + 2 body); blended, it darkens it
    {
        lua("A.wintype(6) return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\t')", x, y, w, hh);
        frame("fb");
        std::uint32_t const gold = pixel(x + 300, y + 1), under = pixel(x + 300, y + 2), body = pixel(x + 300, y + 4);
        int const d = std::abs(luma(under) - luma(body));
        check(d <= 6, "fb", "window type 6: the row under the gold rule differs from the body by %d (luma sum): blended, not added", d);
        check(luma(gold) > luma(body) + 150, "fb", "window type 6: gold rule %06x not lit over the body %06x", unsigned(gold), unsigned(body));
        std::printf("  type 6: rule row %06x, row under it %06x, body %06x\n", unsigned(gold), unsigned(under), unsigned(body));
    }
    lua("A.wintype(6)");
    stop(a);
    free_lib(a);
    cache_empty("fb");
}

// ---- ti (1.2.3): window titles as the game's log window draws "Window2" -----------------------------

void t_ti() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    int const x = 100, y = 100, w = 600, hh = 120;
    // "Window2": advances 9 4 8 6 7 10 7 (the log window's, fitted on 4K shots): 51 px ending at w - 47
    int const tx1 = x + w - 47, tx0 = tx1 - 51;
    double const tw = luan("local m = A.metrics() if not m.wtitle then return -1 end local s = 0 "
                           "for c in ('Window2'):gmatch('.') do s = s + m.wtitle[c:byte() - 31] end return s");
    check(tw == 51, "ti", "metrics().wtitle: 'Window2' is %g px wide, not 51", tw);
    lua("A.wintype(6) return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\tWindow2')", x, y, w, hh);
    frame("ti");
    // the glyphs: inked above the window (their ascenders) from the title's first column to its last, nowhere else
    int lo = W, hi = -1;
    for (int yy = y - 3; yy < y; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            if (pixel(xx, yy) != BG) { lo = std::min(lo, xx); hi = std::max(hi, xx); }
    // (the last glyph's cell may reach one pixel past its advance: the game's '2' does)
    check(lo >= tx0 && lo <= tx0 + 1 && hi <= tx1 && hi >= tx1 - 6, "ti", "title inked over x %d..%d, the game's %d..%d", lo - x, hi - x,
          tx0 - x, tx1 - x);
    // the rule's light row (green: the gold adds without clamping): solid 8 px before the corner, fading in over
    // the 16 px after the title, fading out over the 16 px before it, clear between. The game's log window at
    // 4K: out 132 .. 5 over its 16 px, in 1 .. 132 (red, linear), a clear stretch the title's width
    auto gr = [&](int xx) { return int((pixel(xx, y + 1) >> 8) & 255); };
    int const solid = gr(x + w - 28), body = int((pixel(x + w - 40, y + 6) >> 8) & 255), span = solid - body;
    int const in8 = gr(x + w - 40), out8 = gr(tx0 - 7);
    check(gr(x + w - 32) >= solid - span / 8 && in8 > body + span / 3 && in8 < solid - span / 3 && gr(x + w - 48) < body + span / 8,
          "ti", "fade-in: green %d at w-48, %d at w-40, %d at w-32 (solid %d, body %d)", gr(x + w - 48), in8, gr(x + w - 32), solid, body);
    check(out8 > body + span / 3 && out8 < solid - span / 3 && gr(tx0 - 17) >= solid - span / 8 && gr(tx0) < body + span / 8, "ti",
          "fade-out: green %d at the title - 17, %d at - 7, %d at its first column (solid %d, body %d)", gr(tx0 - 17), out8, gr(tx0),
          solid, body);
    // the fill: 0x60 x the font's white (about 250), 188 +- 6, whatever the body
    int best = 0;
    for (int yy = y - 3; yy < y + 6; ++yy)
        for (int xx = tx0; xx < tx1; ++xx) best = std::max(best, int(pixel(xx, yy) & 255));
    check(best >= 182 && best <= 194, "ti", "title fill's brightest blue %d, the game's 0.75 x 250", best);
    std::printf("  title x %d..%d (game %d..%d), rule green solid %d, fade-in %d, fade-out %d, body %d, fill %d\n", lo - x, hi - x,
                tx0 - x, tx1 - x, solid, in8, out8, body, best);
    // the M command draws the same glyphs at the same place
    std::uint64_t const own = hash_rect(Rect {tx0 - 2, y - 4, tx1 + 2, y});
    lua("return A.layer(1, 0, 'W\\t%d\\t%d\\t%d\\t%d\\t\\nM\\t%d\\t%d\\tC0C0C0FF\\tWindow2')", x, y, w, hh, tx0, y);
    frame("ti");
    check(hash_rect(Rect {tx0 - 2, y - 4, tx1 + 2, y}) == own, "ti", "M draws the title differently from the frame's own");
    lua("A.wintype(6)");
    stop(a);
    free_lib(a);
    cache_empty("ti");
}

// ---- gr (1.1.0): grey quads and icons ---------------------------------------------------------------

// the stage states a game may leave on stages 1-3 (and the texture factor) before EndScene, and a string
// of them: a grey batch uses all of these, and the frame must hand every one back
IDirect3DTexture8* g_odd_tex = nullptr;
void set_grey_game_state() {
    if (!g_odd_tex) g_dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_odd_tex);
    g_dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0x12345678u);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG0, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    g_dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    g_dev->SetTextureStageState(1, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
    g_dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_ADD);
    g_dev->SetTextureStageState(2, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(2, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_dev->SetTextureStageState(2, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_dev->SetTexture(1, g_odd_tex);
    g_dev->SetTexture(2, g_odd_tex);
}
std::string grey_game_state() {
    std::string out;
    char b[24];
    DWORD v = 0;
    g_dev->GetRenderState(D3DRS_TEXTUREFACTOR, &v);
    std::snprintf(b, sizeof(b), "tf %08lx", v);
    out += b;
    g_dev->GetTextureStageState(0, D3DTSS_COLORARG0, &v);
    std::snprintf(b, sizeof(b), " a0 %lu", v);
    out += b;
    D3DTEXTURESTAGESTATETYPE const st[] = {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1,
                                           D3DTSS_ALPHAARG2, D3DTSS_MINFILTER, D3DTSS_MAGFILTER, D3DTSS_MIPFILTER, D3DTSS_ADDRESSU,
                                           D3DTSS_ADDRESSV, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS};
    for (int s = 1; s <= 3; ++s) {
        out += " |";
        for (auto t : st) {
            g_dev->GetTextureStageState(s, t, &v);
            std::snprintf(b, sizeof(b), " %lu", v);
            out += b;
        }
        if (s < 3) {
            IDirect3DBaseTexture8* t = nullptr;
            g_dev->GetTexture(s, &t);
            out += t == g_odd_tex ? " odd" : t ? " other" : " null";
            if (t) t->Release();
        }
    }
    return out;
}

// what the grey stages give for a texel (ARGB) under diffuse d (ARGB), blended over BG; luma weights
// are the texture factor's (0.5 + w / 2 in 8 bits)
std::uint32_t grey_expect(std::uint32_t t, std::uint32_t d, bool old_copy = false) {
    double const r = (t >> 16) & 0xFF, g = (t >> 8) & 0xFF, b = t & 0xFF;
    double L;
    if (old_copy) {
        // a consumer's offline grey copy before 1.1.0: PIL 'L' (ITU-R 601-2), then x 0.62,
        // drawn with a white diffuse; compared with the new mode under a 158 grey diffuse
        L = std::floor((r * 299 + g * 587 + b * 114) / 1000 + 0.5);
        L = std::floor(L * 0.62 + 0.5);
        d = (d & 0xFF000000u) | 0xFFFFFFu;
    } else {
        double const wr = (166 / 255.0 - 0.5) * 2, wg = (202 / 255.0 - 0.5) * 2, wb = (142 / 255.0 - 0.5) * 2;
        L = r * wr + g * wg + b * wb;
    }
    double const a = ((t >> 24) & 0xFF) / 255.0 * (((d >> 24) & 0xFF) / 255.0);
    std::uint32_t out = 0;
    for (int s = 0; s < 24; s += 8) {
        double const src = std::min(255.0, L * (((d >> s) & 0xFF) / 255.0));
        double const bg = (BG >> s) & 0xFF;
        out |= std::uint32_t(std::lround(src * a + bg * (1 - a))) << s;
    }
    return out;
}
std::uint32_t colour_expect(std::uint32_t t) {
    double const a = ((t >> 24) & 0xFF) / 255.0;
    std::uint32_t out = 0;
    for (int s = 0; s < 24; s += 8)
        out |= std::uint32_t(std::lround(((t >> s) & 0xFF) * a + ((BG >> s) & 0xFF) * (1 - a))) << s;
    return out;
}
int chan_diff(std::uint32_t a, std::uint32_t b) {
    int m = 0;
    for (int s = 0; s < 24; s += 8) m = std::max(m, std::abs(int((a >> s) & 0xFF) - int((b >> s) & 0xFF)));
    return m;
}
// the largest channel difference between the frame at (x, y) and f(texel) over a w x h texture drawn 1:1
template <typename F>
int max_diff(std::vector<std::uint32_t> const& px, int w, int h, int x, int y, F f) {
    Back b;
    if (!b.ok) return 999;
    int m = 0;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) m = std::max(m, chan_diff(b.row(y + yy)[x + xx] & 0xFFFFFFu, f(px[std::size_t(yy) * w + xx])));
    return m;
}

void t_gr() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    std::string const hs = lua("local h, w, hh = A.tex_load([[%s\\pic.png]], 64, 64) return h .. ' ' .. w .. ' ' .. hh", g_fix.c_str());
    int h = 0, w = 0, hh = 0;
    if (!check(std::sscanf(hs.c_str(), "%d %d %d", &h, &w, &hh) == 3, "gr", "tex_load: %s", hs.c_str())) return;
    int const Y = 700, X[6] = {40, 140, 240, 340, 440, 600};
    auto q = [&](int x, char const* col, int filter, int scale = 1) {
        char ln[160];
        std::snprintf(ln, sizeof(ln), "Q\\t%d\\t%d\\t%d\\t%d\\t%d\\t0\\t0\\t%d\\t%d\\t%s\\t%d", h, x, Y, x + w * scale, Y + hh * scale, w, hh, col, filter);
        return std::string(ln);
    };
    // the colour reference alone first (a frame with no grey batch)
    lua("return A.layer(3, 0, '%s')", q(X[0], "FFFFFFFF", 0).c_str());
    set_grey_game_state();
    std::string const odd = grey_game_state();
    frame("gr");
    check(grey_game_state() == odd, "gr", "a colour-only frame changed stage 1-3 states: %s -> %s", odd.c_str(), grey_game_state().c_str());
    Rect const r0 {X[0], Y, X[0] + w, Y + hh};
    std::uint64_t const ref = hash_rect(r0);
    // colour, grey, colour again (same texture: the batch breaks twice), grey under a 158 diffuse,
    // linear grey at 2x, a grey item icon at half alpha and the same icon in colour
    std::string cmds = q(X[0], "FFFFFFFF", 0) + "\\n" + q(X[1], "FFFFFFFF", 2) + "\\n" + q(X[2], "FFFFFFFF", 0) + "\\n" +
                       q(X[3], "9E9E9EFF", 2) + "\\n" + q(X[4], "FFFFFFFF", 3, 2) +
                       "\\nI\\t600\\t700\\t32\\t4165\\tFFFFFF80\\t2\\nI\\t640\\t700\\t32\\t4165\\tFFFFFFFF";
    lua("return A.layer(3, 0, '%s')", cmds.c_str());
    set_grey_game_state();
    frame("gr");
    check(grey_game_state() == odd, "gr", "grey stage states not restored: %s -> %s", odd.c_str(), grey_game_state().c_str());
    check(luan("return A.status().grey_ok") == 1, "gr", "grey_ok %s on %s", lua("return A.status().grey_ok").c_str(), g_d3d8.c_str());
    // 1.2.0 merges batches: the two colour quads and the two grey quads do not overlap, so they share a
    // batch each; the colour icon still comes after the grey batches (the colour-after-grey restore path)
    check(luan("return A.status().batches") == 5, "gr", "%s batches, want 5 (colour x2 | grey x2 | linear grey | grey icon | icon)",
          lua("return A.status().batches").c_str());
    NcHeader* hd = cache_map(1);
    std::vector<int> const pe = entries(hd, NC_PICTURE);
    if (!check(pe.size() == 1, "gr", "%d picture entries", int(pe.size()))) return;
    int tw = 0, th = 0;
    std::vector<std::uint32_t> const px = tex_pixels(nc_detail::entries(hd)[pe[0]].obj, tw, th);
    if (!check(tw == w && th == hh, "gr", "picture %dx%d", tw, th)) return;
    int const dc = max_diff(px, w, hh, X[0], Y, colour_expect);
    int const dg = max_diff(px, w, hh, X[1], Y, [](std::uint32_t t) { return grey_expect(t, 0xFFFFFFFFu); });
    int const d158 = max_diff(px, w, hh, X[3], Y, [](std::uint32_t t) { return grey_expect(t, 0xFF9E9E9Eu); });
    int const dold = max_diff(px, w, hh, X[3], Y, [](std::uint32_t t) { return grey_expect(t, 0xFF9E9E9Eu, true); });
    std::printf("  max channel difference: colour %d, grey %d, grey x158 %d; grey x158 vs the old grey copy (PIL L x 0.62) %d (%s)\n",
                dc, dg, d158, dold, g_d3d8.c_str());
    check(dc <= 1, "gr", "colour quad differs from its texels by %d", dc);
    check(dg <= 3, "gr", "grey quad differs from the luma by %d", dg);
    check(d158 <= 3, "gr", "grey x158 differs from the luma by %d", d158);
    check(dold <= 4, "gr", "grey x158 differs from the old grey copy by %d", dold);
    check(hash_rect(Rect {X[2], Y, X[2] + w, Y + hh}) == ref, "gr", "the colour quad after a grey batch is not the colour reference");
    check(inked(Rect {X[4], Y, X[4] + 2 * w, Y + 2 * hh}) > w * hh, "gr", "linear grey at 2x drew %d px", inked(Rect {X[4], Y, X[4] + 2 * w, Y + 2 * hh}));
    {
        // the grey icon: luma of icon_pixels at half alpha
        std::string const hx = lua("local s = A.icon_pixels(4165) return s and s:gsub('.', function(c) return ('%%02x'):format(c:byte()) end) or 'nil'");
        if (check(hx.size() == 4096 * 2, "gr", "icon_pixels(4165): %d hex chars", int(hx.size()))) {
            std::vector<std::uint32_t> ip(1024);
            for (int i = 0; i < 1024; ++i) {
                unsigned bb, gg, rr, aa;
                std::sscanf(hx.c_str() + i * 8, "%2x%2x%2x%2x", &bb, &gg, &rr, &aa);
                ip[i] = (aa << 24) | (rr << 16) | (gg << 8) | bb;
            }
            int const di = max_diff(ip, 32, 32, 600, 700, [](std::uint32_t t) { return grey_expect(t, 0x80FFFFFFu); });
            int const dic = max_diff(ip, 32, 32, 640, 700, colour_expect);
            std::printf("  icon 4165: grey at half alpha %d, colour %d\n", di, dic);
            check(di <= 3, "gr", "grey icon differs from the luma by %d", di);
            check(dic <= 1, "gr", "colour icon differs from its texels by %d", dic);
        }
    }
    // the next frame, colour only again: as before any grey was drawn
    lua("return A.layer(3, 0, '%s')", q(X[0], "FFFFFFFF", 0).c_str());
    set_grey_game_state();
    frame("gr");
    check(hash_rect(r0) == ref && inked(Rect {X[1], Y, X[1] + w, Y + hh}) == 0, "gr", "the frame after a grey frame is not the colour reference");
    check(grey_game_state() == odd, "gr", "stage states after the next frame: %s", grey_game_state().c_str());
    // an old command text with an out-of-range filter still draws point colour, as before 1.1.0
    lua("return A.layer(3, 0, '%s')", q(X[0], "FFFFFFFF", 7).c_str());
    frame("gr");
    check(hash_rect(r0) == ref, "gr", "Q filter 7 no longer draws as filter 0");
    lua("A.layer(3)");
    // reset what the odd state left so later tests start from the device's defaults
    g_dev->SetTexture(1, nullptr);
    g_dev->SetTexture(2, nullptr);
    g_dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_dev->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_DISABLE);
    if (g_odd_tex) g_odd_tex->Release();     // it holds a reference on the device
    g_odd_tex = nullptr;
    stop(a);
    free_lib(a);
    cache_empty("gr");
}

// ---- fpu (1.1.0): the test device keeps Lua's double precision -----------------------------------------

void t_fpu() {
    // without D3DCREATE_FPU_PRESERVE, D3D8 leaves the thread's FPU at 24-bit precision and Lua numbers above
    // 2^24 (mob ids 0x01xxxxxx) collide: 17293316 + 1 would print 17293316
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    lua("return A.layer(1, 0, 'R\\t10\\t10\\t20\\t20\\tFFFFFFFF')");
    frame("fpu");
    std::string const s = lua("local a = tonumber('17293316') return tostring(a + 1) .. ' ' .. tostring(a * 3 - 2 * a)");
    check(s == "17293317 17293316", "fpu", "after a frame, 17293316 + 1 and 3a - 2a give %s (single-precision FPU)", s.c_str());
    unsigned short cw = 0;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    check(((cw >> 8) & 3) != 0, "fpu", "FPU precision control 0 (24-bit) after a frame");
    lua("A.layer(1)");
    stop(a);
    free_lib(a);
}

// ---- 1.2.0: the back-buffer rule (bb) ----------------------------------------------------------------

// A lockable render target the size of a game effect target, filled with the background
struct MidTarget {
    IDirect3DSurface8* rt = nullptr;
    IDirect3DSurface8* bb = nullptr;
    IDirect3DSurface8* zs = nullptr;
    MidTarget() {
        D3DSURFACE_DESC d {};
        g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb);
        if (bb) bb->GetDesc(&d);
        g_dev->CreateRenderTarget(256, 256, d.Format, D3DMULTISAMPLE_NONE, TRUE, &rt);
        if (FAILED(g_dev->GetDepthStencilSurface(&zs))) zs = nullptr;
    }
    ~MidTarget() {
        if (rt) rt->Release();
        if (bb) bb->Release();
        if (zs) zs->Release();
    }
    // pixels of rt that are not the background
    int inked() {
        D3DLOCKED_RECT lr {};
        if (!rt || FAILED(rt->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return -1;
        int n = 0;
        for (int y = 0; y < 256; ++y) {
            auto const* row = reinterpret_cast<std::uint32_t const*>(static_cast<char const*>(lr.pBits) + y * lr.Pitch);
            for (int x = 0; x < 256; ++x) n += (row[x] & 0xFFFFFFu) != BG;
        }
        rt->UnlockRect();
        return n;
    }
    // what 0x100096B0 does mid-frame: the wrapper with a texture target bound (then Clear, BeginScene)
    int call() {
        D3DLOCKED_RECT lr {};
        if (rt && SUCCEEDED(rt->LockRect(&lr, nullptr, 0))) {
            for (int y = 0; y < 256; ++y) {
                auto* row = reinterpret_cast<std::uint32_t*>(static_cast<char*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < 256; ++x) row[x] = 0xFF000000u | BG;
            }
            rt->UnlockRect();
        }
        g_dev->SetRenderTarget(rt, nullptr);
        harness::g_renderer.device = g_dev;
        g_dev->BeginScene();
        harness::run(g_site);
        int const n = inked();
        g_dev->SetRenderTarget(bb, zs);
        return n;
    }
};

void t_bb() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    scene(a, kX[0]);
    frame("bb");
    MidTarget mid;
    if (!check(mid.rt != nullptr && mid.bb != nullptr, "bb", "no mid-frame target")) return;
    // 1. three mid-frame calls and the final one: one draw, on the back buffer only
    lua("A.prof()");
    double const d0 = luan("return A.status().drawn");
    int inked_mid = 0;
    for (int i = 0; i < 3; ++i) inked_mid += mid.call();
    frame("bb");
    check(inked_mid == 0, "bb", "a mid-frame call drew %d px into a texture target", inked_mid);
    check(luan("return A.status().drawn") == d0 + 1, "bb", "%.0f draws for one frame of 4 calls", luan("return A.status().drawn") - d0);
    check(hash_rect(region(kX[0])) == g_ref, "bb", "the back buffer is not the reference");
    check(lua("local p = A.prof() return p.calls .. ' ' .. p.calls_bb .. ' ' .. p.calls_other .. ' ' .. p.draws") == "4 1 3 1", "bb",
          "prof calls/bb/other/draws: %s", lua("local p = A.status() return p.calls").c_str());
    check(luan("return A.status().rt_w") == W, "bb", "status().rt_w %.0f: the size of a mid-frame target", luan("return A.status().rt_w"));
    check(lua("local s = A.status() return s.prof_rule .. ' ' .. tostring(s.bb_fallback)") == "bb false", "bb", "status rule %s",
          lua("local s = A.status() return s.prof_rule").c_str());
    // 2. only mid-frame calls for 680 ms: after 500 ms every call draws (the fallback)
    int last = 0;
    for (int i = 0; i < 40; ++i) {
        advance(17);
        last = mid.call();
    }
    check(last > 0, "bb", "no call had the back buffer for 680 ms and nothing was drawn (no fallback)");
    check(lua("local s = A.status() return s.prof_rule .. ' ' .. tostring(s.bb_fallback)") == "all true", "bb", "fallback not reported: %s",
          lua("local s = A.status() return s.prof_rule").c_str());
    check(lua("local p = A.prof() return p.bb_fallback_n .. ' ' .. tostring(p.fallback_draws > 0 and p.fallback_draws <= 12)") == "1 true", "bb",
          "prof fallback counters");
    // 3. a back-buffer call again: the rule is back
    frame("bb");
    check(lua("return tostring(A.status().bb_fallback)") == "false", "bb", "still in the fallback after a back-buffer call");
    check(mid.call() == 0, "bb", "a mid-frame call drew after the back buffer came back");
    check(hash_rect(region(kX[0])) == g_ref, "bb", "the back buffer is not the reference after the fallback");
    stop(a);
    free_lib(a);
}

// ---- 1.2.0: the vertex cache (vc) --------------------------------------------------------------------

void t_vc() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    std::uint32_t const red = 0xFF0000;
    // 1. the list changes (same vertex count): drawn from the new list
    lua("return A.layer(3, 0, 'R\\t100\\t100\\t140\\t140\\tFF0000FF')");
    frame("vc");
    check(pixel(120, 120) == red && pixel(220, 120) == BG, "vc", "first list: %06x %06x", pixel(120, 120), pixel(220, 120));
    lua("A.prof()");
    for (int i = 0; i < 5; ++i) frame("vc");
    check(luan("return A.prof().xform_builds") == 0, "vc", "an unchanged layer transformed again on every frame");
    lua("return A.layer(3, 0, 'R\\t200\\t100\\t240\\t140\\tFF0000FF')");
    frame("vc");
    check(pixel(220, 120) == red && pixel(120, 120) == BG, "vc", "a changed list drawn from the old vertices: %06x %06x", pixel(220, 120),
          pixel(120, 120));
    lua("return A.layer_pos(3, 0, 50)");
    frame("vc");
    check(pixel(220, 170) == red && pixel(220, 120) == BG, "vc", "a moved list drawn in the old place");
    // 2. the scale changes: everything at twice the size
    lua("return A.screen(%d, %d, false, 2)", W, H);
    frame("vc");
    check(pixel(440, 340) == red && pixel(220, 170) == BG, "vc", "scale 2 drawn with the scale-1 vertices: %06x %06x", pixel(440, 340),
          pixel(220, 170));
    lua("return A.screen(%d, %d, false, 1)", W, H);
    frame("vc");
    check(pixel(220, 170) == red && pixel(440, 340) == BG, "vc", "scale 1 again drawn with the scale-2 vertices");
    lua("A.layer(3)");
    // 3. a texture arrives under a list that drew without it: its UVs normalised by its size
    char const* q = "Q\\t2\\t300\\t300\\t364\\t348\\t0\\t0\\t64\\t48\\tFFFFFFFF\\t0";
    lua("return A.layer(4, 0, '%s')", q);
    frame("vc");
    check(inked(Rect {290, 290, 380, 360}) == 0, "vc", "a quad drawn without its texture");
    std::string const h = lua("return (A.tex_load([[%s\\pic.png]], 64, 64))", g_fix.c_str());
    check(h == "2", "vc", "tex_load gave handle %s, the test needs 2", h.c_str());
    frame("vc");
    std::uint64_t const arrived = hash_rect(Rect {290, 290, 380, 360});
    lua("return A.layer(4, 0, '%s')", q);          // the same list again, built anew: the reference
    frame("vc");
    std::uint64_t const fresh = hash_rect(Rect {290, 290, 380, 360});
    check(arrived == fresh && inked(Rect {290, 290, 380, 360}) > 1000, "vc", "the texture that arrived is drawn with stale UVs");
    lua("A.layer(4)");
    stop(a);
    free_lib(a);
}

// ---- 1.2.0: layer_pos at the same place builds nothing (lp) ------------------------------------------

void t_lp() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    std::uint32_t const red = 0xFF0000;
    lua("return A.layer(5, 0, 'R\\t10\\t10\\t50\\t50\\tFF0000FF')");
    lua("A.prof()");
    lua("return A.layer_pos(5, 100, 600)");
    lua("return A.layer_pos(5, 100, 600)");
    check(lua("local p = A.prof() return p.builds .. ' ' .. p.pos_same .. ' ' .. p.layer_pos") == "1 1 2", "lp",
          "the same place twice: builds/same/calls %s", lua("return 0").c_str());
    frame("lp");
    check(pixel(130, 630) == red, "lp", "moved layer not drawn: %06x", pixel(130, 630));
    lua("return A.layer_pos(5, 100, 600, 0, 0, 20, 20)");                 // a clip, same place
    frame("lp");
    check(pixel(115, 615) == red && pixel(130, 630) == BG, "lp", "a clip at the same place was ignored: %06x %06x", pixel(115, 615),
          pixel(130, 630));
    lua("return A.layer_pos(5, 100, 600, 0, 0, 20, 20)");
    lua("return A.layer_pos(5, 100, 600, 0, 0, 30, 30)");                 // a different clip
    frame("lp");
    check(pixel(125, 625) == red && pixel(135, 635) == BG, "lp", "a changed clip was ignored");
    lua("return A.layer_pos(5, 100, 600)");                               // the clip removed
    frame("lp");
    check(pixel(130, 630) == red, "lp", "removing the clip was ignored");
    check(lua("local p = A.prof() return p.builds .. ' ' .. p.pos_same") == "3 1", "lp", "builds/same after the clips: %s",
          lua("return 0").c_str());
    lua("A.layer(5)");
    stop(a);
    free_lib(a);
}

// ---- 1.2.0: icons are read on the worker, never on the render thread (ic) ----------------------------

void t_ic() {
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(a);
    std::string icons;
    for (int i = 0; i < 6; ++i) {
        char ln[96];
        std::snprintf(ln, sizeof(ln), "%sI\t%d\t%d\t32\t%d\tFFFFFFFF", i ? "\\n" : "", 40 + 40 * i, 700, kIcons[i]);
        icons += ln;
    }
    Rect const row {30, 690, 300, 742};
    // 1. new ids: cut on the worker (the Lua thread) before the lock
    lua("A.prof()");
    lua("return A.layer(2, 0, '%s')", icons.c_str());
    frame("ic");
    std::uint64_t const ref = hash_rect(row);
    check(lua("local p = A.prof() return p.reads_render .. ' ' .. tostring(p.reads_worker >= 5)") == "0 true", "ic",
          "new icons: render reads %s", lua("return 0").c_str());
    // 2. icons back after the grace time (their textures released): pixels cut again on the worker
    lua("A.layer(2)");
    frame("ic");
    advance(NUICACHE_GRACE_MS + 100);
    frame("ic");
    check(entries(cache_map(1), NC_ITEM_ICON).empty(), "ic", "icon entries left after the grace time");
    lua("A.prof()");
    lua("return A.layer(2, 0, '%s')", icons.c_str());
    frame("ic");
    check(hash_rect(row) == ref, "ic", "returning icons not drawn as before");
    check(lua("return A.prof().reads_render") == "0", "ic", "returning icons read on the render thread");
    // 3. pixels the worker did not decode because a module held them: the atlas at start(). b holds it
    //    when a starts; b goes and the frame's sweep releases it before a acquired it. The render thread
    //    asks the worker, which reads it on its next call (mode 0); a worker that never calls again:
    //    after a second the render thread reads it itself, counted (mode 1)
    for (int mode = 0; mode < 2; ++mode) {
        lua("A.layer(2)");
        stop(a);
        frame("ic");
        advance(NUICACHE_GRACE_MS + 100);
        frame("ic");
        start(b);
        lua("return B.layer(2, 0, '%s')", icons.c_str());
        frame("ic");                                   // b makes the atlas and the icons
        start(a);                                      // b holds the atlas: a decodes none
        lua("return A.layer(2, 0, '%s')", icons.c_str());
        lua("B.layer(2) A.prof()");
        stop(b);
        advance(NUICACHE_GRACE_MS + 100);
        frame("ic");                                   // the sweep releases the atlas; a has no pixels: it asks
        check(inked(row) == 0, "ic", "mode %d: drawn although nobody had the atlas's pixels", mode);
        if (mode == 0) {
            check(lua("local p = A.prof() return p.reads_render .. ' ' .. tostring(p.wants >= 1)") == "0 true", "ic",
                  "atlas without pixels: render reads / wants %s", lua("return 0").c_str());
            lua("return A.status().calls");            // any call: the worker supplies the pixels
            frame("ic");
            check(hash_rect(row) == ref, "ic", "the atlas supplied by the worker: not drawn as before");
            check(lua("return A.prof().reads_render") == "0", "ic", "the render thread read the atlas the worker supplied");
        } else {
            for (int i = 0; i < 4; ++i) { advance(300); frame("ic"); }    // no call into A for 1.2 s
            check(hash_rect(row) == ref, "ic", "an atlas the worker never supplied is not drawn after a second");
            check(luan("return A.prof().reads_render") >= 1, "ic", "the render thread's own read is not counted");
        }
    }
    lua("A.layer(2)");
    stop(a);
    free_lib(b);
    free_lib(a);
}

// ---- 1.2.0: the worker builds outside the lock while the render thread draws (lk) --------------------

volatile LONG g_lk_stop = 0;
volatile LONG g_lk_frames = 0, g_lk_torn = 0, g_lk_bad = 0;
DWORD WINAPI lk_render(void*) {
    // probes: the first, a middle and the last quad of the layer's grid
    int const px[3][2] = {{42, 42}, {42 + 6 * 25, 42 + 6 * 15}, {42 + 6 * 49, 42 + 6 * 29}};
    while (!g_lk_stop) {
        harness::g_renderer.device = g_dev;
        g_dev->BeginScene();
        harness::run(g_site);
        std::uint32_t c[3] = {0, 0, 0};
        {
            Back b;
            if (b.ok)
                for (int i = 0; i < 3; ++i) c[i] = b.row(px[i][1])[px[i][0]] & 0xFFFFFFu;
        }
        bool const same = c[0] == c[1] && c[1] == c[2];
        bool const known = c[0] == 0xFF0000 || c[0] == 0x00FF00;
        if (!same) InterlockedIncrement(&g_lk_torn);
        else if (!known && c[0] != BG) InterlockedIncrement(&g_lk_bad);
        InterlockedIncrement(&g_lk_frames);
    }
    return 0;
}

void t_lk() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    // two 1,500-quad layers, every quad red or every quad green; the background of each frame is the
    // last one's (no fill), so a frame shows exactly one of them or a torn mix
    lua("local function grid(col) local t = {} for y = 0, 29 do for x = 0, 49 do t[#t + 1] = ('R\\t%%d\\t%%d\\t%%d\\t%%d\\t%%s'):format("
        "40 + 6 * x, 40 + 6 * y, 44 + 6 * x, 44 + 6 * y, col) end end return table.concat(t, '\\n') end "
        "LK_R, LK_G = grid('FF0000FF'), grid('00FF00FF')");
    lua("return A.layer(6, 0, LK_R)");
    frame("lk");
    lua("A.prof()");
    g_lk_stop = 0;
    g_lk_frames = g_lk_torn = g_lk_bad = 0;
    HANDLE th = CreateThread(nullptr, 0, &lk_render, nullptr, 0, nullptr);
    if (!check(th != nullptr, "lk", "no render thread")) return;
    std::string sends = lua("local n = 0 for i = 1, 300 do if A.layer(6, 0, i %% 2 == 0 and LK_R or LK_G) then n = n + 1 end end return n");
    g_lk_stop = 1;
    WaitForSingleObject(th, 30000);
    CloseHandle(th);
    std::string const p = lua("local p = A.prof() return ('%%d %%d %%.1f %%.1f %%d %%.1f %%d %%d'):format(p.draws, p.waits_over_50, p.wait_max_us, "
                              "p.w_wait_max_us, p.w_waits_over_50, p.w_hold_max_us, p.render_tid, p.worker_tid)");
    std::printf("  %ld frames on a second thread while 300 layers were sent; draws, render waits > 50 us, render wait max, "
                "worker wait max, worker waits > 50 us, worker hold max, tids: %s\n", long(g_lk_frames), p.c_str());
    int draws = 0, w50 = 0;
    double wmax = 0;
    std::sscanf(p.c_str(), "%d %d %lf", &draws, &w50, &wmax);
    check(sends == "300", "lk", "%s of 300 sends", sends.c_str());
    check(g_lk_frames >= 50 && draws >= 50, "lk", "only %ld frames (%d draws) while sending", long(g_lk_frames), draws);
    check(g_lk_torn == 0 && g_lk_bad == 0, "lk", "%ld torn frames, %ld wrong colours", long(g_lk_torn), long(g_lk_bad));
    check(w50 <= 2, "lk", "the render thread waited over 50 us %d times (max %.0f us): the worker holds the lock while it builds", w50, wmax);
    lua("A.layer(6)");
    frame("lk");
    stop(a);
    free_lib(a);
}

// ---- bench (C.8) -------------------------------------------------------------------------------------

void t_bench() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    start(a);
    std::string ids = lua("local t = {} for id = 4096, 6000 do if #t < 16 and A.icon(id) then t[#t + 1] = id end end return table.concat(t, ',')");
    std::string icons = lua("local t, i = {}, 0 for id in ('%s'):gmatch('%%d+') do t[#t + 1] = ('I\\t%%d\\t600\\t32\\t%%s\\tFFFFFFFF'):format(40 + 34 * i, id) i = i + 1 end return table.concat(t, '\\n')", ids.c_str());
    std::string quads = lua("local h = A.tex_load([[%s\\pic.png]], 64, 64) local t = {} for i = 0, 15 do t[#t + 1] = ('Q\\t%%d\\t%%d\\t600\\t%%d\\t632\\t0\\t0\\t32\\t32\\tFFFFFFFF\\t0'):format(h, 40 + 34 * i, 72 + 34 * i) end return table.concat(t, '\\n')", g_fix.c_str());
    double us[2] = {0, 0}, batches[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        lua("return A.layer(1, 0, [[%s]])", k == 0 ? icons.c_str() : quads.c_str());
        for (int i = 0; i < 20; ++i) frame("bench");
        int const n = 400;
        for (int i = 0; i < n; ++i) {
            frame("bench");
            us[k] += luan("return A.status().last_us");
        }
        us[k] /= n;
        batches[k] = luan("return A.status().batches");
    }
    std::printf("  16 icons, one texture each: %.0f batches, %.1f us a frame; 16 quads of one texture: %.0f batch, %.1f us"
                " -> %.2f us per extra batch (%s)\n", batches[0], us[0], batches[1], us[1], (us[0] - us[1]) / 15, g_d3d8.c_str());
    check(batches[0] == 16 && batches[1] == 1, "bench", "batches %.0f / %.0f", batches[0], batches[1]);
    stop(a);
    free_lib(a);
}

// ---- 1.2.1: pictures held from the cache or decoded on the decode thread (pd) ------------------------

std::string g_big;                       // --big: make_big_png.py's 2048 x 2048 PNG

DWORD WINAPI pd_frame_main(void* tag) {
    frame(static_cast<char const*>(tag));
    return 0;
}
// One frame on another thread than the Lua calls, as in game (the render thread is never the Lua
// thread, so no draw waits for a decode there).
void frame_render_thread(char const* tag) {
    HANDLE th = CreateThread(nullptr, 0, &pd_frame_main, const_cast<char*>(tag), 0, nullptr);
    if (!check(th != nullptr, tag, "no render thread")) return;
    WaitForSingleObject(th, 30000);
    CloseHandle(th);
}

// the 512 x 512 picture quad at x (Lua global PD_<var>: the handle)
std::string pd_layer(Mod const& m, char const* hvar, int x, int size = 512) {
    return lua("return %s.layer(1, 0, ('Q\\t%%d\\t%%d\\t80\\t%%d\\t%%d\\t0\\t0\\t%d\\t%d\\tFFFFFFFF\\t1'):format(%s, %d, %d, %d))",
               m.var.c_str(), size, size, hvar, x, x + size, 80 + size);
}
Rect pd_area(int x, int size = 512) { return Rect {x, 80, x + size, 80 + size}; }

void t_pd() {
    if (!check(!g_big.empty() && GetFileAttributesA(g_big.c_str()) != INVALID_FILE_ATTRIBUTES, "pd",
               "no --big picture (make_big_png.py): %s", g_big.c_str()))
        return;
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(b);
    int const xa = 40, xb = 640;

    // 1. b loads the picture nobody holds: tex_load reads, hashes and sizes it only; the decode thread
    //    decodes it; frames drawn on a render thread meanwhile skip its quad, then show it. Nothing
    //    is read or decoded on the render thread or the Lua thread.
    lua("B.prof()");
    std::string const r1 = lua("local t = B.us() local h, w, hh = B.tex_load([[%s]], 512, 512) PD_DT = B.us() - t PD_B = h "
                               "return tostring(h) .. ' ' .. tostring(w) .. ' ' .. tostring(hh)", g_big.c_str());
    double const dt_first = luan("return PD_DT");
    check(r1.find(" 512 512") != std::string::npos, "pd", "tex_load of the big picture: %s", r1.c_str());
    pd_layer(b, "PD_B", xb);
    int frames = 0;
    for (; frames < 400 && inked(pd_area(xb)) == 0; ++frames) {
        frame_render_thread("pd");
        if (inked(pd_area(xb)) == 0) Sleep(2);
    }
    std::uint64_t const ref = hash_rect(pd_area(xb));
    check(inked(pd_area(xb)) > 0, "pd", "the picture never arrived from the decode thread (%d frames)", frames);
    std::string const p1 = lua("local p = B.prof() return ('%%d %%d %%d %%d %%d %%.0f %%.0f'):format(p.decodes_worker, p.decodes_bg, "
                               "p.reads_render, p.wants, p.decode_failed, p.us_decode_bg_max, p.us_tex_load_max)");
    int dw = -1, dbg = -1, rr = -1, wants = -1, dfail = -1;
    double dec_us = 0, tl_us = 0;
    std::sscanf(p1.c_str(), "%d %d %d %d %d %lf %lf", &dw, &dbg, &rr, &wants, &dfail, &dec_us, &tl_us);
    std::printf("  first load: tex_load %.0f us, decode on the decode thread %.0f us, %d render-thread frames before it showed\n",
                dt_first, dec_us, frames);
    check(dw == 0 && dbg == 1 && rr == 0 && wants == 0 && dfail == 0, "pd",
          "first load: decodes on the Lua thread %d (0), on the decode thread %d (1), render reads %d, wants %d, failed %d", dw, dbg, rr,
          wants, dfail);
    double const budget = std::max(3000.0, 0.25 * dec_us);
    check(dec_us > 0 && dt_first <= budget, "pd", "first load: tex_load took %.0f us (limit %.0f: a quarter of the %.0f us decode)",
          dt_first, budget, dec_us);

    // 2. a loads the picture b holds: held (nc_hold), not decoded, as cheap as the read. Then b lets it
    //    go and the grace time passes; b's stop() sweeps every unheld entry before a's first frame (the
    //    1.1.0 blank). a's hold keeps it: a's first frame, on the render thread, draws it at once.
    start(a);
    lua("A.prof()");
    lua("local t = A.us() PD_A = A.tex_load([[%s]], 512, 512) PD_DT = A.us() - t", g_big.c_str());
    double const dt_held = luan("return PD_DT");
    std::string const p2 = lua("local p = A.prof() return ('%%d %%d %%d'):format(p.holds, p.decodes_worker, p.decodes_bg)");
    std::printf("  held load: tex_load %.0f us (%s holds / decodes here / decodes on the decode thread)\n", dt_held, p2.c_str());
    check(p2 == "1 0 0", "pd", "a picture b holds: holds / decodes here / decodes on the decode thread %s (1 0 0)", p2.c_str());
    check(dt_held <= budget, "pd", "a picture b holds: tex_load took %.0f us (limit %.0f)", dt_held, budget);
    pd_layer(a, "PD_A", xa);
    lua("B.tex_free(PD_B) B.layer(1)");
    advance(NUICACHE_GRACE_MS + 100);
    stop(b);                                           // nc_stop sweeps: past the grace time, unheld = gone
    std::vector<int> const pe = entries(cache_map(1), NC_PICTURE);
    bool const kept = check(pe.size() == 1 && (nc_detail::entries(cache_map(1))[pe[0]].holders & bit(a)) != 0, "pd",
                            "the picture a holds was swept before a's first frame (%d entries)", int(pe.size()));
    if (kept) {
        frame_render_thread("pd");
        check(hash_rect(pd_area(xa)) == ref, "pd", "a's first frame after the sweep does not show the picture");
        std::string const p3 = lua("local p = A.prof() return ('%%d %%d %%d %%d'):format(p.held_attached, p.wants, p.reads_render, p.decodes_bg)");
        check(p3 == "1 0 0 0", "pd", "a's first frame: holds attached / wants / render reads / decodes %s (1 0 0 0)", p3.c_str());
    }

    // 3. wintype: b on type 3 draws a window; a switches to type 3 while b holds that atlas: held, no
    //    decode. b goes, the sweep runs, a's first frame draws the type-3 window at once.
    start(b);
    Rect const wr {700, 640, 1060, 940};
    lua("B.wintype(3) B.layer(2, 0, 'W\\t700\\t640\\t360\\t300\\tType 3')");
    frame("pd");
    std::uint64_t const wref = hash_rect(Rect {wr.x0 - 16, wr.y0 - 16, wr.x1 + 16, wr.y1 + 16});
    lua("A.prof()");
    lua("local t = A.us() A.wintype(3) PD_DT = A.us() - t");
    double const dt_wt = luan("return PD_DT");
    std::string const p4 = lua("local p = A.prof() return ('%%d %%d'):format(p.holds, p.decodes_worker)");
    std::printf("  wintype(3) while b holds that atlas: %.0f us (%s holds / decodes here)\n", dt_wt, p4.c_str());
    check(p4 == "1 0", "pd", "wintype(3) while b holds that atlas: holds / decodes here %s (1 0)", p4.c_str());
    lua("B.layer(2) A.layer(1) A.layer(2, 0, 'W\\t700\\t640\\t360\\t300\\tType 3')");
    advance(NUICACHE_GRACE_MS + 100);
    stop(b);
    std::vector<int> const ae = entries(cache_map(1), NC_ATLAS, 3);
    if (check(ae.size() == 1 && (nc_detail::entries(cache_map(1))[ae[0]].holders & bit(a)) != 0, "pd",
              "the type-3 atlas a holds was swept before a's first frame")) {
        frame_render_thread("pd");
        check(hash_rect(Rect {wr.x0 - 16, wr.y0 - 16, wr.x1 + 16, wr.y1 + 16}) == wref, "pd",
              "a's first frame in type 3 after the sweep does not show the window as b drew it");
        check(lua("local p = A.prof() return p.wants .. ' ' .. p.reads_render") == "0 0", "pd", "a's type-3 frame asked for pixels");
    }
    lua("A.layer(2) A.wintype(6)");
    frame("pd");

    // 4. an offline frame on the Lua thread right after tex_load shows the picture (the harness rule
    //    that keeps the consumers' QA frames as they were)
    lua("PD_A2 = A.tex_load([[%s]], 256, 256)", g_big.c_str());
    pd_layer(a, "PD_A2", xa, 256);
    frame("pd");
    check(inked(pd_area(xa, 256)) > 256 * 256 * 9 / 10, "pd", "a frame on the Lua thread right after tex_load: %d of %d pixels drawn",
          inked(pd_area(xa, 256)), 256 * 256);

    // 5. a picture freed while it is decoded: dropped, nothing left; stop() while one is decoded:
    //    returns once the thread is gone; a body that does not decode: never drawn, counted
    lua("A.layer(1) A.tex_free(PD_A) A.tex_free(PD_A2) A.prof()");
    lua("local h = A.tex_load([[%s]], 500, 500) A.tex_free(h)", g_big.c_str());
    frame("pd");
    std::string const p5 = lua("local p = A.prof() return ('%%d %%d'):format(p.decodes_pending, p.decode_threads)");
    check(lua("return A.status().cpu_pixels_kb") == "0", "pd", "pixels of a freed picture kept: %s KB", lua("return A.status().cpu_pixels_kb").c_str());
    check(p5 == "0 0", "pd", "after a picture freed while decoded: pending / threads %s (0 0)", p5.c_str());
    std::string bad = g_big + ".cut.png";
    {
        FILE* in = std::fopen(g_big.c_str(), "rb");
        FILE* out = std::fopen(bad.c_str(), "wb");
        std::vector<char> buf(100000);
        std::size_t const n = in ? std::fread(buf.data(), 1, buf.size(), in) : 0;
        if (out) std::fwrite(buf.data(), 1, n, out);
        if (in) std::fclose(in);
        if (out) std::fclose(out);
    }
    lua("A.prof() PD_BAD = A.tex_load([[%s]], 512, 512)", bad.c_str());
    pd_layer(a, "PD_BAD", xa);
    frame("pd");
    check(inked(pd_area(xa)) == 0, "pd", "a picture whose body does not decode drew %d pixels", inked(pd_area(xa)));
    check(lua("local p = A.prof() return p.decode_failed .. ' ' .. p.reads_render") == "1 0", "pd", "an undecodable body: failed / render reads %s (1 0)",
          lua("return 0").c_str());
    lua("A.layer(1) A.tex_free(PD_BAD)");

    // 1.2.4 (review REND-1): a header declaring more than 16 megapixels is refused by tex_load before any decode (a small
    // file can declare 15000 x 15000 and stb decodes at full size). prepare reads the header only: a signature, an IHDR and
    // the first IDAT's chunk header (stb's header scan runs to the first IDAT, looking for tRNS).
    std::string const huge = g_big + ".huge.png";
    auto write_ihdr = [](std::string const& path, unsigned w, unsigned h) {
        unsigned char png[41] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        for (int i = 0; i < 4; ++i) {
            png[16 + i] = (unsigned char)(w >> (24 - 8 * i));
            png[20 + i] = (unsigned char)(h >> (24 - 8 * i));
        }
        png[24] = 8;      // bit depth; colour type 2 (RGB) next
        png[25] = 2;
        std::memcpy(png + 33, "    IDAT", 8);   // after IHDR's 13 data bytes and its CRC: an empty IDAT
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) { std::fwrite(png, 1, sizeof(png), f); std::fclose(f); }
    };
    write_ihdr(huge, 5000, 5000);
    std::string const rh = lua("A.prof() local h, why = A.tex_load([[%s]], 512, 512) "
                               "return tostring(h) .. '|' .. tostring(why) .. '|' .. A.prof().decodes_bg", huge.c_str());
    check(rh.rfind("nil|", 0) == 0 && rh.find("too large") != std::string::npos && rh.substr(rh.size() - 2) == "|0", "pd",
          "a 5000 x 5000 header: tex_load must refuse it before a decode: %s", rh.c_str());
    write_ihdr(huge, 4096, 4096);   // 16 MP, the limit: read (its missing body then fails on the decode thread)
    std::string const rl = lua("local h = A.tex_load([[%s]], 512, 512) if h then A.tex_free(h) end return tostring(h ~= nil)", huge.c_str());
    check(rl == "true", "pd", "a 4096 x 4096 header (16 MP, the limit) was refused at tex_load");
    std::remove(huge.c_str());
    lua("PD_A3 = A.tex_load([[%s]], 400, 400)", g_big.c_str());
    long long const s0 = GetTickCount();
    stop(a);
    std::printf("  stop() with a decode in progress: %lu ms\n", (unsigned long)(GetTickCount() - s0));
    check(lua("return A.prof().decode_threads") == "0", "pd", "a decode thread outlived stop()");

    // 6. FreeLibrary with a decode in progress and no stop(): the thread keeps the module mapped and
    //    unloads it when it is done (no crash, the hook and the cache let go)
    start(a);
    lua("A.tex_load([[%s]], 300, 300)", g_big.c_str());
    free_lib(a);
    bool const mapped = GetModuleHandleA("nuitest_a.dll") != nullptr;
    int waited = 0;
    while (GetModuleHandleA("nuitest_a.dll") && waited < 10000) { Sleep(5); waited += 5; }
    std::printf("  FreeLibrary during a decode: still mapped right after %s, unloaded after %d ms\n", mapped ? "yes" : "no (done already)", waited);
    check(GetModuleHandleA("nuitest_a.dll") == nullptr, "pd", "the module never unloaded after its decode thread finished");
    frame("pd");
    std::remove(bad.c_str());
    free_lib(b);
}

// ---- main -------------------------------------------------------------------------------------------

LONG WINAPI crashed(EXCEPTION_POINTERS* ep) {
    std::printf("CRASH in %s: exception %08lx at %p\n", g_test.c_str(), ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    ExitProcess(3);
}

// ---- 1.2.5: exception barriers (ex) and Releases on the render thread (rt) ---------------------------

// a frame on a new render thread; false when it did not finish within ms (a lock left held). A stuck
// thread is left behind: the test has failed and returns at once.
bool frame_render_thread_within(char const* tag, DWORD ms) {
    HANDLE th = CreateThread(nullptr, 0, &pd_frame_main, const_cast<char*>(tag), 0, nullptr);
    if (!th) return false;
    bool const done = WaitForSingleObject(th, ms) == WAIT_OBJECT_0;
    CloseHandle(th);
    return done;
}

// review HOST-1: an exception (out of memory) on the Lua thread, in a draw and in a create callback
// never reaches std::terminate, leaves no lock held and leaves no cache entry half made
void t_ex() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    std::string const s0 = start(a);
    check(s0.rfind("true", 0) == 0, "ex", "start: %s", s0.c_str());
    scene(a, kX[0]);
    if (!check(frame_render_thread_within("ex", 30000), "ex", "the first frame")) return;

    // 1. a Lua call throws with the lock held: nil plus a reason, and the render thread still draws
    InterlockedExchange(&g_throw_site, 1);
    std::string const r1 = lua("local c, why = A.layer(2, 0, 'T\t10\t10\tFFFFFFFF\tx') return tostring(c) .. ' ' .. tostring(why)");
    check(g_throw_site == 0, "ex", "site 1 (layer, lock held) never reached");
    check(r1 == "nil out of memory", "ex", "a throwing layer() returned '%s'", r1.c_str());
    if (!check(frame_render_thread_within("ex", 5000), "ex", "the render thread waits for a lock the throwing Lua call kept")) return;
    check(lua("return tostring(A.layer(2, 0, 'T\t10\t10\tFFFFFFFF\tx') ~= nil)") == "true", "ex", "layer() after the throw");

    // 2. a draw throws with the lock held: that frame is skipped, the next draws, Lua calls still return
    InterlockedExchange(&g_throw_site, 2);
    if (!check(frame_render_thread_within("ex", 5000), "ex", "a throwing draw did not return")) return;
    check(g_throw_site == 0, "ex", "site 2 (draw, lock held) never reached");
    if (!check(frame_render_thread_within("ex", 5000), "ex", "a later draw waits for the lock a throwing draw kept")) return;
    check(inked(region(kX[0])) > 0, "ex", "nothing drawn after a throwing draw");

    // 3. a create callback throws: the cache entry is not left being made, so the picture still arrives
    InterlockedExchange(&g_throw_site, 3);
    std::string const r3 = lua("local h, w, hh = A.tex_load([[%s/pic.png]], 40, 40) if not h then return 'tex_load ' .. tostring(w) end "
                               "return tostring(A.layer(3, 0, ('Q\t%%d\t500\t420\t540\t460\t0\t0\t%%d\t%%d\tFFFFFFFF\t1'):format(h, w, hh)) ~= nil)",
                               g_fix.c_str());
    check(r3 == "true", "ex", "the picture layer: %s", r3.c_str());
    Rect const pic {500, 420, 540, 460};
    int frames = 0;
    for (; frames < 400; ++frames) {     // drawn before each look: the back buffer holds the last test's frame
        if (!check(frame_render_thread_within("ex", 5000), "ex", "a frame after the throwing create did not return")) return;
        if (inked(pic) > 0) break;
        Sleep(2);
    }
    check(g_throw_site == 0, "ex", "site 3 (create callback) never reached");
    check(inked(pic) > 0, "ex", "the picture never arrived after its create callback threw once (%d frames)", frames);
    double const errs = luan("return A.status().native_errors");
    check(errs >= 3, "ex", "status().native_errors %.0f (3 throws)", errs);
    stop(a);
    free_lib(a);
}

volatile LONG g_rt_run = 0;
DWORD WINAPI rt_frames_main(void*) {
    while (g_rt_run) {
        frame("rt");
        Sleep(1);
    }
    return 0;
}

// review REND-5: the game's device is not multithreaded, so a private texture the Lua thread drops is
// Released by the next draw, and stop() gives everything back on the render thread when it draws
void t_rt() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    std::string const s0 = start(a, "noshare");                   // every texture private
    check(s0.rfind("true", 0) == 0, "rt", "start: %s", s0.c_str());
    scene(a, kX[0]);
    std::string const r = lua("local h, w, hh = A.tex_load([[%s/pic.png]], 48, 48) if not h then return 'tex_load ' .. tostring(w) end RTH = h "
                              "return tostring(A.layer(2, 0, ('Q\t%%d\t620\t420\t668\t468\t0\t0\t%%d\t%%d\tFFFFFFFF\t1'):format(h, w, hh)) ~= nil)",
                              g_fix.c_str());
    check(r == "true", "rt", "the picture layer: %s", r.c_str());
    Rect const pic {620, 420, 668, 468};
    for (int i = 0; i < 400; ++i) {      // drawn before each look: the back buffer holds the last test's frame
        frame_render_thread("rt");
        if (inked(pic) > 0) break;
        Sleep(2);
    }
    if (!check(inked(pic) > 0, "rt", "the picture never drew")) return;

    // 1. freed on the Lua thread: queued, then Released by the next draw
    lua("A.layer(2) A.tex_free(RTH)");
    double const queued = luan("return A.status().pending_releases");
    check(queued >= 1, "rt", "a private texture freed on the Lua thread was not left for the render thread (%.0f queued)", queued);
    frame_render_thread("rt");
    double const left = luan("return A.status().pending_releases");
    check(left == 0, "rt", "%.0f private textures still queued after a draw", left);

    // 2. stop() while the render thread draws: the render thread gives everything back
    double const f0 = luan("return A.status().final_render");
    InterlockedExchange(&g_rt_run, 1);
    HANDLE th = CreateThread(nullptr, 0, &rt_frames_main, nullptr, 0, nullptr);
    Sleep(50);
    stop(a);
    InterlockedExchange(&g_rt_run, 0);
    if (th) {
        WaitForSingleObject(th, 30000);
        CloseHandle(th);
    }
    double const f1 = luan("return A.status().final_render");
    check(f1 == f0 + 1, "rt", "stop() while drawing gave back on the Lua thread (final_render %.0f -> %.0f)", f0, f1);
    free_lib(a);
}

// ---- rv (nui 1.2.6): the review's fixes through the whole host -------------------------------------
// HOST-4: a failed atlas upload is retried after the backoff, and status().drawing says whether the
// module draws; REND-6: the game's vertex stream 0 is bound again after a draw; HOST-8: a handle is
// never reused, not even after stop() and start().
void t_rv() {
    Mod a;
    if (!load(a, "nuitest_a.dll", "A")) return;
    std::string const s0 = start(a);
    check(s0.rfind("true", 0) == 0, "rv", "start: %s", s0.c_str());
    check(lua("return tostring(A.status().drawing)") == "true", "rv", "status().drawing after start: %s",
          lua("return tostring(A.status().drawing)").c_str());
    scene(a, kX[0]);
    frame("rv");
    check(inked(region(kX[0])) > 0, "rv", "the scene was not drawn");

    // REND-6: stream 0 as the game left it
    IDirect3DVertexBuffer8* vb = nullptr;
    if (check(SUCCEEDED(g_dev->CreateVertexBuffer(64 * 24, 0, D3DFVF_XYZ | D3DFVF_NORMAL, D3DPOOL_MANAGED, &vb)) && vb, "rv",
              "no vertex buffer")) {
        g_dev->SetStreamSource(0, vb, 24);
        frame("rv");
        IDirect3DVertexBuffer8* got = nullptr;
        UINT stride = 0;
        g_dev->GetStreamSource(0, &got, &stride);
        check(got == vb && stride == 24, "rv", "stream 0 after a draw: %p stride %u, the game had %p stride 24",
              static_cast<void*>(got), stride, static_cast<void*>(vb));
        if (got) got->Release();
        g_dev->SetStreamSource(0, nullptr, 0);
        vb->Release();
    }

    // HOST-4: the atlas's create fails twice (private textures: one create per try). Before 1.2.6 the
    // window stayed blank until a reload; now it is tried again after each 2 s backoff
    stop(a);
    start(a, "noshare");
    scene(a, kX[0]);
    InterlockedExchange(&g_throw_extra, 1);
    InterlockedExchange(&g_throw_site, 4);
    frame("rv");
    std::string const msg = lua("return A.status().message");
    check(lua("return tostring(A.status().drawing)") == "false" && inked(region(kX[0])) == 0, "rv",
          "after a failed atlas upload: drawing %s, %d px drawn", lua("return tostring(A.status().drawing)").c_str(), inked(region(kX[0])));
    check(msg.find("start again") == std::string::npos, "rv", "status says \"%s\" (start() while running cannot help)", msg.c_str());
    for (int i = 0; i < 3; ++i) frame("rv");
    check(g_throw_site == 4 && g_throw_extra == 0, "rv", "the atlas was tried again within its backoff");
    advance(LONG(NUICACHE_BACKOFF_MS) + 100);
    frame("rv");                          // the second throw: still not drawing
    check(g_throw_site == 0 && lua("return tostring(A.status().drawing)") == "false", "rv", "the retry after the backoff did not run");
    bool drawn = false;
    for (int k = 0; k < 3 && !drawn; ++k) {
        advance(LONG(NUICACHE_BACKOFF_MS) + 100);
        frame("rv");
        drawn = inked(region(kX[0])) > 0;
    }
    InterlockedExchange(&g_throw_site, 0);
    InterlockedExchange(&g_throw_extra, 0);
    check(drawn && hash_rect(region(kX[0])) == g_ref, "rv", "the window never came back after the atlas upload failed (sticky failure)");
    check(lua("return tostring(A.status().drawing)") == "true", "rv", "status().drawing is not true again once the atlas is up");

    // HOST-8: handles keep counting across stop() and start()
    std::string const h1 = lua("return (A.tex_load([[%s\\pic.png]], 30, 30))", g_fix.c_str());
    stop(a);
    start(a);
    std::string const h2 = lua("return (A.tex_load([[%s\\pic.png]], 30, 30))", g_fix.c_str());
    check(std::atof(h2.c_str()) > std::atof(h1.c_str()), "rv", "a handle after stop() and start() is %s, one from before was %s",
          h2.c_str(), h1.c_str());
    check(lua("return tostring(A.status().drawing)") == "true", "rv", "status().drawing after a restart");
    stop(a);
    check(lua("return tostring(A.status().drawing)") == "false", "rv", "status().drawing after stop()");
    free_lib(a);
    cache_empty("rv");
}

// ---- tl (nui 1.2.6, review HOST-10): a picture over 1 MiB is read and hashed on the decode thread, and
// stop() does not wait for a decode in progress ---------------------------------------------------------
void t_tl() {
    std::string const big = g_dir + "\\tl_big.png";
    std::vector<unsigned char> const bytes =
        pngw::encode(800, 500, 4, [](int x, int y) { return 0xFF000000u | std::uint32_t((x * 7) ^ (y * 13)) | (std::uint32_t(x & 255) << 16); });
    if (!check(bytes.size() > (1u << 20) && pngw::save(big, bytes), "tl", "cannot write %s", big.c_str())) return;
    std::uint64_t const hash = nc_fnv1a64(bytes.data(), bytes.size());
    Mod a, b;
    if (!load(a, "nuitest_a.dll", "A") || !load(b, "nuitest_b.dll", "B")) return;
    start(a);
    start(b);
    // 1. a loads it: the Lua thread reads at most 1 MiB; the decode thread reads, hashes and decodes it
    lua("A.prof()");
    std::string const r1 = lua("local h, w, hh = A.tex_load([[%s]], 64, 64) TLA = h return tostring(h) .. ' ' .. tostring(w) .. ' ' .. tostring(hh)",
                               big.c_str());
    check(r1.find(" 64 40") != std::string::npos, "tl", "tex_load of the 1.6 MB picture: %s", r1.c_str());
    double const read = luan("return A.prof().tex_load_read_max");
    check(read > 0 && read <= double(1u << 20), "tl", "tex_load read %.0f bytes of a %u-byte file on the Lua thread (at most 1 MiB)", read,
          unsigned(bytes.size()));
    Rect const ra {40, 700, 104, 740}, rb {140, 700, 204, 740};
    lua("A.layer(4, 0, ('Q\t%%d\t40\t700\t104\t740\t0\t0\t64\t40\tFFFFFFFF\t1'):format(TLA))");
    for (int i = 0; i < 400 && inked(ra) == 0; ++i) {
        frame_render_thread("tl");
        if (inked(ra) == 0) Sleep(2);
    }
    check(inked(ra) > 0, "tl", "the picture never arrived from the decode thread");
    NcHeader* h = cache_map(1);
    int at = -1;
    for (int i : entries(h, NC_PICTURE))
        if (nc_detail::entries(h)[i].hash_lo == std::uint32_t(hash) && nc_detail::entries(h)[i].hash_hi == std::uint32_t(hash >> 32)) at = i;
    check(at >= 0, "tl", "no cache entry keyed by the file's content hash (the decode thread's hash)");
    // 2. b loads the same file: decoded too (no hash yet, so no hold), and its texture is a's
    lua("TLB = B.tex_load([[%s]], 64, 64) B.layer(4, 0, ('Q\t%%d\t140\t700\t204\t740\t0\t0\t64\t40\tFFFFFFFF\t1'):format(TLB))", big.c_str());
    for (int i = 0; i < 400 && inked(rb) == 0; ++i) {
        frame_render_thread("tl");
        if (inked(rb) == 0) Sleep(2);
    }
    int n = 0;
    LONG holders = 0;
    for (int i : entries(h, NC_PICTURE))
        if (nc_detail::entries(h)[i].hash_lo == std::uint32_t(hash) && nc_detail::entries(h)[i].hash_hi == std::uint32_t(hash >> 32)) {
            ++n;
            holders = nc_detail::entries(h)[i].holders;
        }
    check(inked(rb) > 0 && n == 1 && (holders & bit(a)) && (holders & bit(b)), "tl", "b's copy: drawn %d, %d entries, holders %08lx",
          inked(rb), n, (unsigned long)holders);
    // 3. stop() while a decode sleeps in the decode thread: back within its short wait, not the decode's
    InterlockedExchange(&g_decode_ms, 3000);
    lua("A.tex_load([[%s\\pic.png]], 37, 37)", g_fix.c_str());
    Sleep(150);
    DWORD const t0 = GetTickCount();
    stop(a);
    DWORD const waited = GetTickCount() - t0;
    InterlockedExchange(&g_decode_ms, 0);
    check(waited < 1500, "tl", "stop() waited %lu ms for a decode in progress", (unsigned long)waited);
    std::printf("  tl: tex_load read %.0f of %u bytes, stop() with a 3 s decode in progress took %lu ms\n", read, unsigned(bytes.size()),
                (unsigned long)waited);
    for (int i = 0; i < 200 && luan("return A.prof().decode_threads") > 0; ++i) Sleep(50);   // the thread leaves by itself
    stop(b);
    free_lib(a);
    free_lib(b);
    cache_empty("tl");
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crashed);
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5) {
        std::printf("usage: test_render.exe <dll dir> <FFXiMain.dll copy> <ffxi dir> <fixtures> [--only a,b] [--golden f] [--update-golden] [--d3d8 dll]\n");
        return 2;
    }
    g_dir = argv[1];
    g_ffxi = argv[3];
    g_fix = argv[4];
    std::set<std::string> only;
    for (int i = 5; i < argc; ++i) {
        std::string const a = argv[i];
        std::string const v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--only") {
            std::size_t p = 0;
            for (;;) {
                std::size_t const q = v.find(',', p);
                only.insert(v.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            ++i;
        } else if (a == "--golden") { g_golden_path = v; ++i; }
        else if (a == "--update-golden") { g_update = true; }
        else if (a == "--d3d8") { g_d3d8 = v; ++i; }
        else if (a == "--big") { g_big = v; ++i; }
    }
    if (!g_golden_path.empty()) golden_load();
    if (!LoadLibraryA(argv[2])) { std::printf("cannot load %s\n", argv[2]); return 2; }   // unpacks the client code
    if (!lua_init()) { std::printf("no lua5.1.dll\n"); return 2; }
    g_dev = make_device();
    if (!g_dev) { std::printf("no D3D8 device (%s)\n", g_d3d8.c_str()); return 2; }
    g_dev_base = refs(g_dev);
    struct Test { char const* name; void (*fn)(); };
    // e1 first: it draws the reference every other test compares with
    Test const tests[] = {{"e1", t_e1}, {"e2", t_e2}, {"e3c", t_e3c}, {"e4", t_e4}, {"e5", t_e5},
                          {"e7", t_e7}, {"e8", t_e8}, {"wt", t_wt}, {"fr", t_fr}, {"fb", t_fb}, {"ti", t_ti}, {"gr", t_gr}, {"fpu", t_fpu}, {"bb", t_bb},
                          {"vc", t_vc}, {"lp", t_lp}, {"ic", t_ic}, {"lk", t_lk}, {"bench", t_bench}, {"pd", t_pd}, {"ex", t_ex}, {"rt", t_rt},
                          {"rv", t_rv}, {"tl", t_tl}};
    int failed = 0, ran = 0;
    if (only.count("fmt")) only.insert("e7");           // the fmt golden is checked by e7
    for (Test const& t : tests) {
        bool const needed = t.fn == t_e1;   // always: e1 draws the reference the others compare with
        if (!only.empty() && !only.count(t.name) && !needed) continue;
        g_test = t.name;
        int const f0 = g_fail;
        std::printf("== %s\n", t.name);
        t.fn();
        ++ran;
        bool const ok = g_fail == f0;
        if (!ok) ++failed;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
        if (t.fn == t_e1 && g_ref == 0) {
            std::printf("no reference scene: nothing else can be judged\n");
            return 1;
        }
    }
    if (g_update && !g_golden_path.empty()) golden_save();
    std::printf("test_render: %d of %d tests passed, %d checks passed, %d failed\n", ran - failed, ran, g_pass, g_fail);
    return failed ? 1 : 0;
}
