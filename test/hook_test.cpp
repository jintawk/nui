// hook_test.cpp - offline test of UiHook.h against the real FFXiMain.dll code.
//
// Loads a COPY of FFXiMain.dll (an unpacked copy, never the install; its DllMain unpacks
// .text as in game), so the signature is searched in, and the patch written to, the real
// unpacked EndScene wrapper. The wrapper is then called through an asm harness with a fake
// renderer whose fake device's vtable slot 0x8C is a recording stub, and every register the
// caller can see is compared with an unhooked baseline call.
//
// This exe is module A; hook_client.dll (built from the same header) is module B.
//
//   hook_test.exe <path to FFXiMain.dll copy> <path to hook_client.dll>
#include "../src/UiHook/UiHook.h"

#include "hook_harness.hpp"

#include <cstdio>
#include <cstring>

using namespace harness;

namespace {

int g_pass = 0, g_fail = 0;
void check(char const* name, bool ok, char const* detail = "") {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s %s\n", name, detail); }
}

// callbacks
struct Rec { volatile int calls; void* renderer; void* device; std::uint32_t seq; };
Rec g_cb[2];
void UIHOOK_ALIGN_STACK __cdecl cb(void* user, void* renderer, void* device) {
    Rec& r = *static_cast<Rec*>(user);
    ++r.calls;
    r.renderer = renderer;
    r.device = device;
    r.seq = g_seq++;
}

Regs run_frame(std::uintptr_t site) {
    for (Rec& r : g_cb) r = Rec {};
    return run(site);
}

// one frame: every expectation about callbacks, EndScene and registers
void frame(char const* tag, std::uintptr_t site, Regs const& base, int want_cb0, int want_cb1) {
    Regs const r = run_frame(site);
    print_regs(tag, r);
    char name[128];
    auto nm = [&](char const* what) { std::snprintf(name, sizeof(name), "%s: %s", tag, what); return name; };
    check(nm("registers equal the unhooked call"), same(r, base));
    check(nm("eax is EndScene's return"), r.eax == 0x1234ABCDu);
    check(nm("esp balanced"), r.esp_delta == 0);
    check(nm("original EndScene ran exactly once"), g_es_calls == 1);
    check(nm("original EndScene got this == device"), g_es_this == reinterpret_cast<std::uintptr_t>(&g_device));
    check(nm("no other device slot called"), g_wrong_slot == 0);
    Rec const* want[2] = {&g_cb[0], &g_cb[1]};
    int const wants[2] = {want_cb0, want_cb1};
    for (int i = 0; i < 2; ++i) {
        char what[96];
        std::snprintf(what, sizeof(what), "client %d ran %d time(s)", i + 1, wants[i]);
        check(nm(what), want[i]->calls == wants[i]);
        if (wants[i]) {
            std::snprintf(what, sizeof(what), "client %d got renderer and device", i + 1);
            check(nm(what), want[i]->renderer == &g_renderer && want[i]->device == &g_device);
            std::snprintf(what, sizeof(what), "client %d ran before EndScene", i + 1);
            check(nm(what), want[i]->seq < g_es_seq);
        }
    }
}

using fn_attach = UiBus*(__cdecl*)();
using fn_int = int(__cdecl*)();
using fn_register = int(__cdecl*)(uihook_draw_fn, void*);
using fn_void = void(__cdecl*)();
using fn_ptr = void*(__cdecl*)();

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: hook_test <FFXiMain.dll copy> <hook_client.dll>\n"); return 2; }
    setup();

    // 0. no FFXiMain: fails closed
    UiBus* bus = uihook_attach();
    check("bus attach", bus != nullptr);
    check("no FFXiMain: ensure_hook fails", !uihook_ensure_hook(bus));
    check("no FFXiMain: status says so", std::strstr(bus->status, "not loaded") != nullptr, bus->status);
    std::printf("  without FFXiMain: \"%s\"\n", bus->status);

    // 1. the real unpacked wrapper
    HMODULE ffxi = LoadLibraryA(argv[1]);
    check("FFXiMain copy loaded (DllMain unpacked .text)", ffxi != nullptr);
    if (!ffxi) { std::printf("hook_test: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    char const* reason = "";
    std::uintptr_t const site = uihook_detail::find_wrapper(false, &reason);
    std::printf("  FFXiMain at %p, wrapper at %p (rva 0x%lx)\n", static_cast<void*>(ffxi), reinterpret_cast<void*>(site),
                (unsigned long)(site - reinterpret_cast<std::uintptr_t>(ffxi)));
    check("signature found exactly once", site != 0, reason);
    check("wrapper at rva 0x9740 (build 2026-10-13)", site - reinterpret_cast<std::uintptr_t>(ffxi) == 0x9740);
    check("simulated missing signature is not found", uihook_detail::find_wrapper(true, &reason) == 0);
    std::printf("  simulated: \"%s\"\n", reason);
    unsigned char before[16];
    std::memcpy(before, reinterpret_cast<void const*>(site), 16);

    Regs const base = run_frame(site);
    print_regs("unhooked baseline", base);
    check("baseline: EndScene once, this == device, eax returned",
          g_es_calls == 1 && g_es_this == reinterpret_cast<std::uintptr_t>(&g_device) && base.eax == 0x1234ABCDu);
    check("baseline: callee-saved registers and esp kept",
          base.ebx == 0x11111111u && base.esi == 0x22222222u && base.edi == 0x33333333u && base.ebp == 0x44444444u &&
              base.esp_delta == 0);

    // 2. patch, nobody registered: pass-through
    check("ensure_hook installs", uihook_ensure_hook(bus), bus->status);
    std::printf("  %s; thunk block %p\n", bus->status, static_cast<void*>(bus->block));
    auto const* p = reinterpret_cast<unsigned char const*>(site);
    check("wrapper now starts E9 rel32 90", p[0] == 0xE9 && p[5] == 0x90);
    check("bytes 6..15 untouched", std::memcmp(p + 6, before + 6, 10) == 0);
    check("installs == 1", bus->installs == 1);
    check("dispatch null before any client", *bus->dispatch_ptr == nullptr);
    LONG const d0 = bus->dispatches;
    frame("patched, no client", site, base, 0, 0);
    check("patched, no client: dispatcher not entered", bus->dispatches == d0);

    // 3. client 1 (module A)
    int const s1 = uihook_register(bus, &cb, &g_cb[0]);
    check("client 1 registered", s1 >= 0);
    check("client 1's module owns the frame", *bus->dispatch_ptr == reinterpret_cast<void*>(&uihook_detail::dispatch));
    frame("client 1", site, base, 1, 0);

    // 4. module B attaches to the existing bus: no second patch
    HMODULE mb = LoadLibraryA(argv[2]);
    check("module B loaded", mb != nullptr);
    if (!mb) { std::printf("hook_test: %d passed, %d failed\n", g_pass, g_fail); return 1; }
    auto b_attach = reinterpret_cast<fn_attach>(reinterpret_cast<void*>(GetProcAddress(mb, "client_attach")));
    auto b_ensure = reinterpret_cast<fn_int>(reinterpret_cast<void*>(GetProcAddress(mb, "client_ensure")));
    auto b_register = reinterpret_cast<fn_register>(reinterpret_cast<void*>(GetProcAddress(mb, "client_register")));
    auto b_unregister = reinterpret_cast<fn_void>(reinterpret_cast<void*>(GetProcAddress(mb, "client_unregister")));
    auto b_dispatcher = reinterpret_cast<fn_ptr>(reinterpret_cast<void*>(GetProcAddress(mb, "client_dispatcher")));
    UiBus* bbus = b_attach();
    check("module B attaches to the same bus", bbus && bbus->block == bus->block && bbus->site == site);
    unsigned char patched[16];
    std::memcpy(patched, p, 16);
    check("module B ensure_hook reports live", b_ensure() == 1);
    check("module B did not patch again (installs == 1, bytes unchanged)",
          bus->installs == 1 && std::memcmp(patched, p, 16) == 0);
    check("a fresh scan no longer finds the original bytes", uihook_detail::find_wrapper(false, &reason) == 0);
    check("module B has its own dispatcher", b_dispatcher() != reinterpret_cast<void*>(&uihook_detail::dispatch));
    int const s2 = b_register(&cb, &g_cb[1]);
    check("client 2 registered (module B)", s2 >= 0 && s2 != s1);
    frame("two clients", site, base, 1, 1);

    // 5. unregister client 1 (the owner): module B takes the frame
    uihook_unregister(bus, s1, true);
    check("owner handed to module B", *bus->dispatch_ptr == b_dispatcher());
    frame("client 2 only", site, base, 0, 1);

    // 6. client 1 back, then module B unloads (its DllMain leaves the bus)
    int const s1b = uihook_register(bus, &cb, &g_cb[0]);
    check("client 1 re-registered, B still owns", s1b >= 0 && *bus->dispatch_ptr == b_dispatcher());
    frame("both again", site, base, 1, 1);
    (void)b_unregister;
    check("module B unloaded", FreeLibrary(mb) != 0);
    check("owner handed back to module A by B's DllMain",
          *bus->dispatch_ptr == reinterpret_cast<void*>(&uihook_detail::dispatch));
    frame("after B unloaded", site, base, 1, 0);

    // 7. everyone leaves: pure pass-through
    uihook_unregister(bus, s1b, true);
    check("no clients: dispatch null", *bus->dispatch_ptr == nullptr && bus->owner_slot == -1);
    LONG const d1 = bus->dispatches;
    frame("all unregistered", site, base, 0, 0);
    check("all unregistered: dispatcher not entered", bus->dispatches == d1);
    check("ensure_hook still live, installs == 1", uihook_ensure_hook(bus) && bus->installs == 1);

    std::printf("hook_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
