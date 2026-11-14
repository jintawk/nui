// hook_mod.cpp - a bare UiHook module for hook_matrix.exe, built in variants (build_test.sh):
//
//   -DHOOKMOD_NAME=\"a\"        its client name
//   -DUIHOOK_REV=n              the revision it claims (1 = this header; 2, 3 = simulated newer ones)
//   -DHOOKMOD_ORDER=n           the draw order hint it registers with
//   -DUIHOOK_TEST_SIG_BLIND     its own signature scan never finds the wrapper (an older module after a
//                               client update): it can only join a hook another module installed
//   -DHOOKMOD_NO_DETACH         its DllMain does not unregister: unloading leaves a dead client record
//   -DHOOKMOD_ABI1              built from the frozen pre-release header (test/abi1/UiHook_abi1.h)
//   -DUIHOOK_FAULT_*            a deliberately broken UiHook (the self-test requires the matrix to fail)
//
// Its draw callback lives in this DLL (so unloading it really unmaps it) and reports to the host's
// sink with the module's tag.
#ifdef HOOKMOD_ABI1
#include "abi1/UiHook_abi1.h"
#else
#include "hook_layout.hpp"
#endif

#ifndef HOOKMOD_NAME
#define HOOKMOD_NAME "m"
#endif
#ifndef HOOKMOD_ORDER
#define HOOKMOD_ORDER 0
#endif

typedef void(__cdecl* sink_fn)(int tag, void* renderer, void* device);

namespace {
UiBus* g_bus = nullptr;
int g_slot = -1;
sink_fn g_sink = nullptr;
int g_tag = 0;
char g_layout[4096];

void UIHOOK_ALIGN_STACK __cdecl my_draw(void*, void* renderer, void* device) {
    if (g_sink) g_sink(g_tag, renderer, device);
}
}  // namespace

extern "C" {
// slot >= 0, or a UIHOOK_JOIN_* code (ABI 1: -1 not hooked, -3 full, -4 no bus)
__declspec(dllexport) int __cdecl m_start(sink_fn sink, int tag) {
    g_sink = sink;
    g_tag = tag;
    if (!g_bus) g_bus = uihook_attach();
    if (!g_bus) return -4;
    if (g_slot >= 0) return g_slot;
#ifdef HOOKMOD_ABI1
    if (!uihook_ensure_hook(g_bus)) return -1;
    g_slot = uihook_register(g_bus, &my_draw, nullptr);
    return g_slot >= 0 ? g_slot : -3;
#else
    int const r = uihook_join(g_bus, &my_draw, nullptr, HOOKMOD_NAME, HOOKMOD_ORDER);
    if (r >= 0) g_slot = r;
    return r;
#endif
}
__declspec(dllexport) void __cdecl m_stop() {
    uihook_unregister(g_bus, g_slot, true);
    g_slot = -1;
}
// review HOOK-1: this module's site write (UiHook.h write_site), aimed at a test buffer
__declspec(dllexport) int __cdecl m_write8(void* site, unsigned char const* want, unsigned char const* expect) {
#ifdef HOOKMOD_ABI1
    (void)site; (void)want; (void)expect;
    return -1;
#else
    return uihook_detail::write_site(reinterpret_cast<std::uintptr_t>(site), want, expect) ? 1 : 0;
#endif
}
__declspec(dllexport) void* __cdecl m_dispatcher() { return reinterpret_cast<void*>(&uihook_detail::dispatch); }
__declspec(dllexport) void* __cdecl m_bus() { return g_bus; }
__declspec(dllexport) int __cdecl m_slot() { return g_slot; }
__declspec(dllexport) int __cdecl m_rev() {
#ifdef HOOKMOD_ABI1
    return 0;
#else
    return UIHOOK_REV;
#endif
}
__declspec(dllexport) int __cdecl m_state() {
#ifdef HOOKMOD_ABI1
    return -1;
#else
    return uihook_state(g_bus);
#endif
}
__declspec(dllexport) char const* __cdecl m_layout() {
#ifdef HOOKMOD_ABI1
    g_layout[0] = 0;
#else
    uihook_layout_text(g_layout, sizeof(g_layout));
#endif
    return g_layout;
}
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
#ifndef HOOKMOD_NO_DETACH
        uihook_unregister(g_bus, g_slot, false);
        g_slot = -1;
#endif
    }
    return TRUE;
}
