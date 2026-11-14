// test_cache.cpp - offline tests of NuiCache.h (nui design E1, E2, E3 cache part, E3b, E4, E5, E6, E8).
//
// Loads cache_<x>.dll modules built from cache_mod.cpp (each a separate module with its own copy
// of the cache code, like separate addons) in any order, drives their frames, FreeLibrary's them
// with or without stop(), and inspects the shared mapping directly. Objects are fake COM objects
// (QueryInterface/AddRef/Release with counters, a device pointer they pin, and a dead flag that is
// never cleared, so any use or Release after the last reference is counted), or real
// IDirect3DTexture8s on real D3D8 devices on hidden windows (E4, E5 real).
//
//   test_cache.exe <dir with the DLLs> [--only t1,t2] [--layout layout_nuicache.txt]
//                  [--dump-layout file] [--d3d8 path\D3D8.dll]
// Exit 0 when every selected test passes. Tests: layout e1 e2 evict e3c e5 e6 e8 e4 e5real hold.
// hold (nui 1.2.1): nc_hold on the Lua thread keeps an entry another module made through every sweep
// until it is given back; a key nobody made is NC_FAILED and touches nothing (no create, no purge of
// unheld entries, no create backoff).
#include <windows.h>
#include <psapi.h>
#include <d3d8.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "../src/NuiCache/NuiCache.h"

struct TEnv {
    void* ctx;
    void*(__cdecl* create)(void* ctx, void* device, std::uint32_t kind, std::uint32_t w, std::uint32_t h,
                           std::uint32_t* bytes);
    void(__cdecl* use)(void* ctx, void* obj);
    volatile LONG* now;
    volatile LONG yield_in_lock;
};

// ---- fake COM objects ---------------------------------------------------------------------------

struct Fake {
    void** vtbl;
    volatile LONG refs;
    int is_dev;
    Fake* dev;
    std::uint32_t bytes;
    void* mem;
    volatile LONG dead;
    int id;
};

volatile LONG g_underflow = 0, g_dead_uses = 0, g_uses = 0, g_live_tex = 0, g_create_calls = 0;
CRITICAL_SECTION g_fake_cs;
std::vector<Fake*> g_fakes;   // never freed: a dead object stays readable

long __stdcall f_qi(Fake*, void const*, void** out) { if (out) *out = nullptr; return E_NOINTERFACE; }
unsigned long __stdcall f_addref(Fake* f) {
    if (f->dead) InterlockedIncrement(&g_underflow);
    return static_cast<unsigned long>(InterlockedIncrement(&f->refs));
}
unsigned long __stdcall f_release(Fake* f) {
    if (f->dead || f->refs <= 0) { InterlockedIncrement(&g_underflow); return 0; }
    LONG const r = InterlockedDecrement(&f->refs);
    if (r < 0) InterlockedIncrement(&g_underflow);
    if (r == 0 && !f->is_dev) {
        InterlockedExchange(&f->dead, 1);
        if (f->mem) VirtualFree(f->mem, 0, MEM_RELEASE);
        f->mem = nullptr;
        InterlockedDecrement(&g_live_tex);
        if (f->dev) f_release(f->dev);          // a texture pins its device (the stricter model)
    }
    return static_cast<unsigned long>(r);
}
void* g_vtbl[3] = {reinterpret_cast<void*>(&f_qi), reinterpret_cast<void*>(&f_addref), reinterpret_cast<void*>(&f_release)};

Fake* new_fake(bool dev) {
    auto* f = new Fake {};
    f->vtbl = g_vtbl;
    f->refs = 1;
    f->is_dev = dev ? 1 : 0;
    EnterCriticalSection(&g_fake_cs);
    f->id = int(g_fakes.size());
    g_fakes.push_back(f);
    LeaveCriticalSection(&g_fake_cs);
    return f;
}

// ---- the world the modules see ------------------------------------------------------------------

struct World {
    bool real = false;              // device is an IDirect3DDevice8
    bool alloc_mem = false;         // fake textures commit w*h*4*2 (data + managed shadow), for E4
    volatile LONG fail_creates = 0; // the next n creates fail (address space exhausted)
};
World g_world;
volatile LONG g_now = 5000000;
int(__cdecl* g_lock_in_create)() = nullptr;   // rl: a module's t_hold_lock, called by the next create

void* __cdecl env_create(void* ctx, void* device, std::uint32_t kind, std::uint32_t w, std::uint32_t h, std::uint32_t* bytes) {
    (void)kind;
    auto* W = static_cast<World*>(ctx);
    InterlockedIncrement(&g_create_calls);
    if (auto take = g_lock_in_create) {     // rl: the cache lock is taken by another module during the create
        g_lock_in_create = nullptr;
        take();
    }
    *bytes = w * h * 4;
    if (W->fail_creates > 0) { InterlockedDecrement(&W->fail_creates); return nullptr; }
    if (W->real) {
        auto* d = static_cast<IDirect3DDevice8*>(device);
        IDirect3DTexture8* t = nullptr;
        if (FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t))) return nullptr;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
            for (std::uint32_t y = 0; y < h; ++y) std::memset(static_cast<char*>(lr.pBits) + y * lr.Pitch, int(y), w * 4);
            t->UnlockRect(0);
        }
        return t;
    }
    Fake* f = new_fake(false);
    f->dev = static_cast<Fake*>(device);
    f->bytes = *bytes;
    if (f->dev) f_addref(f->dev);
    if (W->alloc_mem) {
        f->mem = VirtualAlloc(nullptr, std::size_t(*bytes) * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (f->mem) std::memset(f->mem, 0x5A, std::size_t(*bytes) * 2);
    }
    InterlockedIncrement(&g_live_tex);
    return f;
}

void __cdecl env_use(void* ctx, void* obj) {
    auto* W = static_cast<World*>(ctx);
    InterlockedIncrement(&g_uses);
    if (!W->real && static_cast<Fake*>(obj)->dead) InterlockedIncrement(&g_dead_uses);
}

TEnv g_env {&g_world, &env_create, &env_use, &g_now, 0};

void advance(LONG ms) { InterlockedExchangeAdd(&g_now, ms); }

// ---- modules ------------------------------------------------------------------------------------

std::string g_dir;

struct Mod {
    HMODULE h = nullptr;
    std::string file;
    int(__cdecl* start)(TEnv const*, char const*) = nullptr;
    void(__cdecl* set_yield)(int) = nullptr;
    int(__cdecl* want)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                       std::uint32_t, std::uint32_t) = nullptr;
    int(__cdecl* acquire)(int, void*, int) = nullptr;
    int(__cdecl* hold)(int) = nullptr;
    void(__cdecl* unwant)(int) = nullptr;
    void(__cdecl* drop)(int) = nullptr;
    int(__cdecl* frame)(void*) = nullptr;
    int(__cdecl* busy)() = nullptr;
    void*(__cdecl* obj)(int) = nullptr;
    int(__cdecl* entry)(int) = nullptr;
    void(__cdecl* stop)() = nullptr;
    void(__cdecl* status)(NcStatus*) = nullptr;
    NcHeader*(__cdecl* header)() = nullptr;
    int(__cdecl* slot)() = nullptr;
    int(__cdecl* hold_lock)() = nullptr;
    void(__cdecl* drop_lock)() = nullptr;
    int(__cdecl* fake_creating)(std::uint32_t, std::uint32_t) = nullptr;
    long(__cdecl* loader_calls)() = nullptr;
};

template <class T>
void bind(HMODULE m, T& fn, char const* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!fn) { std::printf("missing export %s\n", name); std::fflush(stdout); ExitProcess(4); }
}

bool load(Mod& m, char const* letter) {
    m = Mod {};
    m.file = g_dir + "\\cache_" + letter + ".dll";
    m.h = LoadLibraryA(m.file.c_str());
    if (!m.h) { std::printf("cannot load %s (%lu)\n", m.file.c_str(), GetLastError()); return false; }
    bind(m.h, m.start, "t_start");
    bind(m.h, m.set_yield, "t_set_yield");
    bind(m.h, m.want, "t_want");
    bind(m.h, m.acquire, "t_acquire");
    bind(m.h, m.hold, "t_hold");
    bind(m.h, m.unwant, "t_unwant");
    bind(m.h, m.drop, "t_drop");
    bind(m.h, m.frame, "t_frame");
    bind(m.h, m.busy, "t_busy");
    bind(m.h, m.obj, "t_obj");
    bind(m.h, m.entry, "t_entry");
    bind(m.h, m.stop, "t_stop");
    bind(m.h, m.status, "t_status");
    bind(m.h, m.header, "t_header");
    bind(m.h, m.slot, "t_slot");
    bind(m.h, m.hold_lock, "t_hold_lock");
    bind(m.h, m.drop_lock, "t_drop_lock");
    bind(m.h, m.fake_creating, "t_fake_creating");
    bind(m.h, m.loader_calls, "t_loader_calls");
    return true;
}
void unload(Mod& m) {
    if (m.h) FreeLibrary(m.h);
    m.h = nullptr;
}

constexpr std::uint32_t kInstLo = 0x1234ABCDu, kInstHi = 0x0BADF00Du;   // "install dir" hash
int want_atlas(Mod& m, std::uint32_t type) { return m.want(NC_ATLAS, 1, type, 0, kInstLo, kInstHi, 1024, 512); }
int want_icon(Mod& m, std::uint32_t id) { return m.want(NC_ITEM_ICON, 1, id, 0, kInstLo, kInstHi, 32, 32); }
int want_pic(Mod& m, std::uint32_t hash, std::uint32_t w, std::uint32_t h) { return m.want(NC_PICTURE, 1, w, h, hash, 0, w, h); }

int frame(Mod& m, void* dev) { advance(17); return m.frame(dev); }

// ---- mapping inspection -------------------------------------------------------------------------

NcHeader* g_hdr = nullptr;   // major 1 mapping, from any module (views are leaked, so it stays)

int live_entries(NcHeader* h, std::uint32_t kind = 0) {
    int n = 0;
    NcEntry* E = nc_detail::entries(h);
    for (int i = 0; i < NUICACHE_ENTRIES; ++i)
        if (E[i].state == 2 && (kind == 0 || E[i].kind == kind)) ++n;
    return n;
}
int entry_of(NcHeader* h, void* obj) {
    NcEntry* E = nc_detail::entries(h);
    for (int i = 0; i < NUICACHE_ENTRIES; ++i)
        if (E[i].state == 2 && E[i].obj == obj) return i;
    return -1;
}
LONG refs_of(void* obj) {
    if (g_world.real) {
        auto* u = static_cast<IUnknown*>(obj);
        u->AddRef();
        return LONG(u->Release());
    }
    return static_cast<Fake*>(obj)->refs;
}

// Table invariants: one live/creating entry per key and gen, refcount 1 per live entry (only the
// cache holds it), no dead object in the table, holder bits only for live clients, counters agree.
int check_table(NcHeader* h, char const* where) {
    int errors = 0;
    NcEntry* E = nc_detail::entries(h);
    NcClient* C = nc_detail::clients(h);
    LONG live_mask = 0;
    for (int i = 0; i < NUICACHE_CLIENTS; ++i) if (C[i].state == 1) live_mask |= LONG(1) << i;
    int live = 0;
    for (int i = 0; i < NUICACHE_ENTRIES; ++i) {
        NcEntry const& e = E[i];
        if (e.state != 1 && e.state != 2) continue;
        for (int j = i + 1; j < NUICACHE_ENTRIES; ++j) {
            NcEntry const& f = E[j];
            if ((f.state == 1 || f.state == 2) && f.kind == e.kind && f.fmt == e.fmt && f.a == e.a && f.b == e.b
                && f.hash_lo == e.hash_lo && f.hash_hi == e.hash_hi && f.gen == e.gen) {
                if (errors < 5) std::printf("  [%s] duplicate key: entries %d and %d (%s)\n", where, i, j, e.label);
                ++errors;
            }
        }
        if (e.state != 2) continue;
        ++live;
        if (!g_world.real) {
            auto* f = static_cast<Fake*>(e.obj);
            if (!f || f->dead || f->refs != 1) {
                if (errors < 5) std::printf("  [%s] entry %d (%s): obj refs %ld dead %ld\n", where, i, e.label, f ? f->refs : -1L, f ? f->dead : -1L);
                ++errors;
            }
        } else if (refs_of(e.obj) != 1) {
            std::printf("  [%s] entry %d refcount %ld\n", where, i, refs_of(e.obj));
            ++errors;
        }
        if (e.holders & ~live_mask) {
            if (errors < 5) std::printf("  [%s] entry %d (%s) holders %08lx, live clients %08lx\n", where, i, e.label, e.holders, live_mask);
            ++errors;
        }
    }
    if (live != h->live_entries) {
        std::printf("  [%s] live_entries counter %ld, table %d\n", where, h->live_entries, live);
        ++errors;
    }
    return errors;
}

// ---- reporting ----------------------------------------------------------------------------------

int g_fail_count = 0;
std::string g_test;
bool expect(bool ok, char const* what) {
    if (!ok) {
        std::printf("  FAIL %s: %s\n", g_test.c_str(), what);
        ++g_fail_count;
    }
    return ok;
}
#define EXPECT(cond) expect((cond), #cond)

// ---- layout (E3b) -------------------------------------------------------------------------------

#define NC_LAYOUT(X)                                                                                   \
    X(NcHeader, magic) X(NcHeader, ready) X(NcHeader, major) X(NcHeader, map_size) X(NcHeader, creator_pid) \
    X(NcHeader, max_rev) X(NcHeader, lock) X(NcHeader, lock_timeouts) X(NcHeader, device) X(NcHeader, gen)  \
    X(NcHeader, pending) X(NcHeader, last_sweep_ms) X(NcHeader, grace_ms) X(NcHeader, unheld_budget)         \
    X(NcHeader, client_cap) X(NcHeader, client_stride) X(NcHeader, clients_off) X(NcHeader, entry_cap)       \
    X(NcHeader, entry_stride) X(NcHeader, entries_off) X(NcHeader, creates) X(NcHeader, hits)                \
    X(NcHeader, releases) X(NcHeader, fallbacks) X(NcHeader, live_bytes) X(NcHeader, unheld_bytes)           \
    X(NcHeader, lock_breaks) X(NcHeader, reclaims) X(NcHeader, live_entries) X(NcHeader, reserved)           \
    X(NcClient, state) X(NcClient, serial) X(NcClient, module) X(NcClient, rev) X(NcClient, name)           \
    X(NcClient, version) X(NcClient, reserved)                                                               \
    X(NcEntry, state) X(NcEntry, holders) X(NcEntry, creator) X(NcEntry, kind) X(NcEntry, fmt) X(NcEntry, a) \
    X(NcEntry, b) X(NcEntry, hash_lo) X(NcEntry, hash_hi) X(NcEntry, obj_type) X(NcEntry, obj)               \
    X(NcEntry, device) X(NcEntry, gen) X(NcEntry, w) X(NcEntry, h) X(NcEntry, bytes) X(NcEntry, last_used_ms) \
    X(NcEntry, label) X(NcEntry, reserved)

std::string layout_text() {
    std::string s;
    char line[96];
#define NC_LINE(T, f) std::snprintf(line, sizeof(line), "%s.%s %u\n", #T, #f, unsigned(offsetof(T, f))); s += line;
    NC_LAYOUT(NC_LINE)
#undef NC_LINE
    std::snprintf(line, sizeof(line), "sizeof NcHeader %u NcClient %u NcEntry %u\n", unsigned(sizeof(NcHeader)),
                  unsigned(sizeof(NcClient)), unsigned(sizeof(NcEntry)));
    s += line;
    return s;
}

std::string read_file(std::string const& path) {
    std::string s;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return s;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    std::fclose(f);
    std::string out;
    for (char c : s) if (c != '\r') out += c;
    return out;
}

std::string g_layout_path;
void t_layout() {
    std::string const want = read_file(g_layout_path);
    std::string const have = layout_text();
    if (!EXPECT(!want.empty())) { std::printf("  cannot read %s\n", g_layout_path.c_str()); return; }
    if (want != have) {
        std::size_t a = 0, b = 0;
        int shown = 0;
        while (a < want.size() && b < have.size() && shown < 6) {
            std::size_t const ea = want.find('\n', a), eb = have.find('\n', b);
            std::string const la = want.substr(a, ea - a), lb = have.substr(b, eb - b);
            if (la != lb) { std::printf("  frozen: %s   built: %s\n", la.c_str(), lb.c_str()); ++shown; }
            a = ea + 1;
            b = eb + 1;
        }
    }
    EXPECT(want == have);
    int n = 0;
    for (char c : have) n += c == '\n';
    std::printf("  %d offsets compared\n", n);
}

// ---- E1: two modules share one object -----------------------------------------------------------

Fake* new_device() { return new_fake(true); }

void t_e1() {
    Fake* D = new_device();
    LONG const dead0 = g_dead_uses, under0 = g_underflow;
    Mod a, b;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1);
    EXPECT(b.start(&g_env, "") == 1);
    g_hdr = a.header();
    LONG const creates0 = g_hdr->creates, hits0 = g_hdr->hits;
    int const ia = want_atlas(a, 6), ib = want_atlas(b, 6);
    frame(a, D);
    frame(b, D);
    EXPECT(live_entries(g_hdr, NC_ATLAS) == 1);
    void* oa = a.obj(ia);
    void* ob = b.obj(ib);
    EXPECT(oa && oa == ob);
    int const e = oa ? entry_of(g_hdr, oa) : -1;
    EXPECT(e >= 0);
    if (e >= 0) {
        LONG const want_bits = (LONG(1) << a.slot()) | (LONG(1) << b.slot());
        EXPECT(nc_detail::entries(g_hdr)[e].holders == want_bits);
    }
    EXPECT(oa && refs_of(oa) == 1);
    EXPECT(g_hdr->creates - creates0 == 1 && g_hdr->hits - hits0 == 1);
    // held for 60 s while other entries churn (so sweeps run): never released under a holder
    for (int k = 0; k < 60; ++k) {
        advance(1000);
        int const ic = want_icon(b, 9000 + k);
        frame(a, D);
        frame(b, D);
        b.unwant(ic);
    }
    EXPECT(a.obj(ia) == oa && b.obj(ib) == oa);
    EXPECT(oa && !static_cast<Fake*>(oa)->dead);
    EXPECT(g_dead_uses == dead0);
    EXPECT(check_table(g_hdr, "e1 held") == 0);
    std::printf("  atlas entries %d, holders %08lx, refcount %ld, creates %ld hits %ld, churn icons live %d\n",
                live_entries(g_hdr, NC_ATLAS), e >= 0 ? nc_detail::entries(g_hdr)[e].holders : 0L, oa ? refs_of(oa) : -1L,
                g_hdr->creates - creates0, g_hdr->hits - hits0, live_entries(g_hdr, NC_ITEM_ICON));
    b.stop();
    a.stop();
    unload(b);
    unload(a);
    EXPECT(live_entries(g_hdr) == 0);
    EXPECT(D->refs == 1);
    EXPECT(g_underflow == under0);
}

// ---- hold (nui 1.2.1): nc_hold ----------------------------------------------------------------------

void t_hold() {
    Fake* D = new_device();
    LONG const dead0 = g_dead_uses, under0 = g_underflow;
    Mod a, b;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1);
    EXPECT(b.start(&g_env, "") == 1);
    g_hdr = a.header();
    // a makes a picture and another one it then lets go of (unheld, within the grace time)
    int const pa = want_pic(a, 0xC1, 512, 512), qa = want_pic(a, 0xC2, 512, 512);
    frame(a, D);
    frame(b, D);
    void* const obj = a.obj(pa);
    int const e = obj ? entry_of(g_hdr, obj) : -1;
    if (!EXPECT(e >= 0)) return;
    a.unwant(qa);
    LONG const creates0 = g_hdr->creates, hits0 = g_hdr->hits;
    // b holds a's picture from its Lua thread: the same object, b's bit set, nothing created
    int const pb = want_pic(b, 0xC1, 512, 512);
    EXPECT(b.hold(pb) == NC_HIT);
    EXPECT(b.obj(pb) == obj);
    EXPECT((nc_detail::entries(g_hdr)[e].holders & (LONG(1) << b.slot())) != 0);
    EXPECT(g_hdr->creates == creates0 && g_hdr->hits == hits0 + 1);
    // a key nobody made: NC_FAILED, nothing created, a's unheld picture not purged, no backoff on b
    int const nb = want_pic(b, 0xC3, 512, 512);
    LONG const live0 = g_hdr->live_entries;
    EXPECT(b.hold(nb) == NC_FAILED);
    EXPECT(b.obj(nb) == nullptr);
    EXPECT(g_hdr->creates == creates0 && g_hdr->live_entries == live0);
    EXPECT(live_entries(g_hdr, NC_PICTURE) == 2);       // pa (held) and qa (unheld, in its grace time)
    b.unwant(nb);
    // a lets go and the grace time passes: sweeps (a's frames) keep the entry while b holds it
    a.unwant(pa);
    for (int k = 0; k < 4; ++k) {
        advance(NUICACHE_GRACE_MS);
        frame(a, D);
    }
    EXPECT(live_entries(g_hdr, NC_PICTURE) == 1);       // qa went; pa stays for b
    EXPECT(entry_of(g_hdr, obj) == e && !static_cast<Fake*>(obj)->dead);
    // b's frame draws the held object (no acquire, no create)
    EXPECT(frame(b, D) == 1 && b.obj(pb) == obj && g_hdr->creates == creates0);
    EXPECT(g_dead_uses == dead0);
    // given back: the next sweep after the grace time releases it, once
    b.unwant(pb);
    advance(NUICACHE_GRACE_MS + 100);
    frame(a, D);
    EXPECT(live_entries(g_hdr, NC_PICTURE) == 0);
    EXPECT(static_cast<Fake*>(obj)->dead && static_cast<Fake*>(obj)->refs == 0);
    // b can still create (no backoff from the failed hold)
    int const cb = want_pic(b, 0xC4, 512, 512);
    EXPECT(frame(b, D) == 1 && b.obj(cb) != nullptr);
    EXPECT(check_table(g_hdr, "hold") == 0);
    b.stop();
    a.stop();
    unload(b);
    unload(a);
    EXPECT(live_entries(g_hdr) == 0);
    EXPECT(g_underflow == under0);
}

// ---- E2: every load and unload order, with and without stop ---------------------------------------

struct Set { std::vector<int> h; };
void want_set(Mod& m, char letter, Set& s) {
    s.h.clear();
    s.h.push_back(want_atlas(m, 6));
    if (letter == 'a') { s.h.push_back(want_icon(m, 4165)); s.h.push_back(want_icon(m, 4166)); s.h.push_back(want_pic(m, 0xA1, 760, 570)); }
    if (letter == 'b') { s.h.push_back(want_icon(m, 4165)); s.h.push_back(want_pic(m, 0xB1, 760, 570)); }
    if (letter == 'c') { s.h.push_back(want_atlas(m, 1)); s.h.push_back(want_icon(m, 4166)); }
}

void t_e2() {
    Fake* D = new_device();
    char const letters[3] = {'a', 'b', 'c'};
    int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    int runs = 0, steps = 0, failed_runs = 0;
    LONG const under0 = g_underflow;
    for (int mode = 0; mode < 2; ++mode) {          // 0 = stop then FreeLibrary, 1 = FreeLibrary only
        for (auto& lp : perms) {
            for (auto& up : perms) {
                int const fails0 = g_fail_count;
                LONG const dead0 = g_dead_uses;
                Mod m[3];
                Set s[3];
                bool loaded[3] = {false, false, false};
                auto frame_all = [&](char const* where) {
                    for (int k = 0; k < 3; ++k) {
                        int const i = lp[k];
                        if (!loaded[i]) continue;
                        int const drawn = frame(m[i], D);
                        if (!expect(drawn == int(s[i].h.size()), "every live module draws its whole layer")) {
                            std::printf("  %s: module %c drew %d of %d\n", where, letters[i], drawn, int(s[i].h.size()));
                        }
                    }
                    // shared keys: one object
                    for (int i = 0; i < 3; ++i)
                        for (int j = i + 1; j < 3; ++j)
                            if (loaded[i] && loaded[j]) expect(m[i].obj(s[i].h[0]) == m[j].obj(s[j].h[0]), "atlas shared");
                    if (g_hdr) expect(check_table(g_hdr, where) == 0, "table invariants");
                    ++steps;
                };
                for (int k = 0; k < 3; ++k) {
                    int const i = lp[k];
                    char name[2] = {letters[i], 0};
                    if (!load(m[i], name)) { ++g_fail_count; return; }
                    expect(m[i].start(&g_env, "") == 1, "start shares");
                    if (!g_hdr) g_hdr = m[i].header();
                    want_set(m[i], letters[i], s[i]);
                    loaded[i] = true;
                    frame_all("load");
                }
                for (int k = 0; k < 3; ++k) {
                    int const i = up[k];
                    if (mode == 0) m[i].stop();
                    unload(m[i]);
                    loaded[i] = false;
                    frame_all("unload");
                }
                if (mode == 0) {
                    expect(live_entries(g_hdr) == 0, "last stop releases every unheld entry");
                } else {
                    NcEntry* E = nc_detail::entries(g_hdr);
                    int held = 0;
                    for (int i = 0; i < NUICACHE_ENTRIES; ++i) if (E[i].state == 2 && E[i].holders) ++held;
                    expect(held == 0, "detach cleared every holder bit");
                    Mod d;                               // the next native-UI module sweeps them
                    if (!load(d, "d")) { ++g_fail_count; return; }
                    expect(d.start(&g_env, "") == 1, "sweeper starts");
                    advance(LONG(NUICACHE_GRACE_MS) + 100);
                    frame(d, D);
                    expect(live_entries(g_hdr) == 0, "unheld entries swept after grace");
                    d.stop();
                    unload(d);
                }
                expect(D->refs == 1, "device refcount back to baseline");
                expect(g_dead_uses == dead0, "no use of a released object");
                ++runs;
                if (g_fail_count != fails0) {
                    ++failed_runs;
                    if (failed_runs <= 3)
                        std::printf("  run failed: mode %s, load %c%c%c, unload %c%c%c\n", mode ? "nostop" : "stop", letters[lp[0]],
                                    letters[lp[1]], letters[lp[2]], letters[up[0]], letters[up[1]], letters[up[2]]);
                }
            }
        }
    }
    EXPECT(g_underflow == under0);
    std::printf("  %d runs (36 orders x stop/nostop), %d checked steps, %d failed runs, underflows %ld\n", runs, steps,
                failed_runs, g_underflow - under0);
}

// ---- grace, budget, dead clients, dead creators, create failure ---------------------------------

void t_evict() {
    Fake* D = new_device();
    Mod a, b;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1);
    g_hdr = a.header();
    // 1. grace: an unheld entry lives 10 s and is found again within it
    int ia = want_atlas(a, 6);
    frame(a, D);
    void* atlas = a.obj(ia);
    a.unwant(ia);
    advance(5000);
    frame(a, D);
    EXPECT(live_entries(g_hdr, NC_ATLAS) == 1);
    LONG const creates0 = g_hdr->creates;
    int ib = want_atlas(b, 6);
    frame(b, D);
    EXPECT(b.obj(ib) == atlas && g_hdr->creates == creates0);
    b.unwant(ib);
    advance(LONG(NUICACHE_GRACE_MS) + 100);
    frame(a, D);
    bool const grace_ok = live_entries(g_hdr, NC_ATLAS) == 0;
    EXPECT(grace_ok);
    std::printf("  grace: kept at 5 s, re-found (creates +%ld), released after 10 s: %s\n", g_hdr->creates - creates0,
                grace_ok ? "yes" : "no");
    // 2. budget: 12 x 1 MiB unheld -> the 4 oldest go at once
    int pics[12];
    for (int k = 0; k < 12; ++k) pics[k] = want_pic(a, 0x100 + k, 512, 512);
    frame(a, D);
    EXPECT(live_entries(g_hdr, NC_PICTURE) == 12);
    for (int k = 0; k < 12; ++k) { a.unwant(pics[k]); advance(1); }
    frame(a, D);
    int left = live_entries(g_hdr, NC_PICTURE);
    bool oldest_gone = true;
    NcEntry* E = nc_detail::entries(g_hdr);
    for (int i = 0; i < NUICACHE_ENTRIES; ++i)
        if (E[i].state == 2 && E[i].kind == NC_PICTURE && E[i].hash_lo < 0x104) oldest_gone = false;
    EXPECT(left == 8 && oldest_gone);
    EXPECT(g_hdr->unheld_bytes == LONG(8u << 20));
    std::printf("  budget: 12 MiB unheld -> %d pictures left (oldest 4 gone: %s), unheld %ld bytes\n", left,
                oldest_gone ? "yes" : "no", g_hdr->unheld_bytes);
    advance(LONG(NUICACHE_GRACE_MS) + 100);
    frame(a, D);
    EXPECT(live_entries(g_hdr, NC_PICTURE) == 0);
    // 3. a client whose module vanished without DllMain: reclaimed, its bit cleared
    ia = want_atlas(a, 6);
    frame(a, D);
    int const e = entry_of(g_hdr, a.obj(ia));
    a.unwant(ia);
    NcClient* C = nc_detail::clients(g_hdr);
    int ghost = -1;
    for (int i = 0; i < NUICACHE_CLIENTS && ghost < 0; ++i) if (C[i].state == 0) ghost = i;
    void* nowhere = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);   // not a module
    LONG const reclaims0 = g_hdr->reclaims;
    if (EXPECT(ghost >= 0 && e >= 0)) {
        C[ghost].serial = 77;
        C[ghost].module = static_cast<HMODULE>(nowhere);
        std::strcpy(C[ghost].name, "ghost");
        InterlockedExchange(&C[ghost].state, 1);
        nc_detail::atomic_or(&E[e].holders, LONG(1) << ghost);
        InterlockedExchange(&g_hdr->pending, 1);
        frame(a, D);
        EXPECT(C[ghost].state == 0 && (E[e].holders & (LONG(1) << ghost)) == 0);
        advance(LONG(NUICACHE_GRACE_MS) + 100);
        frame(a, D);
        EXPECT(live_entries(g_hdr, NC_ATLAS) == 0);
        if (C[ghost].state != 0) InterlockedExchange(&C[ghost].state, 0);
    }
    std::printf("  dead client: reclaims +%ld, its entry released after grace: %s\n", g_hdr->reclaims - reclaims0,
                live_entries(g_hdr, NC_ATLAS) == 0 ? "yes" : "no");
    VirtualFree(nowhere, 0, MEM_RELEASE);
    // 4. creating: busy while the creator lives, cleaned up once it is gone
    EXPECT(a.fake_creating(NC_ITEM_ICON, 777) >= 0);
    int const iw = b.want(NC_ITEM_ICON, 0, 777, 0, 0, 0, 32, 32);
    int const busy0 = b.busy();
    EXPECT(frame(b, D) == 0 && b.busy() == busy0 + 1);
    unload(a);                                    // without stop: DllMain detach only
    int const drawn = frame(b, D);
    EXPECT(drawn == 1 && b.entry(iw) >= 0);
    std::printf("  creating entry: busy while its creator lives, drawn after the creator unloads: %s\n", drawn == 1 ? "yes" : "no");
    // 5. create failure: drop unheld; on the render thread no second try in the same frame (the Lua
    //    thread's one retry is rl's part 3); then 2 s of backoff
    LONG const calls0 = g_create_calls;
    g_world.fail_creates = 1;
    int const i1 = want_icon(b, 555);
    EXPECT(frame(b, D) == 1 && !b.obj(i1));       // review HOOK-5: one try on the render thread, no retry this frame
    LONG const calls1 = g_create_calls;
    EXPECT(calls1 == calls0 + 1);
    frame(b, D);
    EXPECT(g_create_calls == calls1 && !b.obj(i1));   // backoff: no create call
    advance(LONG(NUICACHE_BACKOFF_MS) + 10);
    EXPECT(frame(b, D) == 2 && b.obj(i1));
    std::printf("  create failure: unheld dropped, one try, then backoff 2 s (create calls %ld)\n", g_create_calls - calls0);
    b.stop();
    unload(b);
    EXPECT(live_entries(g_hdr) == 0);
    EXPECT(D->refs == 1);
    EXPECT(check_table(g_hdr, "evict end") == 0);
}

// ---- E3 cache part: revisions share, an ABI mismatch falls back, majors stay apart --------------

void t_e3c() {
    Fake* D = new_device();
    Mod a, r2, x, z;
    if (!EXPECT(load(a, "a") && load(r2, "r2") && load(x, "x") && load(z, "z"))) return;
    EXPECT(a.start(&g_env, "") == 1);
    g_hdr = a.header();
    int const ia = want_atlas(a, 6);
    frame(a, D);
    EXPECT(r2.start(&g_env, "") == 1);
    int const ir = want_atlas(r2, 6);
    frame(r2, D);
    EXPECT(r2.obj(ir) && r2.obj(ir) == a.obj(ia));
    EXPECT(g_hdr->max_rev == 2);
    NcStatus st;
    EXPECT(x.start(&g_env, "") == 0);             // NUICACHE_ENTRIES 200: layout differs -> private
    x.status(&st);
    EXPECT(std::strcmp(st.why, "layout differs") == 0);
    int const ix = want_atlas(x, 6);
    EXPECT(frame(x, D) == 1 && x.entry(ix) == -1 && x.obj(ix) != a.obj(ia));
    std::printf("  rev 1 + rev 2 share one atlas (max_rev %ld); entry-cap 200 build: private (\"%s\"), draws %s\n",
                g_hdr->max_rev, st.why, x.obj(ix) ? "yes" : "no");
    EXPECT(z.start(&g_env, "") == 1);             // major 99: its own mapping
    int const iz = want_atlas(z, 6);
    EXPECT(frame(z, D) == 1);
    NcHeader* hz = z.header();
    EXPECT(hz && hz->major == 99);                // views of one mapping differ in address: check the major
    EXPECT(z.obj(iz) && z.obj(iz) != a.obj(ia));
    EXPECT(live_entries(g_hdr, NC_ATLAS) == 1);
    if (hz) EXPECT(hz->major == 99 && live_entries(hz, NC_ATLAS) == 1);
    std::printf("  major 99: own mapping %s, own atlas %s, both draw\n", hz && hz->major == 99 ? "yes" : "no",
                z.obj(iz) != a.obj(ia) ? "yes" : "no");
    x.stop();
    z.stop();
    r2.stop();
    a.stop();
    unload(x);
    unload(z);
    unload(r2);
    unload(a);
    EXPECT(live_entries(g_hdr) == 0);
    if (hz) EXPECT(live_entries(hz) == 0);
    EXPECT(D->refs == 1);
}

// ---- E5: device change (fake) ------------------------------------------------------------------

void t_e5() {
    Fake* D1 = new_device();
    Fake* D2 = new_device();
    LONG const dead0 = g_dead_uses;
    Mod a, b;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1);
    g_hdr = a.header();
    int const a1 = want_atlas(a, 6), a2 = want_icon(a, 4165);
    int const b1 = want_atlas(b, 6), b2 = want_icon(b, 4165);
    frame(a, D1);
    frame(b, D1);
    LONG const gen0 = g_hdr->gen;
    EXPECT(D1->refs == 1 + 2);                    // two textures pin it
    frame(a, D2);
    frame(b, D2);
    frame(a, D2);
    frame(b, D2);
    EXPECT(g_hdr->gen == gen0 + 1);
    EXPECT(live_entries(g_hdr) == 2);
    bool on_d2 = true;
    for (int h : {a1, a2}) on_d2 = on_d2 && a.obj(h) && static_cast<Fake*>(a.obj(h))->dev == D2;
    EXPECT(on_d2);
    EXPECT(a.obj(a1) == b.obj(b1) && a.obj(a2) == b.obj(b2));
    EXPECT(D1->refs == 1);
    EXPECT(g_dead_uses == dead0);
    EXPECT(check_table(g_hdr, "e5") == 0);
    std::printf("  gen %ld -> %ld, live %d (on D2: %s), D1 refs %ld (baseline 1), dead uses %ld\n", gen0, g_hdr->gen,
                live_entries(g_hdr), on_d2 ? "yes" : "no", D1->refs, g_dead_uses - dead0);
    b.stop();
    a.stop();
    unload(b);
    unload(a);
    EXPECT(live_entries(g_hdr) == 0 && D2->refs == 1);
}

// ---- E6: races ----------------------------------------------------------------------------------
// Two threads (modules a and b, one with the Lua-thread lock rules and one with the render-thread
// rules) walk the same sequence of NEW keys, so every key is raced for creation; a third module c
// draws (and churns) its own icons on the main thread meanwhile. The cache yields inside every
// critical section (t_set_yield) to make interleavings likely. Rounds of 300 keys fit the table;
// between rounds the table is checked while quiet, then swept.

struct Racer { Mod* m; int mode; std::uint32_t base; int keys; void* dev; long tally[8]; };

DWORD WINAPI racer(void* p) {
    auto* r = static_cast<Racer*>(p);
    for (int k = 0; k < r->keys; ++k) {
        int const h = want_icon(*r->m, r->base + std::uint32_t(k));
        int const res = r->m->acquire(h, r->dev, r->mode);
        if (res >= 0 && res < 8) ++r->tally[res];
        r->m->unwant(h);
    }
    return 0;
}

void t_e6() {
    Fake* D = new_device();
    LONG const dead0 = g_dead_uses, under0 = g_underflow;
    Mod a, b, c;
    if (!EXPECT(load(a, "a") && load(b, "b") && load(c, "c"))) return;
    EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1 && c.start(&g_env, "") == 1);
    g_hdr = a.header();
    int hc[8];
    for (int k = 0; k < 8; ++k) hc[k] = want_icon(c, 7000 + k);
    frame(c, D);
    for (Mod* m : {&a, &b, &c}) m->set_yield(1);
    LONG const timeouts0 = g_hdr->lock_timeouts, creates0 = g_hdr->creates;
    Racer ra {&a, 1, 0, 300, D, {}}, rb {&b, 0, 0, 300, D, {}};
    int const rounds = 34;                        // 34 x 300 > 10,000 acquire/free per thread
    int frames = 0, errors = 0, dup_rounds = 0;
    for (int round = 0; round < rounds; ++round) {
        ra.base = rb.base = 100000 + std::uint32_t(round) * 1000;
        HANDLE th[2] = {CreateThread(nullptr, 0, racer, &ra, 0, nullptr), CreateThread(nullptr, 0, racer, &rb, 0, nullptr)};
        while (WaitForMultipleObjects(2, th, TRUE, 0) == WAIT_TIMEOUT) {
            c.frame(D);                           // clock frozen during the round: no sweep
            ++frames;
            if ((frames & 3) == 0) for (int k = 0; k < 8; ++k) c.drop(hc[k]);
        }
        CloseHandle(th[0]);
        CloseHandle(th[1]);
        c.frame(D);
        int const e = check_table(g_hdr, "e6 round");
        if (e) ++dup_rounds;
        errors += e;
        advance(LONG(NUICACHE_GRACE_MS) + 100);   // sweep the round's keys away
        frame(c, D);
    }
    for (Mod* m : {&a, &b, &c}) m->set_yield(0);
    EXPECT(errors == 0);
    int icons = 0, wrong_holders = 0;
    NcEntry* E = nc_detail::entries(g_hdr);
    for (int i = 0; i < NUICACHE_ENTRIES; ++i) {
        if (E[i].state != 2) continue;
        if (E[i].kind == NC_ITEM_ICON && E[i].a >= 7000 && E[i].a < 7008) ++icons;
        if (E[i].holders != (LONG(1) << c.slot())) ++wrong_holders;
    }
    EXPECT(icons == 8 && wrong_holders == 0 && live_entries(g_hdr) == 8);
    EXPECT(g_dead_uses == dead0 && g_underflow == under0);
    long const per_thread = long(rounds) * ra.keys;
    std::printf("  %d rounds, 2 x %ld acquire/free of raced new keys + %d frames of a third module: rounds with a duplicate or bad refcount %d\n",
                rounds, per_thread, frames, dup_rounds);
    std::printf("  creates %ld (keys %ld), lock timeouts %ld; lua-rules thread: hit %ld created %ld private %ld busy %ld;"
                " render-rules thread: hit %ld created %ld busy %ld\n",
                g_hdr->creates - creates0, per_thread, g_hdr->lock_timeouts - timeouts0, ra.tally[NC_HIT],
                ra.tally[NC_CREATED], ra.tally[NC_PRIVATE], ra.tally[NC_BUSY], rb.tally[NC_HIT], rb.tally[NC_CREATED],
                rb.tally[NC_BUSY]);
    a.stop();
    b.stop();
    c.stop();
    unload(a);
    unload(b);
    unload(c);
    EXPECT(live_entries(g_hdr) == 0 && D->refs == 1);
}

// ---- E8: fallbacks ------------------------------------------------------------------------------

void t_e8() {
    Fake* D = new_device();
    Mod a, b, c, d;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1);
    g_hdr = a.header();
    // 1. table full -> private, still drawn, released by the module
    frame(a, D);
    int const room = NUICACHE_ENTRIES - live_entries(g_hdr);
    std::vector<int> fill;
    for (int k = 0; k < room; ++k) fill.push_back(want_icon(a, 20000 + k));
    frame(a, D);
    EXPECT(live_entries(g_hdr) == NUICACHE_ENTRIES);
    LONG const fb0 = g_hdr->fallbacks;
    int const ib = want_icon(b, 99999);
    int const drawn = frame(b, D);
    void* priv = b.obj(ib);
    EXPECT(drawn == 1 && b.entry(ib) == -1);
    EXPECT(g_hdr->fallbacks == fb0 + 1);
    b.unwant(ib);
    EXPECT(priv && static_cast<Fake*>(priv)->dead);
    std::printf("  table full (%d entries): new texture private, drawn %d, fallbacks +%ld, freed by its module: %s\n",
                live_entries(g_hdr), drawn, g_hdr->fallbacks - fb0, priv && static_cast<Fake*>(priv)->dead ? "yes" : "no");
    for (int h : fill) a.unwant(h);
    advance(LONG(NUICACHE_GRACE_MS) + 100);
    frame(a, D);
    EXPECT(live_entries(g_hdr) == 0);
    // 2. a lock left held by a module that unloads: broken by CAS
    EXPECT(a.hold_lock() == 1);
    unload(a);
    LONG const breaks0 = g_hdr->lock_breaks;
    int const ib2 = want_atlas(b, 6);
    EXPECT(frame(b, D) == 1 && b.entry(ib2) >= 0);
    EXPECT(g_hdr->lock_breaks == breaks0 + 1);
    std::printf("  stale lock: broken %ld time(s), drawn shared: %s\n", g_hdr->lock_breaks - breaks0, b.entry(ib2) >= 0 ? "yes" : "no");
    // 3. a lock held by a live module: Lua thread falls back after ~50 ms, render thread gives up
    if (!EXPECT(load(c, "c"))) return;
    EXPECT(c.start(&g_env, "") == 1);
    EXPECT(c.hold_lock() == 1);
    int const l1 = want_icon(b, 4165), l2 = want_icon(b, 4166);
    DWORD const t0 = GetTickCount();
    int const r1 = b.acquire(l1, D, 1);
    DWORD const waited = GetTickCount() - t0;
    int const r2 = b.acquire(l2, D, 0);
    c.drop_lock();
    EXPECT(r1 == NC_PRIVATE && b.entry(l1) == -1 && waited >= 40);
    EXPECT(r2 == NC_BUSY && !b.obj(l2));
    std::printf("  live lock: Lua thread private after %lu ms, render thread busy (draws next frame)\n", waited);
    b.unwant(l1);
    b.unwant(l2);
    // 4. noshare: the same layer drawn from private objects, the table untouched
    if (!EXPECT(load(d, "d"))) return;
    EXPECT(d.start(&g_env, "noshare") == 0);
    int const live0 = live_entries(g_hdr);
    int const d1 = want_atlas(d, 6), d2 = want_icon(d, 4165);
    EXPECT(frame(d, D) == 2 && d.entry(d1) == -1 && d.entry(d2) == -1 && d.obj(d1) != b.obj(ib2));
    EXPECT(live_entries(g_hdr) == live0);
    void* dobj = d.obj(d1);
    d.stop();
    EXPECT(dobj && static_cast<Fake*>(dobj)->dead);
    std::printf("  noshare: drew 2 private, table unchanged (%d live), released at stop: %s\n", live_entries(g_hdr),
                dobj && static_cast<Fake*>(dobj)->dead ? "yes" : "no");
    unload(d);
    c.stop();
    b.stop();
    unload(c);
    unload(b);
    EXPECT(live_entries(g_hdr) == 0 && D->refs == 1);
    EXPECT(check_table(g_hdr, "e8 end") == 0);
}

// ---- ld: no loader call on the render thread (review HOOK-4) -------------------------------------

void t_ld() {
    Fake* D = new_device();
    Mod a, b;
    if (!EXPECT(load(a, "a") && load(b, "b"))) return;
    EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1);
    g_hdr = a.header();
    int const ia = want_atlas(a, 6);
    frame(a, D);
    a.unwant(ia);                                 // an unheld entry: every frame of a sweeps, looking for dead clients
    long const c0 = a.loader_calls();
    LONG const sweeps0 = g_hdr->last_sweep_ms;
    for (int k = 0; k < 5; ++k) frame(a, D);
    long const calls = a.loader_calls() - c0;
    EXPECT(g_hdr->last_sweep_ms != sweeps0);
    if (!EXPECT(calls == 0)) std::printf("  FAIL ld: 5 render-thread sweeps made %ld GetModuleHandleEx calls (the loader)\n", calls);
    std::printf("  5 sweeps with another client live: %ld loader calls on the render thread\n", calls);
    a.stop();
    b.stop();
    unload(a);
    unload(b);
    EXPECT(live_entries(g_hdr) == 0 && D->refs == 1);
}

// ---- rl: the render thread never waits as the Lua thread, nor creates twice (review HOOK-5) ---------

void t_rl() {
    Fake* D = new_device();
    Mod b, c;
    if (!EXPECT(load(b, "b") && load(c, "c"))) return;
    EXPECT(b.start(&g_env, "") == 1 && c.start(&g_env, "") == 1);
    g_hdr = b.header();
    LONG const tex0 = g_live_tex;
    frame(b, D);
    // 1. the cache lock is taken by another module while the render thread creates: it spins briefly,
    //    keeps the object, and publishes it at its next acquire (no second create)
    int const i = want_icon(b, 7770);
    LONG const k0 = g_create_calls;
    g_lock_in_create = c.hold_lock;
    DWORD const t0 = GetTickCount();
    int const r1 = b.acquire(i, D, 0);
    DWORD const waited = GetTickCount() - t0;
    c.drop_lock();
    EXPECT(r1 == NC_BUSY && !b.obj(i));
    if (!EXPECT(waited < 40)) std::printf("  FAIL rl: the render thread waited %lu ms for the cache lock\n", waited);
    int const r2 = b.acquire(i, D, 0);
    EXPECT(r2 == NC_HIT && b.obj(i) != nullptr);
    if (!EXPECT(g_create_calls == k0 + 1))
        std::printf("  FAIL rl: %ld creates for one texture (the unpublished object was thrown away)\n", g_create_calls - k0);
    EXPECT(check_table(g_hdr, "rl published") == 0);
    std::printf("  lock busy after a create: render thread back in %lu ms, published next time (%ld create)\n", waited,
                g_create_calls - k0);
    // 2. a failed create on the render thread is not retried in the same frame
    int const j = want_icon(b, 7771);
    g_world.fail_creates = 5;
    LONG const k1 = g_create_calls;
    int const r3 = b.acquire(j, D, 0);
    LONG const tries = g_create_calls - k1;
    g_world.fail_creates = 0;
    EXPECT(r3 == NC_FAILED);
    if (!EXPECT(tries == 1)) std::printf("  FAIL rl: a failed create was tried %ld times in one render-thread acquire\n", tries);
    advance(LONG(NUICACHE_BACKOFF_MS) + 10);
    EXPECT(b.acquire(j, D, 0) == NC_CREATED);
    // 3. the Lua thread still purges and retries once
    int const k = want_icon(b, 7772);
    g_world.fail_creates = 1;
    LONG const k2 = g_create_calls;
    EXPECT(b.acquire(k, D, 1) == NC_CREATED && g_create_calls == k2 + 2);
    g_world.fail_creates = 0;
    // 4. an object still unpublished at stop() is given back
    int const m = want_icon(b, 7773);
    g_lock_in_create = c.hold_lock;
    EXPECT(b.acquire(m, D, 0) == NC_BUSY);
    c.drop_lock();
    b.stop();
    c.stop();
    unload(b);
    unload(c);
    EXPECT(live_entries(g_hdr) == 0 && D->refs == 1);
    if (!EXPECT(g_live_tex == tex0)) std::printf("  FAIL rl: %ld textures leaked\n", g_live_tex - tex0);
    EXPECT(check_table(g_hdr, "rl end") == 0);
}

// ---- real D3D8 devices (E4 real, E5 real) --------------------------------------------------------

std::string g_d3d8_path;
struct Real { IDirect3D8* d3d = nullptr; IDirect3DDevice8* dev[2] = {nullptr, nullptr}; HMODULE dll = nullptr; std::string why; };

bool make_real(Real& r, char const* dll) {
    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "nui_cache_test";
    RegisterClassA(&wc);
    r.dll = LoadLibraryA(dll);
    if (!r.dll) { r.why = std::string("cannot load ") + dll; return false; }
    using Create8 = IDirect3D8*(WINAPI*)(UINT);
    auto create = reinterpret_cast<Create8>(reinterpret_cast<void*>(GetProcAddress(r.dll, "Direct3DCreate8")));
    r.d3d = create ? create(D3D_SDK_VERSION) : nullptr;
    if (!r.d3d) { r.why = "Direct3DCreate8 failed"; return false; }
    D3DDISPLAYMODE mode {};
    r.d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    for (int i = 0; i < 2; ++i) {
        HWND hwnd = CreateWindowA("nui_cache_test", "nui", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        D3DPRESENT_PARAMETERS pp {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.BackBufferFormat = mode.Format;
        pp.hDeviceWindow = hwnd;
        if (FAILED(r.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &r.dev[i]))) {
            r.why = "CreateDevice failed";
            return false;
        }
    }
    return true;
}
void free_real(Real& r) {
    for (auto*& d : r.dev) if (d) { d->Release(); d = nullptr; }
    if (r.d3d) r.d3d->Release();
    r.d3d = nullptr;
}
ULONG probe(IUnknown* u) { u->AddRef(); return u->Release(); }

SIZE_T private_usage() {
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return pmc.PrivateUsage;
}

void real_e5(char const* dll, char const* label) {
    Real r;
    if (!make_real(r, dll)) { std::printf("  %s: skipped (%s)\n", label, r.why.c_str()); free_real(r); return; }
    IDirect3DDevice8* D1 = r.dev[0];
    IDirect3DDevice8* D2 = r.dev[1];
    ULONG const base1 = probe(D1), base2 = probe(D2);
    // informational: does a live texture pin its device?
    IDirect3DTexture8* t = nullptr;
    D1->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t);
    ULONG const with_tex = probe(D1);
    if (t) t->Release();
    std::printf("  %s: device refcount %lu, with one live texture %lu -> textures %s their device\n", label, base1, with_tex,
                with_tex > base1 ? "PIN" : "do not pin");
    g_world.real = true;
    Mod a, b;
    if (EXPECT(load(a, "a") && load(b, "b"))) {
        EXPECT(a.start(&g_env, "") == 1 && b.start(&g_env, "") == 1);
        g_hdr = a.header();
        int const a1 = want_atlas(a, 6), a2 = want_icon(a, 4165), b1 = want_atlas(b, 6);
        (void)a2;
        frame(a, D1);
        frame(b, D1);
        EXPECT(a.obj(a1) && a.obj(a1) == b.obj(b1));
        LONG const gen0 = g_hdr->gen;
        frame(a, D2);
        frame(b, D2);
        EXPECT(g_hdr->gen == gen0 + 1);
        bool on_d2 = true;
        NcEntry* E = nc_detail::entries(g_hdr);
        for (int i = 0; i < NUICACHE_ENTRIES; ++i)
            if (E[i].state == 2) {
                IDirect3DDevice8* owner = nullptr;
                static_cast<IDirect3DTexture8*>(E[i].obj)->GetDevice(&owner);
                on_d2 = on_d2 && owner == D2 && E[i].device == D2;
                if (owner) owner->Release();
            }
        EXPECT(on_d2 && live_entries(g_hdr) == 2);
        ULONG const after1 = probe(D1);
        EXPECT(after1 == base1);
        EXPECT(a.obj(a1) == b.obj(b1));
        EXPECT(check_table(g_hdr, label) == 0);
        std::printf("  %s: gen %ld -> %ld, %d entries re-made on D2 (%s), D1 refcount %lu (baseline %lu)\n", label, gen0,
                    g_hdr->gen, live_entries(g_hdr), on_d2 ? "verified by GetDevice" : "WRONG", after1, base1);
        b.stop();
        a.stop();
        unload(b);
        unload(a);
        EXPECT(live_entries(g_hdr) == 0 && probe(D2) == base2);
    }
    g_world.real = false;
    free_real(r);
}

void t_e5real() {
    real_e5("d3d8.dll", "system d3d8");
    if (!g_d3d8_path.empty()) real_e5(g_d3d8_path.c_str(), "dgVoodoo d3d8");
}

// ---- E4: memory ---------------------------------------------------------------------------------

// Module b's first frame, after a already holds the same layer: PrivateUsage delta.
long long measure_b(void* dev, bool noshare_b, bool own_picture) {
    Mod a, b;
    if (!load(a, "a") || !load(b, "b")) return -1;
    a.start(&g_env, "");
    b.start(&g_env, noshare_b ? "noshare" : "");
    if (!g_hdr) g_hdr = a.header();
    want_atlas(a, 6);
    for (int k = 0; k < 16; ++k) want_icon(a, 4100 + k);
    want_pic(a, 0xAA, 760, 570);
    frame(a, dev);
    want_atlas(b, 6);
    for (int k = 0; k < 16; ++k) want_icon(b, 4100 + k);
    if (own_picture) want_pic(b, 0xBB, 760, 570);
    SIZE_T const p0 = private_usage();
    frame(b, dev);
    SIZE_T const p1 = private_usage();
    b.stop();
    a.stop();
    unload(b);
    unload(a);
    return static_cast<long long>(p1) - static_cast<long long>(p0);
}

void e4_run(void* dev, char const* label) {
    measure_b(dev, false, false);                 // warm-up (first-use allocations of the runtime)
    long long const shared = measure_b(dev, false, false), priv = measure_b(dev, true, false);
    long long const shared_r = measure_b(dev, false, true), priv_r = measure_b(dev, true, true);
    double const mb = 1024.0 * 1024.0;
    std::printf("  %s: layer shared by both (atlas 1024x512 + 16 icons): b's first frame %+.2f MiB shared vs %+.2f MiB noshare (%.0f%%)\n",
                label, shared / mb, priv / mb, priv > 0 ? 100.0 * double(shared) / double(priv) : 0.0);
    std::printf("  %s: realistic (same + b's own 760x570 picture): %+.2f vs %+.2f MiB -> saved per extra client %.2f MiB\n", label,
                shared_r / mb, priv_r / mb, (priv_r - shared_r) / mb);
    EXPECT(priv >= (2 << 20));
    EXPECT(shared * 4 < priv);
}

void t_e4() {
    Fake* D = new_device();
    g_world.alloc_mem = true;
    e4_run(D, "fake (data + shadow committed)");
    g_world.alloc_mem = false;
    // 4 modules hold one atlas
    Mod m[4];
    char const* names[4] = {"a", "b", "c", "d"};
    LONG bits = 0;
    void* first = nullptr;
    bool same = true;
    int hs[4];
    for (int i = 0; i < 4; ++i) {
        if (!EXPECT(load(m[i], names[i]))) return;
        m[i].start(&g_env, "");
        if (!g_hdr) g_hdr = m[i].header();
        hs[i] = want_atlas(m[i], 6);
        frame(m[i], D);
        bits |= LONG(1) << m[i].slot();
    }
    for (int i = 0; i < 4; ++i) { if (!first) first = m[i].obj(hs[i]); same = same && m[i].obj(hs[i]) == first; }
    int const e = entry_of(g_hdr, first);
    EXPECT(live_entries(g_hdr, NC_ATLAS) == 1 && same && e >= 0 && nc_detail::entries(g_hdr)[e].holders == bits);
    std::printf("  4 modules: %d atlas entry, holders %08lx, one object: %s\n", live_entries(g_hdr, NC_ATLAS),
                e >= 0 ? nc_detail::entries(g_hdr)[e].holders : 0L, same ? "yes" : "no");
    for (int i = 0; i < 4; ++i) { m[i].stop(); unload(m[i]); }
    EXPECT(live_entries(g_hdr) == 0 && D->refs == 1);
    Real r;
    if (make_real(r, "d3d8.dll")) {
        g_world.real = true;
        e4_run(r.dev[0], "system d3d8 (managed textures)");
        g_world.real = false;
    } else {
        std::printf("  system d3d8: skipped (%s)\n", r.why.c_str());
    }
    free_real(r);
    if (g_d3d8_path.empty()) return;
    Real v;
    if (make_real(v, g_d3d8_path.c_str())) {
        g_world.real = true;
        e4_run(v.dev[0], "dgVoodoo d3d8 (managed textures)");
        g_world.real = false;
    } else {
        std::printf("  dgVoodoo d3d8: skipped (%s)\n", v.why.c_str());
    }
    free_real(v);
}

// ---- main ---------------------------------------------------------------------------------------

LONG WINAPI crashed(EXCEPTION_POINTERS* ep) {
    std::printf("CRASH in %s: exception %08lx at %p\n", g_test.c_str(), ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    ExitProcess(3);
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crashed);
    setvbuf(stdout, nullptr, _IONBF, 0);
    InitializeCriticalSection(&g_fake_cs);
    if (argc < 2) { std::printf("usage: test_cache.exe <dll dir> [--only a,b] [--layout f] [--dump-layout f] [--d3d8 dll]\n"); return 2; }
    g_dir = argv[1];
    std::set<std::string> only;
    for (int i = 2; i < argc; ++i) {
        std::string const a = argv[i];
        std::string const v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--only") {
            std::size_t p = 0;
            while (p <= v.size()) {
                std::size_t const q = v.find(',', p);
                only.insert(v.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            ++i;
        } else if (a == "--layout") { g_layout_path = v; ++i; }
        else if (a == "--d3d8") { g_d3d8_path = v; ++i; }
        else if (a == "--dump-layout") {
            FILE* f = std::fopen(v.c_str(), "wb");
            if (!f) return 2;
            std::string const s = layout_text();
            std::fwrite(s.data(), 1, s.size(), f);
            std::fclose(f);
            std::printf("wrote %s\n", v.c_str());
            return 0;
        }
    }
    struct Test { char const* name; void (*fn)(); };
    Test const tests[] = {{"layout", t_layout}, {"e1", t_e1}, {"e2", t_e2}, {"evict", t_evict}, {"e3c", t_e3c},
                          {"e5", t_e5}, {"e6", t_e6}, {"e8", t_e8}, {"e4", t_e4}, {"e5real", t_e5real},
                          {"hold", t_hold}, {"ld", t_ld}, {"rl", t_rl}};
    int failed = 0, ran = 0;
    for (Test const& t : tests) {
        if (!only.empty() && !only.count(t.name)) continue;
        g_test = t.name;
        int const f0 = g_fail_count;
        std::printf("== %s\n", t.name);
        t.fn();
        ++ran;
        bool const ok = g_fail_count == f0;
        if (!ok) ++failed;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
    }
    std::printf("%d of %d tests passed; fake objects %d, live fake textures %ld, underflows %ld, dead uses %ld\n", ran - failed, ran,
                int(g_fakes.size()), g_live_tex, g_underflow, g_dead_uses);
    return failed ? 1 : 0;
}
