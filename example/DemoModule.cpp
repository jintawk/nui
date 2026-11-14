// DemoModule.cpp - the entire native side of an addon built on nui.
//
// Everything else (the hook, the renderer, the shared texture cache, the Lua API) comes from the
// library compiled in beside this file. Build it with build.sh; the result is one self-contained
// 32-bit DLL you drop in your addon's libs/ folder.
#include <windows.h>

#include "host.hpp"

namespace {
// "name" is the client name UiHook and the texture cache report in status() (15 characters at most),
// "version" is what nui.version() returns.
nui::HostConfig const kConfig {"nuidemo", "1.0.0"};
}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // host_detach() touches no Direct3D, takes no lock and waits on nothing, so it is safe here.
    if (reason == DLL_PROCESS_DETACH) nui::host_detach();
    return TRUE;
}

extern "C" __declspec(dllexport) int __cdecl luaopen__nuidemo(lua_State* L) {
    return nui::host_open(L, kConfig);
}
