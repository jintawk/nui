// nuitest.cpp - a bare consumer of the whole nui library for test_render.exe (design E, step 5b): the
// same glue as an addon's module (ChronicleModule.cpp), so the real host, renderer and shared cache
// run exactly as they do in game, with a Lua table pushed by luaopen__nuitest.
//
// Built several times from this one file plus src (test/build_render_test.sh):
//   -DNUITEST_NAME=\"a\"     the module's UiHook / cache client name (separate DLLs a, b, c: separate
//                            addons as far as Windows, UiHook and the cache can tell)
//   -DNUICACHE_MAJOR=99      a module of another cache major (its own mapping: shares nothing)
//   -DNUI_TEST_CLOCK         the cache reads test_render's clock (an export of the exe) instead of
//                            GetTickCount, so a test can pass the 10 s grace time at once
//   -DNUI_FAULT_*            a deliberately broken library (build_render_test.sh --self-test)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "host.hpp"

#ifndef NUITEST_NAME
#define NUITEST_NAME "t"
#endif

extern "C" unsigned long __cdecl nui_test_now() {
    typedef unsigned long(__cdecl * now_fn)();
    static now_fn fn = reinterpret_cast<now_fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), "test_now")));
    return fn ? fn() : GetTickCount();
}

// 1.2.5: test_render's "ex" asks one site of the host to throw once (src/host.cpp NUI_TEST_THROW)
extern "C" int __cdecl nui_test_throw(int site) {
    typedef int(__cdecl * throw_fn)(int);
    static throw_fn fn = reinterpret_cast<throw_fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), "test_throw")));
    return fn ? fn(site) : 0;
}

// 1.2.6: test_render's "tl" makes each decode on the decode thread sleep first (src/host.cpp decode_one)
extern "C" unsigned long __cdecl nui_test_decode_ms() {
    typedef unsigned long(__cdecl * ms_fn)();
    static ms_fn fn = reinterpret_cast<ms_fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), "test_decode_ms")));
    return fn ? fn() : 0;
}

namespace {
nui::HostConfig const kConfig {NUITEST_NAME, "1.0.0-test"};
}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) nui::host_detach();
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl luaopen__nuitest(lua_State* L) {
    return nui::host_open(L, kConfig);
}
