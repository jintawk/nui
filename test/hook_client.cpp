// hook_client.cpp - a second module on the UiHook bus, for hook_test.exe (module B).
// Built from the same UiHook.h, so it has its own dispatcher and its own cached bus pointer.
#include "../src/UiHook/UiHook.h"

namespace {
UiBus* g_bus = nullptr;
int g_slot = -1;
}  // namespace

extern "C" {
__declspec(dllexport) UiBus* __cdecl client_attach() {
    g_bus = uihook_attach();
    return g_bus;
}
__declspec(dllexport) int __cdecl client_ensure() { return uihook_ensure_hook(g_bus) ? 1 : 0; }
__declspec(dllexport) int __cdecl client_register(uihook_draw_fn draw, void* user) {
    g_slot = uihook_register(g_bus, draw, user);
    return g_slot;
}
__declspec(dllexport) void __cdecl client_unregister() {
    uihook_unregister(g_bus, g_slot, true);
    g_slot = -1;
}
__declspec(dllexport) void* __cdecl client_dispatcher() { return reinterpret_cast<void*>(&uihook_detail::dispatch); }
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        uihook_unregister(g_bus, g_slot, false);
        g_slot = -1;
    }
    return TRUE;
}
