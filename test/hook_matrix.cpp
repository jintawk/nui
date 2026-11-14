// hook_matrix.cpp - UiHook 2 across modules, revisions and load/unload orders (design E2, E3, E3b),
// against the real unpacked EndScene wrapper of a FFXiMain.dll copy (as hook_test.cpp).
//
//   hook_matrix.exe <FFXiMain.dll copy> <module dir> <layout_uihook.txt> [scenario]
//
// Without a scenario it runs every scenario in a child process of its own (the patch is permanent
// per process, so "who patches first" needs a fresh one), echoes their output and exits non-zero if
// any check failed or any child crashed. Each FAIL line carries a tag ([owner], [draw], ...) so the
// self-test can require that a deliberately broken build is caught by the check aimed at it.
//
// The modules (hook_mod.cpp variants, in <module dir>):
//   mod_a1 mod_b1 mod_c1   rev 1, names a b c          mod_b2 mod_c3   rev 2, rev 3 (simulated newer)
//   mod_s1   rev 1, cannot find the wrapper itself    mod_d1   rev 1, DllMain does not unregister
//   mod_x0   built from the frozen pre-release ABI 1 header
//
// patch (review HOOK-1): a module's site write flips 8 bytes between the wrapper's original start and
// the patch 200,000 times each way while another thread reads them: no read may see a mix of the two.
// drain (review HOOK-2): frames run on a thread of their own; a driver's stop() returns only after
// the frame moved on by two dispatcher entries, and the last driver's stop waits before returning.
#include "hook_layout.hpp"

#include "hook_harness.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace harness;

namespace {

// ---- checks -------------------------------------------------------------------------------------
int g_pass = 0, g_fail = 0, g_printed = 0;
char g_scenario[32] = "";
char g_ctx[160] = "";

void ctx(char const* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_ctx, sizeof(g_ctx), fmt, ap);
    va_end(ap);
}

bool check(char const* tag, bool ok, char const* fmt, ...) {
    if (ok) { ++g_pass; return true; }
    ++g_fail;
    if (g_printed++ < 40) {
        char what[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(what, sizeof(what), fmt, ap);
        va_end(ap);
        std::printf("FAIL [%s] %s: %s: %s\n", tag, g_scenario, g_ctx, what);
        std::fflush(stdout);
    }
    return false;
}

int result() {
    std::printf("RESULT %s %d %d\n", g_scenario, g_pass, g_fail);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}

// ---- modules ------------------------------------------------------------------------------------
typedef void(__cdecl* sink_fn)(int tag, void* renderer, void* device);
using fn_start = int(__cdecl*)(sink_fn, int);
using fn_void = void(__cdecl*)();
using fn_ptr = void*(__cdecl*)();
using fn_int = int(__cdecl*)();
using fn_str = char const*(__cdecl*)();
using fn_write8 = int(__cdecl*)(void*, unsigned char const*, unsigned char const*);

struct Mod {
    char const* key;
    int rev;            // 0 = ABI 1
    std::string path, base;
    HMODULE h = nullptr;
    fn_start start = nullptr;
    fn_void stop = nullptr;
    fn_ptr dispatcher = nullptr;
    fn_int slot = nullptr, state = nullptr;
    fn_str layout = nullptr;
    fn_write8 write8 = nullptr;
    bool live = false;  // started successfully, not stopped, still loaded
    int slot_no = -1;
    bool layout_checked = false;
    std::uintptr_t lo = 0, hi = 0;    // image range while loaded
};

std::vector<Mod> g_mods;
std::string g_layout_file;

struct Rec { int calls; void* renderer; void* device; std::uint32_t seq; };
Rec g_rec[16];

void __cdecl sink(int tag, void* renderer, void* device) {
    if (tag < 0 || tag >= 16) return;
    Rec& r = g_rec[tag];
    ++r.calls;
    r.renderer = renderer;
    r.device = device;
    r.seq = g_seq++;
}

int idx(char const* key) {
    for (std::size_t i = 0; i < g_mods.size(); ++i)
        if (std::strcmp(g_mods[i].key, key) == 0) return static_cast<int>(i);
    std::printf("unknown module %s\n", key);
    std::exit(2);
}

std::string normalise(std::string const& text) {
    std::string out, line;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        char const ch = i < text.size() ? text[i] : '\n';
        if (ch == '\r') continue;
        if (ch != '\n') { line += ch; continue; }
        if (!line.empty() && line[0] != '#') out += line + "\n";
        line.clear();
    }
    return out;
}

std::string read_file(std::string const& p) {
    std::string s;
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

std::string first_diff(std::string const& want, std::string const& got) {
    std::size_t a = 0, b = 0;
    while (a < want.size() || b < got.size()) {
        std::size_t const ea = want.find('\n', a), eb = got.find('\n', b);
        std::string const la = a < want.size() ? want.substr(a, ea - a) : "<end>";
        std::string const lb = b < got.size() ? got.substr(b, eb - b) : "<end>";
        if (la != lb) return "file '" + la + "' vs build '" + lb + "'";
        a = ea == std::string::npos ? want.size() : ea + 1;
        b = eb == std::string::npos ? got.size() : eb + 1;
    }
    return "";
}

template <typename T>
T bind(HMODULE h, char const* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(h, name)));
}

bool load(Mod& m) {
    m.h = LoadLibraryA(m.path.c_str());
    if (!check("setup", m.h != nullptr, "load %s (error %lu)", m.base.c_str(), GetLastError())) return false;
    m.start = bind<fn_start>(m.h, "m_start");
    m.stop = bind<fn_void>(m.h, "m_stop");
    m.dispatcher = bind<fn_ptr>(m.h, "m_dispatcher");
    m.slot = bind<fn_int>(m.h, "m_slot");
    m.state = bind<fn_int>(m.h, "m_state");
    m.layout = bind<fn_str>(m.h, "m_layout");
    m.write8 = bind<fn_write8>(m.h, "m_write8");
    MEMORY_BASIC_INFORMATION r {};
    VirtualQuery(m.h, &r, sizeof(r));
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(m.h);
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(reinterpret_cast<char const*>(m.h) + dos->e_lfanew);
    m.lo = reinterpret_cast<std::uintptr_t>(m.h);
    m.hi = m.lo + nt->OptionalHeader.SizeOfImage;
    if (m.rev > 0 && !m.layout_checked) {
        m.layout_checked = true;
        std::string const want = normalise(read_file(g_layout_file));
        std::string const got = normalise(m.layout());
        check("layout", !want.empty() && want == got, "%s's UiHook layout differs from layout_uihook.txt: %s",
              m.base.c_str(), first_diff(want, got).c_str());
    }
    return true;
}

// ---- the bus, seen from the host -----------------------------------------------------------------
UiBus* g_bus = nullptr;    // the host's own view (it never registers)

UiBus* bus() {
    if (g_bus) return g_bus;
    char name[64];
    std::snprintf(name, sizeof(name), UIHOOK_MAP_NAME_FMT, static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!h) return nullptr;
    g_bus = static_cast<UiBus*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    return g_bus;
}

// ABI 1's header prefix is the same as ABI 2's up to `site`; read it by those offsets.
struct V1Prefix {
    LONG ready; unsigned long abi_version, size; LONG install_lock, hook_installed, owner_slot, in_flight, installs,
        dispatches; void* block; void** dispatch_ptr; std::uintptr_t site;
};
V1Prefix const* v1bus() {
    char name[64];
    std::snprintf(name, sizeof(name), "Local\\FFXiUiHook_v1_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!h) return nullptr;
    return static_cast<V1Prefix const*>(MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0));
}

bool in_loaded_image(void const* p) {
    MEMORY_BASIC_INFORMATION r {};
    return p && VirtualQuery(p, &r, sizeof(r)) && r.State == MEM_COMMIT && r.Type == MEM_IMAGE;
}

// ---- one frame and its invariants -----------------------------------------------------------------
std::uintptr_t g_site = 0;
Regs g_base;
unsigned char g_orig[16];

// The frame must be driven by a live v2 client of the highest live revision (ties: any of them),
// with owner_slot / owner_rev matching; nobody when no v2 client is live. False = the dispatcher
// pointer is dangling (a frame would crash): the scenario stops here.
bool verify_owner() {
    UiBus* b = bus();
    int maxrev = 0, nlive = 0;
    for (Mod const& m : g_mods)
        if (m.live && m.rev > 0) { ++nlive; maxrev = std::max(maxrev, m.rev); }
    if (!b || !b->hook_installed) {
        check("owner", nlive == 0, "%d v2 clients live without a v2 hook", nlive);
        return true;
    }
    void* const dp = *b->dispatch_ptr;
    if (nlive == 0) {
        check("handover", dp == nullptr && b->owner_slot == -1 && b->owner_rev == 0,
              "no client live but dispatch %p owner_slot %ld owner_rev %ld", dp, b->owner_slot, b->owner_rev);
        if (dp && !in_loaded_image(dp)) {
            check("handover", false, "dispatcher %p is unmapped: stopping", dp);
            return false;
        }
        return true;
    }
    Mod const* driver = nullptr;
    for (Mod const& m : g_mods)
        if (m.live && m.rev > 0 && m.dispatcher() == dp) driver = &m;
    if (!driver) {
        bool const mapped = in_loaded_image(dp);
        check("handover", false, "the frame is driven by %p, no live client's dispatcher%s", dp,
              mapped ? " (a module that left)" : " (unmapped: stopping)");
        return mapped;
    }
    check("owner", driver->rev == maxrev, "driven by %s rev %d, highest live rev is %d", driver->key, driver->rev, maxrev);
    check("owner", b->owner_slot == driver->slot_no && b->owner_rev == driver->rev,
          "owner_slot %ld owner_rev %ld, driver %s slot %d rev %d", b->owner_slot, b->owner_rev, driver->key,
          driver->slot_no, driver->rev);
    return true;
}

bool frame(char const* step) {
    ctx("%s", step);
    if (!verify_owner()) return false;
    UiBus* b = bus();
    LONG const d0 = b ? b->dispatches : 0;
    for (Rec& r : g_rec) r = Rec {};
    g_wrong_slot = 0;
    Regs const r = run(g_site);
    check("regs", same(r, g_base), "registers differ from the unhooked call (eax %08lx esp delta %lu)",
          (unsigned long)r.eax, (unsigned long)r.esp_delta);
    check("regs", g_es_calls == 1 && g_es_this == reinterpret_cast<std::uintptr_t>(&g_device) && g_wrong_slot == 0,
          "EndScene ran %lu times, this %08lx, wrong slots %lu", (unsigned long)g_es_calls, (unsigned long)g_es_this,
          (unsigned long)g_wrong_slot);
    int nlive = 0, nlive2 = 0;
    for (std::size_t i = 0; i < g_mods.size(); ++i) {
        Mod const& m = g_mods[i];
        Rec const& c = g_rec[i];
        if (m.live) {
            ++nlive;
            if (m.rev > 0) ++nlive2;
            check("draw", c.calls == 1, "%s drawn %d times, want 1", m.key, c.calls);
            check("draw", c.calls == 0 || (c.renderer == &g_renderer && c.device == &g_device && c.seq < g_es_seq),
                  "%s got renderer %p device %p seq %lu (EndScene seq %lu)", m.key, c.renderer, c.device,
                  (unsigned long)c.seq, (unsigned long)g_es_seq);
        } else {
            check("draw", c.calls == 0, "%s is not live but was drawn %d times", m.key, c.calls);
        }
    }
    if (b && b->hook_installed) {
        check("clients", uihook_active_clients(b) == nlive2, "%d active clients on the bus, %d live v2 modules",
              uihook_active_clients(b), nlive2);
        check("install", b->installs == 1, "installs %ld", b->installs);
        if (nlive2 == 0) check("passthrough", b->dispatches == d0, "dispatcher entered with no client");
    }
    (void)nlive;
    return true;
}

bool start(char const* key, int want) {   // want: 1 = a slot, else the exact code
    Mod& m = g_mods[idx(key)];
    if (!m.h && !load(m)) return false;
    int const r = m.start(&sink, idx(key));
    ctx("start %s", key);
    if (want == 1) {
        if (!check("join", r >= 0, "%s start returned %d (state %d, status \"%s\")", key, r, m.state(),
                   bus() ? bus()->status : "no bus"))
            return false;
        m.live = true;
        m.slot_no = r;
    } else {
        check(want == UIHOOK_JOIN_BLOCKED ? "abi1" : "join", r == want, "%s start returned %d, want %d (status \"%s\")",
              key, r, want, bus() ? bus()->status : "no bus");
        m.live = false;
    }
    return true;
}

void stop(char const* key) {
    Mod& m = g_mods[idx(key)];
    m.stop();
    m.live = false;
    m.slot_no = -1;
}

void unload(char const* key) {
    Mod& m = g_mods[idx(key)];
    FreeLibrary(m.h);
    ctx("unload %s", key);
    check("unload", GetModuleHandleA(m.base.c_str()) == nullptr, "%s is still loaded", m.base.c_str());
    m.h = nullptr;
    m.live = false;
    m.slot_no = -1;
}

// ---- dead-client trap: a call into an unloaded module's image ------------------------------------
std::uintptr_t g_dead_lo = 0, g_dead_hi = 0;
LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
    auto const code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    auto const at = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    if (g_dead_lo && at >= g_dead_lo && at < g_dead_hi)
        check("liveness", false, "a dead client's draw (unloaded module) was called at %p", ep->ExceptionRecord->ExceptionAddress);
    else
        check("crash", false, "exception %08lx at %p", (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
    result();
    ExitProcess(3);
}

// ---- scenarios --------------------------------------------------------------------------------------
bool site_bytes_are(unsigned char const* want) { return std::memcmp(reinterpret_cast<void const*>(g_site), want, 16) == 0; }

// first: the first patch and the ABI 2 mapping
void sc_first() {
    if (!start("a1", 1)) return;
    UiBus* b = bus();
    ctx("after the first start");
    if (!check("install", b != nullptr, "no v2 mapping")) return;
    auto const* p = reinterpret_cast<unsigned char const*>(g_site);
    check("install", b->installs == 1 && b->hook_installed == 1 && b->site == g_site, "installs %ld site %08lx",
          b->installs, (unsigned long)b->site);
    check("install", p[0] == 0xE9 && p[5] == 0x90 && std::memcmp(p + 6, g_orig + 6, 10) == 0,
          "wrapper is not E9 rel32 90 + original bytes 6..15");
    unsigned char want[uihook_detail::kThunkBytes];
    uihook_detail::thunk_bytes(reinterpret_cast<std::uintptr_t>(b->block), g_site, want);
    check("install", reinterpret_cast<std::uintptr_t>(b->dispatch_ptr) == reinterpret_cast<std::uintptr_t>(b->block)
                         && std::memcmp(b->block + uihook_detail::kThunkAt, want, sizeof(want)) == 0,
          "thunk bytes differ");
    check("install", std::strstr(b->status, "hook installed") && std::strstr(b->status, "by a (rev 1)"),
          "status \"%s\"", b->status);
    check("abi", b->abi_version == 2 && b->size == sizeof(UiBus), "abi %lu size %lu", b->abi_version, b->size);
    MEMORY_BASIC_INFORMATION r {};
    VirtualQuery(b, &r, sizeof(r));
    check("abi", r.RegionSize == UIHOOK_MAP_BYTES, "mapping view is %lu bytes", (unsigned long)r.RegionSize);
    check("abi", v1bus() == nullptr, "a v1 mapping exists");
    check("state", g_mods[idx("a1")].state() == UIHOOK_LIVE, "state %d", g_mods[idx("a1")].state());
    char line[256];
    uihook_describe(b, line, sizeof(line));
    check("status", std::strstr(line, "1 clients: a r1 (drives)") != nullptr, "describe \"%s\"", line);
    std::string const want_layout = normalise(read_file(g_layout_file));
    char host_layout[4096];
    uihook_layout_text(host_layout, sizeof(host_layout));
    check("layout", !want_layout.empty() && want_layout == normalise(host_layout),
          "the canonical header's layout differs from layout_uihook.txt: %s",
          first_diff(want_layout, normalise(host_layout)).c_str());
    if (!frame("a1 alone")) return;
    // a sig-blind module joins at once when the hook is already there; the register lock is held by
    // a thread that is gone (a stale token): it is broken after about 50 ms, not waited on forever
    InterlockedExchange(&b->reg_lock, 0x7FFFFFF0);
    DWORD const t0 = GetTickCount();
    if (!start("s1", 1)) return;
    DWORD const waited = GetTickCount() - t0;
    ctx("stale register lock");
    check("lock", b->lock_breaks == 1 && b->reg_lock == 0 && waited < 2000, "lock_breaks %ld reg_lock %08lx, waited %lu ms",
          b->lock_breaks, b->reg_lock, (unsigned long)waited);
    if (!frame("a1 + s1 (joined at once)")) return;
    uihook_describe(b, line, sizeof(line));
    check("status", std::strstr(line, "2 clients: a r1 (drives), s r1") != nullptr, "describe \"%s\"", line);
    unload("s1");
    if (!frame("s1 unloaded without stop")) return;
    stop("a1");
    if (!frame("a1 stopped")) return;
    unload("a1");
    frame("a1 unloaded");
    ctx("end");
    check("install", b->installs == 1 && b->handovers >= 2, "installs %ld handovers %ld", b->installs, b->handovers);
}

// every load order x unload order x {stop then unload, unload only} per module, of 2 and 3 modules
bool sequence(std::vector<char const*> const& load_order, std::vector<char const*> const& unload_order, unsigned mask) {
    char step[96];
    std::string tag;
    for (auto k : load_order) tag += std::string(k) + ">";
    tag += " / ";
    for (std::size_t i = 0; i < unload_order.size(); ++i)
        tag += std::string(unload_order[i]) + ((mask >> i) & 1 ? "(free) " : "(stop) ");
    for (auto k : load_order) {
        if (!start(k, 1)) return false;
        std::snprintf(step, sizeof(step), "%s: %s started", tag.c_str(), k);
        if (!frame(step)) return false;
    }
    for (std::size_t i = 0; i < unload_order.size(); ++i) {
        char const* k = unload_order[i];
        if (((mask >> i) & 1) == 0) {
            stop(k);
            std::snprintf(step, sizeof(step), "%s: %s stopped", tag.c_str(), k);
            if (!frame(step)) return false;
        }
        unload(k);
        std::snprintf(step, sizeof(step), "%s: %s unloaded", tag.c_str(), k);
        if (!frame(step)) return false;
    }
    return true;
}

bool by_name(char const* a, char const* b) { return std::strcmp(a, b) < 0; }

void sc_matrix(std::vector<char const*> keys) {
    int seqs = 0;
    std::sort(keys.begin(), keys.end(), by_name);
    // every pair: 2 load orders x 2 unload orders x 4 stop/free masks
    for (std::size_t i = 0; i < keys.size(); ++i)
        for (std::size_t j = i + 1; j < keys.size(); ++j) {
            std::vector<char const*> lo {keys[i], keys[j]};
            do {
                std::vector<char const*> ul {keys[i], keys[j]};
                do {
                    for (unsigned mask = 0; mask < 4; ++mask) {
                        ++seqs;
                        if (!sequence(lo, ul, mask)) return;
                    }
                } while (std::next_permutation(ul.begin(), ul.end(), by_name));
            } while (std::next_permutation(lo.begin(), lo.end(), by_name));
        }
    // all three: 6 load orders x 6 unload orders x 8 masks
    std::vector<char const*> lo = keys;
    do {
        std::vector<char const*> ul = keys;
        do {
            for (unsigned mask = 0; mask < 8; ++mask) {
                ++seqs;
                if (!sequence(lo, ul, mask)) return;
            }
        } while (std::next_permutation(ul.begin(), ul.end(), by_name));
    } while (std::next_permutation(lo.begin(), lo.end(), by_name));
    std::printf("  %s: %d sequences\n", g_scenario, seqs);
}

// sigblind: a module that cannot find the wrapper waits, then joins once a newer one has patched
void sc_sigblind() {
    if (!start("s1", UIHOOK_JOIN_RETRY)) return;
    UiBus* b = bus();
    ctx("s1 alone");
    if (!check("join", b != nullptr, "no bus")) return;
    check("join", g_mods[idx("s1")].state() == UIHOOK_WAIT, "state %d, want WAIT", g_mods[idx("s1")].state());
    check("join", b->installs == 0 && site_bytes_are(g_orig), "s1 changed the wrapper (installs %ld)", b->installs);
    check("join", std::strstr(b->status, "not found") != nullptr, "status \"%s\"", b->status);
    char line[256];
    uihook_describe(b, line, sizeof(line));
    check("status", std::strncmp(line, "waiting: ", 9) == 0, "describe \"%s\"", line);
    if (!frame("s1 waiting: pass-through")) return;
    if (!start("s1", UIHOOK_JOIN_RETRY)) return;          // the slow-timer retry, still nothing
    if (!start("b2", 1)) return;                          // the newer module knows the wrapper
    if (!frame("b2 patched, s1 not yet retried")) return;
    if (!start("s1", 1)) return;                          // the retry joins
    if (!frame("s1 joined b2's hook")) return;
    stop("b2");
    if (!frame("b2 stopped: s1 drives")) return;
    unload("b2");
    if (!frame("b2 unloaded")) return;
    if (!start("b2", 1)) return;
    if (!frame("b2 back: takes the frame from s1")) return;
    unload("s1");
    if (!frame("s1 unloaded without stop")) return;
    stop("b2");
    unload("b2");
    frame("all gone");
}

// abi1_first: a pre-release ABI 1 module patched first; UiHook 2 must not patch over it
void sc_abi1_first() {
    if (!start("x0", 1)) return;
    V1Prefix const* v1 = v1bus();
    ctx("x0 patched");
    if (!check("setup", v1 && v1->installs == 1 && v1->site == g_site, "the ABI 1 module did not patch")) return;
    unsigned char after[16];
    std::memcpy(after, reinterpret_cast<void const*>(g_site), 16);
    if (!frame("x0 alone")) return;
    if (!start("a1", UIHOOK_JOIN_BLOCKED)) return;
    UiBus* b = bus();
    ctx("a1 refused");
    if (!check("abi1", b != nullptr, "no v2 bus")) return;
    check("abi1", g_mods[idx("a1")].state() == UIHOOK_BLOCKED, "state %d, want BLOCKED", g_mods[idx("a1")].state());
    check("abi1", std::strstr(b->status, "ABI 1") != nullptr, "status \"%s\"", b->status);
    check("abi1", b->installs == 0 && b->hook_installed == 0 && b->blocked == 1 && b->foreign_site == g_site,
          "v2 installs %ld blocked %ld foreign %08lx", b->installs, b->blocked, (unsigned long)b->foreign_site);
    check("abi1", site_bytes_are(after) && v1->installs == 1, "the wrapper changed after the ABI 1 patch");
    char line[256];
    uihook_describe(b, line, sizeof(line));
    check("status", std::strncmp(line, "blocked: ", 9) == 0, "describe \"%s\"", line);
    if (!frame("x0 drawn, a1 not")) return;
    if (!start("a1", UIHOOK_JOIN_BLOCKED)) return;         // retry: still blocked
    if (!start("b2", UIHOOK_JOIN_BLOCKED)) return;         // a newer revision too
    if (!frame("x0 drawn, a1 and b2 refused")) return;
    stop("x0");
    if (!frame("x0 stopped")) return;
    unload("x0");
    if (!frame("x0 unloaded: its patch stays")) return;
    if (!start("a1", UIHOOK_JOIN_BLOCKED)) return;
    ctx("end");
    check("abi1", b->installs == 0 && site_bytes_are(after), "UiHook 2 patched after all");
    unload("a1");
    unload("b2");
}

// abi1_after: UiHook 2 patched first; a pre-release module loading later changes nothing
void sc_abi1_after() {
    if (!start("a1", 1)) return;
    unsigned char after[16];
    std::memcpy(after, reinterpret_cast<void const*>(g_site), 16);
    if (!start("x0", -1)) return;
    ctx("x0 refused");
    V1Prefix const* v1 = v1bus();
    check("abi1", v1 && v1->installs == 0, "the ABI 1 module patched");
    check("abi1", site_bytes_are(after) && bus()->installs == 1, "the wrapper changed");
    if (!frame("a1 drawn, x0 not")) return;
    unload("x0");
    stop("a1");
    unload("a1");
    frame("all gone");
}

// dead: a module unloaded without its DllMain unregistering
void sc_dead() {
    if (!start("a1", 1) || !start("d1", 1)) return;
    if (!frame("a1 + d1")) return;
    Mod& d = g_mods[idx("d1")];
    int const dslot = d.slot_no;
    g_dead_lo = d.lo;
    g_dead_hi = d.hi;
    FreeLibrary(d.h);
    d.h = nullptr;
    d.live = false;
    d.slot_no = -1;
    UiBus* b = bus();
    ctx("d1 freed without unregistering");
    check("setup", GetModuleHandleA(d.base.c_str()) == nullptr && b->clients[dslot].occupied == 2,
          "d1 still loaded, or its record went away");
    LONG const rec0 = b->reclaimed;
    if (!frame("a1 drives past d1's dead record")) return;
    ctx("after the frame");
    check("liveness", b->reclaimed == rec0 + 1 && b->clients[dslot].occupied == 0, "reclaimed %ld, slot %d occupied %ld",
          b->reclaimed, dslot, b->clients[dslot].occupied);
    if (!start("b1", 1)) return;
    check("liveness", g_mods[idx("b1")].slot_no == dslot, "b1 got slot %d, the freed one was %d", g_mods[idx("b1")].slot_no, dslot);
    if (!frame("b1 in d1's old slot")) return;
    stop("a1");
    unload("a1");
    unload("b1");
    if (!frame("all gone")) return;

    // the owner dies without unregistering: the next register repairs the frame before any frame runs
    if (!start("d1", 1)) return;
    g_dead_lo = d.lo;
    g_dead_hi = d.hi;
    int const dslot2 = d.slot_no;
    FreeLibrary(d.h);
    d.h = nullptr;
    d.live = false;
    d.slot_no = -1;
    if (!start("a1", 1)) return;
    std::printf("  dead: a1 mapped at the dead owner's old base: %s\n", g_mods[idx("a1")].lo == g_dead_lo ? "yes" : "no");
    ctx("a1 registered after the owner died");
    check("repair", *b->dispatch_ptr == g_mods[idx("a1")].dispatcher(), "the dead owner still drives");
    if (!frame("a1 repaired the frame")) return;
    check("liveness", b->clients[dslot2].occupied == 0 && uihook_active_clients(b) == 1,
          "the dead owner's record was not reclaimed");
    stop("a1");
    unload("a1");
    frame("all gone again");
}

// patch (review HOOK-1): 8 bytes flipped between the original and the patch while another thread reads
volatile LONG g_patch_run = 0, g_patch_reads = 0, g_patch_torn = 0;
unsigned char g_patch_a[8], g_patch_b[8];
volatile LONGLONG* g_patch_at = nullptr;
DWORD WINAPI patch_reader(void*) {
    while (g_patch_run) {
        LONGLONG const v = InterlockedCompareExchange64(g_patch_at, 0, 0);
        if (std::memcmp(&v, g_patch_a, 8) != 0 && std::memcmp(&v, g_patch_b, 8) != 0) InterlockedIncrement(&g_patch_torn);
        InterlockedIncrement(&g_patch_reads);
    }
    return 0;
}
void sc_patch() {
    Mod& m = g_mods[idx("a1")];
    if (!m.h && !load(m)) return;
    ctx("site write");
    auto* buf = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!check("setup", buf != nullptr && m.write8 != nullptr, "no buffer or no m_write8")) return;
    unsigned char* site = buf + 16;                  // 16-aligned, as the wrapper
    std::memcpy(g_patch_a, uihook_detail::kWrapperSig, 8);
    unsigned char const b[8] = {0xE9, 0x41, 0x0C, 0x50, 0x8B, 0x90, g_patch_a[6], g_patch_a[7]};   // bytes 0..5 all differ
    std::memcpy(g_patch_b, b, 8);
    std::memcpy(site, g_patch_a, 8);
    g_patch_at = reinterpret_cast<volatile LONGLONG*>(site);
    g_patch_run = 1;
    HANDLE th = CreateThread(nullptr, 0, &patch_reader, nullptr, 0, nullptr);
    while (th && g_patch_reads < 1000) Sleep(0);
    int bad = 0;
    for (int i = 0; i < 200000; ++i) {
        if (m.write8(site, g_patch_b, g_patch_a) != 1) ++bad;
        if (m.write8(site, g_patch_a, g_patch_b) != 1) ++bad;
    }
    int const refused = m.write8(site, g_patch_b, g_patch_b);   // the site does not hold `expect`
    g_patch_run = 0;
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    SYSTEM_INFO si {};
    GetSystemInfo(&si);
    std::printf("  patch: %ld reads during 400,000 site writes, %ld torn (%lu CPUs)\n", g_patch_reads, g_patch_torn,
                (unsigned long)si.dwNumberOfProcessors);
    check("patch", th != nullptr && si.dwNumberOfProcessors >= 2, "needs a reader thread on a second CPU");
    check("patch", g_patch_torn == 0, "%ld reads saw a mix of the original bytes and the patch (a torn site write)", g_patch_torn);
    check("patch", bad == 0 && refused == 0 && std::memcmp(site, g_patch_a, 8) == 0,
          "%d writes failed, a write over unexpected bytes returned %d", bad, refused);
    VirtualFree(buf, 0, MEM_RELEASE);
}

// drain (review HOOK-2): frames on their own thread, as the game's render thread
volatile LONG g_drain_run = 0;
DWORD WINAPI drain_frames(void*) {
    while (g_drain_run) {
        run(g_site);
        Sleep(2);
    }
    return 0;
}
void sc_drain() {
    if (!start("a1", 1) || !start("b1", 1)) return;
    if (!frame("a1 drives, b1 joined")) return;
    UiBus* b = bus();
    LONG const d0 = b->dispatches;
    g_drain_run = 1;
    HANDLE th = CreateThread(nullptr, 0, &drain_frames, nullptr, 0, nullptr);
    DWORD const w0 = GetTickCount();
    while (th && b->dispatches - d0 < 5 && GetTickCount() - w0 < 5000) Sleep(1);
    ctx("frames on a render thread");
    if (!check("setup", th && b->dispatches - d0 >= 5, "the frame thread did not run")) { g_drain_run = 0; return; }
    // the driver leaves: the frame goes to b1, and stop() returns once it moved on twice
    LONG const d1 = b->dispatches;
    stop("a1");
    LONG const moved = b->dispatches - d1;
    check("drain", moved >= 2, "the driver's stop() returned after %ld dispatcher entries, want 2 (the render thread may still be in its code)", moved);
    check("drain", *b->dispatch_ptr == g_mods[idx("b1")].dispatcher(), "b1 does not drive after a1 left");
    // the last driver leaves: nobody to hand to, so the whole drain time passes
    DWORD const t0 = GetTickCount();
    stop("b1");
    DWORD const waited = GetTickCount() - t0;
    check("drain", waited >= 50 && waited < 2000, "the last driver's stop() returned after %lu ms, want the drain time", (unsigned long)waited);
    g_drain_run = 0;
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
    std::printf("  drain: the hand-over waited for %ld entries, the last driver %lu ms\n", moved, (unsigned long)waited);
    unload("a1");
    unload("b1");
    frame("all gone");
}

// ---- driver -------------------------------------------------------------------------------------
char const* const kScenarios[] = {"first", "same", "revs", "sigblind", "abi1_first", "abi1_after", "dead", "patch", "drain"};

int run_child_scenarios(char** argv) {
    int total_pass = 0, total_fail = 0, bad = 0;
    char self[MAX_PATH];
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    for (char const* sc : kScenarios) {
        std::string cmd = std::string("\"") + self + "\" \"" + argv[1] + "\" \"" + argv[2] + "\" \"" + argv[3] + "\" " + sc;
        SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 0);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi {};
        std::vector<char> cl(cmd.begin(), cmd.end());
        cl.push_back(0);
        if (!CreateProcessA(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::printf("FAIL [crash] cannot start scenario %s\n", sc);
            ++bad;
            CloseHandle(rd);
            CloseHandle(wr);
            continue;
        }
        CloseHandle(wr);
        std::string out;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        std::fputs(out.c_str(), stdout);
        int p = 0, f = 0;
        std::string const key = std::string("RESULT ") + sc + " ";
        std::size_t const at = out.find(key);
        if (at == std::string::npos || std::sscanf(out.c_str() + at + key.size(), "%d %d", &p, &f) != 2) {
            std::printf("FAIL [crash] scenario %s ended (exit %08lx) without a result\n", sc, (unsigned long)code);
            ++bad;
            continue;
        }
        std::printf("  scenario %-10s %6d passed, %d failed\n", sc, p, f);
        total_pass += p;
        total_fail += f;
        if (f || code) ++bad;
    }
    std::printf("hook_matrix: %d passed, %d failed, %d of %d scenarios failed\n", total_pass, total_fail, bad,
                static_cast<int>(sizeof(kScenarios) / sizeof(kScenarios[0])));
    return bad || total_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--layout") == 0) {     // print this build's layout (to seed the file)
        char text[4096];
        uihook_layout_text(text, sizeof(text));
        std::fputs(text, stdout);
        return 0;
    }
    if (argc < 4) {
        std::printf("usage: hook_matrix <FFXiMain.dll copy> <module dir> <layout_uihook.txt> [scenario]\n");
        return 2;
    }
    if (argc < 5) return run_child_scenarios(argv);

    std::snprintf(g_scenario, sizeof(g_scenario), "%s", argv[4]);
    g_layout_file = argv[3];
    struct { char const* key; int rev; } const table[] = {
        {"a1", 1}, {"b1", 1}, {"c1", 1}, {"b2", 2}, {"c3", 3}, {"s1", 1}, {"d1", 1}, {"x0", 0},
    };
    for (auto const& t : table) {
        Mod m;
        m.key = t.key;
        m.rev = t.rev;
        m.base = std::string("mod_") + t.key + ".dll";
        m.path = std::string(argv[2]) + "\\" + m.base;
        g_mods.push_back(m);
    }
    setup();

    ctx("setup");
    HMODULE ffxi = LoadLibraryA(argv[1]);
    if (!check("setup", ffxi != nullptr, "FFXiMain copy not loaded")) return result();
    char const* reason = "";
    g_site = uihook_detail::find_wrapper(false, &reason);
    if (!check("setup", g_site != 0, "wrapper not found: %s", reason)) return result();
    std::memcpy(g_orig, reinterpret_cast<void const*>(g_site), 16);
    AddVectoredExceptionHandler(1, &on_fault);
    g_base = run(g_site);
    check("setup", g_es_calls == 1 && g_base.eax == 0x1234ABCDu && g_base.esp_delta == 0, "unhooked baseline wrong");

    std::string const sc = g_scenario;
    if (sc == "first") sc_first();
    else if (sc == "same") sc_matrix({"a1", "b1", "c1"});
    else if (sc == "revs") sc_matrix({"a1", "b2", "c3"});
    else if (sc == "sigblind") sc_sigblind();
    else if (sc == "abi1_first") sc_abi1_first();
    else if (sc == "abi1_after") sc_abi1_after();
    else if (sc == "dead") sc_dead();
    else if (sc == "patch") sc_patch();
    else if (sc == "drain") sc_drain();
    else { std::printf("unknown scenario %s\n", g_scenario); return 2; }
    return result();
}
