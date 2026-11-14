// cache_mod.cpp - a bare consumer of NuiCache.h for test_cache.exe (nui design E, step 5a).
//
// Built several times from this one file: -DNUITEST_NAME="\"a\"" (separate modules a, b, c, d),
// -DNUICACHE_REV=2, -DNUICACHE_MAJOR=99, -DNUICACHE_ENTRIES=200 (an ABI mismatch), and with one
// -DNUI_FAULT_* for the self-test. It plays the part of a renderer: it keeps "wants" (texture
// keys a layer needs), acquires them through the cache at the top of each frame, and "draws" by
// handing every object it holds to the test's use() callback, which counts any use of an object
// already released. Objects come from the test's create() callback (fake COM objects, or real
// IDirect3DTexture8s on a real D3D8 device), so the cache never knows the difference.
#include <windows.h>
#include <cstdint>
#include <cstring>

struct TEnv {
    void* ctx;
    void*(__cdecl* create)(void* ctx, void* device, std::uint32_t kind, std::uint32_t w, std::uint32_t h,
                           std::uint32_t* bytes);
    void(__cdecl* use)(void* ctx, void* obj);
    volatile LONG* now;                 // the test's clock, shared by every module
    volatile LONG yield_in_lock;        // SwitchToThread inside the cache's critical sections (E6)
};

namespace {
TEnv g_env;
LONG g_zero = 0;
}  // namespace

#define NUICACHE_NOW() (static_cast<DWORD>(*(g_env.now ? g_env.now : &g_zero)))
#define NUICACHE_TEST_YIELD() \
    do { if (g_env.yield_in_lock) SwitchToThread(); } while (0)
// review HOOK-4: every GetModuleHandleEx the cache makes is counted (t_loader_calls)
namespace {
volatile LONG g_loader_calls = 0;
BOOL WINAPI counted_module_handle(DWORD flags, LPCSTR at, HMODULE* out) {
    InterlockedIncrement(&g_loader_calls);
    return GetModuleHandleExA(flags, at, out);
}
}  // namespace
#define GetModuleHandleExA counted_module_handle
#include "../src/NuiCache/NuiCache.h"
#undef GetModuleHandleExA

#ifndef NUITEST_NAME
#define NUITEST_NAME "t"
#endif

namespace {

struct Want {
    bool used;
    NcKey key;
    std::uint16_t w, h;
    NcRef ref;
    char label[32];
};
constexpr int kWants = 512;
Want g_want[kWants];
int g_busy = 0;

void* __cdecl create_cb(void* ctx, void* device, NcKey const* key, NcMade* made) {
    auto* w = static_cast<Want*>(ctx);
    if (!g_env.create) return nullptr;
    std::uint32_t bytes = 0;
    void* obj = g_env.create(g_env.ctx, device, key->kind, w->w, w->h, &bytes);
    made->w = w->w;
    made->h = w->h;
    made->bytes = bytes;
    return obj;
}

}  // namespace

extern "C" {

// opts: "noshare" -> private. Returns 1 when sharing.
__declspec(dllexport) int __cdecl t_start(TEnv const* env, char const* opts) {
    g_env = *env;
    std::memset(g_want, 0, sizeof(g_want));
    return nc_start(NUITEST_NAME, "0.1.0", opts && std::strstr(opts, "noshare")) ? 1 : 0;
}

__declspec(dllexport) void __cdecl t_set_yield(int on) { g_env.yield_in_lock = on; }

__declspec(dllexport) int __cdecl t_want(std::uint32_t kind, std::uint32_t fmt, std::uint32_t a, std::uint32_t b,
                                         std::uint32_t hash_lo, std::uint32_t hash_hi, std::uint32_t w, std::uint32_t h) {
    for (int i = 0; i < kWants; ++i) {
        if (g_want[i].used) continue;
        Want& x = g_want[i];
        std::memset(&x, 0, sizeof(x));
        x.used = true;
        x.key.kind = kind;
        x.key.fmt = fmt;
        x.key.a = a;
        x.key.b = b;
        x.key.hash = (std::uint64_t(hash_hi) << 32) | hash_lo;
        x.w = std::uint16_t(w);
        x.h = std::uint16_t(h);
        x.ref.entry = -1;
        static char const* const names[] = {"?", "atlas", "icon", "pic"};
        wsprintfA(x.label, "%s %lu", kind < 4 ? names[kind] : "kind", static_cast<unsigned long>(a));
        return i;
    }
    return -1;
}

// Acquire one want now, from the given thread role (0 render, 1 Lua) -> NcResult.
__declspec(dllexport) int __cdecl t_acquire(int i, void* device, int thread) {
    if (i < 0 || i >= kWants || !g_want[i].used) return -1;
    Want& x = g_want[i];
    if (x.ref.obj) return 100;      // already held
    return nc_acquire(x.key, device, create_cb, &x, x.label, thread ? NC_LUA : NC_RENDER, &x.ref);
}

// nui 1.2.1: take this module's hold on want i's entry if one is live (nc_hold, Lua thread) -> NcResult.
__declspec(dllexport) int __cdecl t_hold(int i) {
    if (i < 0 || i >= kWants || !g_want[i].used) return -1;
    Want& x = g_want[i];
    if (x.ref.obj) return 100;      // already held
    return nc_hold(x.key, &x.ref);
}

__declspec(dllexport) void __cdecl t_unwant(int i) {
    if (i < 0 || i >= kWants || !g_want[i].used) return;
    nc_release(g_want[i].ref);
    g_want[i].used = false;
}

__declspec(dllexport) void __cdecl t_drop(int i) {     // give the object back, keep wanting it
    if (i < 0 || i >= kWants || !g_want[i].used) return;
    nc_release(g_want[i].ref);
}

// One frame: -1 = skipped, else the number of objects drawn. Busy acquires are counted in t_busy().
__declspec(dllexport) int __cdecl t_frame(void* device) {
    NcFrame const f = nc_frame(device);
    if (f == NC_FRAME_SKIP) return -1;
    if (f == NC_FRAME_RESET)
        for (int i = 0; i < kWants; ++i)
            if (g_want[i].used) nc_release(g_want[i].ref);
    int drawn = 0;
    for (int i = 0; i < kWants; ++i) {
        Want& x = g_want[i];
        if (!x.used) continue;
        if (!x.ref.obj) {
            NcResult const r = nc_acquire(x.key, device, create_cb, &x, x.label, NC_RENDER, &x.ref);
            if (r == NC_BUSY) ++g_busy;
        }
        if (x.ref.obj) {
            if (g_env.use) g_env.use(g_env.ctx, x.ref.obj);
            ++drawn;
        }
    }
    return drawn;
}

__declspec(dllexport) int __cdecl t_busy() { return g_busy; }
__declspec(dllexport) long __cdecl t_loader_calls() { return g_loader_calls; }
__declspec(dllexport) void* __cdecl t_obj(int i) { return i >= 0 && i < kWants && g_want[i].used ? g_want[i].ref.obj : nullptr; }
__declspec(dllexport) int __cdecl t_entry(int i) { return i >= 0 && i < kWants && g_want[i].used ? g_want[i].ref.entry : -2; }

// stop(): private objects are the module's to release; shared refs are simply forgotten (nc_stop
// clears this module's bits everywhere).
__declspec(dllexport) void __cdecl t_stop() {
    for (int i = 0; i < kWants; ++i) {
        if (!g_want[i].used) continue;
        if (g_want[i].ref.obj && g_want[i].ref.entry < 0) nc_release(g_want[i].ref);
        g_want[i].used = false;
    }
    nc_stop();
}

__declspec(dllexport) void __cdecl t_status(NcStatus* s) { nc_status(s); }
__declspec(dllexport) NcHeader* __cdecl t_header() { return nc_detail::local().hdr; }
__declspec(dllexport) int __cdecl t_slot() { return nc_detail::local().slot; }

// Test-only: take the cache lock and keep it (E8: a lock held by a client that then goes away).
__declspec(dllexport) int __cdecl t_hold_lock() {
    nc_detail::Local& L = nc_detail::local();
    return L.hdr && nc_detail::lock(L.hdr, L.token, NC_LUA) ? 1 : 0;
}
__declspec(dllexport) void __cdecl t_drop_lock() {
    nc_detail::Local& L = nc_detail::local();
    if (L.hdr) nc_detail::unlock(L.hdr);
}

// Test-only: leave a `creating` entry for a key, as a creator that dies mid-create would.
__declspec(dllexport) int __cdecl t_fake_creating(std::uint32_t kind, std::uint32_t a) {
    nc_detail::Local& L = nc_detail::local();
    NcHeader* h = L.hdr;
    if (!h || !nc_detail::lock(h, L.token, NC_LUA)) return -1;
    NcEntry* E = nc_detail::entries(h);
    int at = -1;
    for (int i = 0; i < NUICACHE_ENTRIES && at < 0; ++i)
        if (E[i].state == 0) at = i;
    if (at >= 0) {
        nc_detail::clear_entry(E[at]);
        E[at].creator = L.token;
        E[at].kind = kind;
        E[at].a = a;
        E[at].device = h->device;
        E[at].gen = std::uint32_t(h->gen);
        InterlockedExchange(&E[at].state, 1);
    }
    nc_detail::unlock(h);
    return at;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) nc_detach();
    return TRUE;
}
