// UiHook.h - shared hook on FFXiMain's EndScene wrapper, for 2D drawn ABOVE the game's own UI.
//
// Modelled on BroGuyPal's SceneHook.h, with its own mapping name and
// ABI so the two buses never meet. SceneHook draws inside draw_scene (under the UI); UiHook draws
// after the game's whole 2D pass, just before the frame's EndScene, so callbacks sit on top of the
// game's windows. Windower's own texts are drawn by its device wrapper later and stay on top.
//
// The target (client build 2026-10-13): the renderer's EndScene wrapper, __thiscall,
// ecx = renderer ([0x1045666C]), 16 bytes, unique in .text:
//
//     8B 41 0C          mov eax, [ecx+0Ch]       ; the device
//     50                push eax
//     8B 08             mov ecx, [eax]           ; its vtable
//     FF 91 8C 00 00 00 call [ecx+8Ch]           ; IDirect3DDevice8::EndScene
//     C3                ret
//     90 90 90
//
// It is found by that signature, never by address. Exactly one match is required; anything else
// (a client update, another tool's patch, an older UiHook) means no patch and a status message.
//
// The patch, written once and never undone (the first 6 bytes are three whole instructions):
//
//     wrapper:  E9 <rel32 -> thunk>  90
//
// The thunk lives in one VirtualAlloc block owned by no module and never freed:
//
//     block+0   dispatch       the owner module's dispatcher, or null
//     block+16  60             pushad
//               A1 <&dispatch> mov eax, [dispatch]
//               85 C0          test eax, eax
//               74 06          jz  done
//               51             push ecx              ; renderer
//               FF D0          call eax              ; __cdecl dispatch(renderer)
//               83 C4 04       add esp, 4
//     done:     61             popad
//               8B 41 0C 50 8B 08                    ; the 6 displaced bytes
//               E9 <rel32 -> wrapper+6>
//
// pushad/popad give the original code every register exactly as the caller left it (ecx = the
// renderer), so the game's EndScene runs once, with its own `this`, and its eax comes back
// unchanged; esp is balanced. With dispatch == null the hook is a pure pass-through. Handing the
// frame between modules is one aligned 4-byte store into `dispatch`.
//
// Each module compiles its own dispatcher (an inline function, so one copy per DLL). The owner's
// dispatcher calls every live client's callback. A module must leave in DllMain(DLL_PROCESS_DETACH)
// with uihook_unregister(bus, slot, false), which hands `dispatch` to another live module, or null,
// before its code unmaps. Every participant must build against a byte-identical copy of this file;
// the bus refuses a different ABI version.

#ifndef UIHOOK_H
#define UIHOOK_H

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define UIHOOK_ABI_VERSION 1u
#define UIHOOK_MAX_CLIENTS 32

static_assert(sizeof(void*) == 4, "UiHook targets 32-bit FFXI");

// FFXI's render thread does not guarantee a 16-byte aligned stack; GCC builds fault on SSE spills
// without realignment. Put it on draw callbacks too.
#if defined(__GNUC__)
#define UIHOOK_ALIGN_STACK __attribute__((force_align_arg_pointer))
#else
#define UIHOOK_ALIGN_STACK
#endif

// device = [renderer + 0x0C] read with a guarded read, or null
typedef void(__cdecl* uihook_draw_fn)(void* user, void* renderer, void* device);
typedef void(__cdecl* uihook_dispatch_fn)(void* renderer);

// occupied: 0 free, 1 being filled in, 2 live. Only 2 is dispatched.
struct UiClient {
    volatile LONG occupied;
    volatile LONG enabled;
    uihook_draw_fn draw;
    uihook_dispatch_fn dispatch;    // this client's module's dispatcher
    void* user;
};

// ready, abi_version and size are frozen at offsets 0, 4 and 8. Append only.
struct UiBus {
    volatile LONG ready;
    unsigned long abi_version;
    unsigned long size;
    volatile LONG install_lock;
    volatile LONG hook_installed;
    volatile LONG owner_slot;        // -1 when dispatch is null
    volatile LONG in_flight;
    volatile LONG installs;          // times the wrapper was patched in this process (0 or 1)
    volatile LONG dispatches;        // dispatcher entries (wrapper calls while someone listened)
    unsigned char* block;            // never freed
    void* volatile* dispatch_ptr;    // == block
    std::uintptr_t site;             // the patched wrapper
    UiClient clients[UIHOOK_MAX_CLIENTS];
    char status[192];
};

namespace uihook_detail {

inline UiBus*& cached_bus() { static UiBus* value = nullptr; return value; }

constexpr std::size_t kSigBytes = 16;
constexpr unsigned char kWrapperSig[kSigBytes] = {
    0x8B, 0x41, 0x0C, 0x50, 0x8B, 0x08, 0xFF, 0x91, 0x8C, 0x00, 0x00, 0x00, 0xC3, 0x90, 0x90, 0x90,
};
constexpr std::size_t kDisplaced = 6;    // mov eax,[ecx+0Ch] / push eax / mov ecx,[eax]
constexpr std::size_t kThunkAt = 16;

inline bool page_readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool span_readable(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) return false;
    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) return false;
    if (region.State != MEM_COMMIT || !page_readable(region.Protect)) return false;
    std::uintptr_t const low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address >= low && address + size <= low + region.RegionSize;
}

// Count matches of sig in [begin, end), walking contiguous runs of committed readable pages
// (a VirtualProtect inside the module splits regions; never test a whole section at once).
inline int scan_count(std::uintptr_t begin, std::uintptr_t end, unsigned char const* sig, std::size_t n,
                      std::uintptr_t* first) {
    int count = 0;
    std::uintptr_t address = begin;
    while (address < end) {
        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) break;
        std::uintptr_t const region_end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (region_end <= address) break;
        if (region.State != MEM_COMMIT || !page_readable(region.Protect)) { address = region_end; continue; }
        std::uintptr_t run_end = region_end;
        while (run_end < end) {
            MEMORY_BASIC_INFORMATION next {};
            if (!VirtualQuery(reinterpret_cast<void const*>(run_end), &next, sizeof(next))
                || next.State != MEM_COMMIT || !page_readable(next.Protect)) break;
            std::uintptr_t const next_end = reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
            if (next_end <= run_end) break;
            run_end = next_end;
        }
        std::uintptr_t const limit = run_end < end ? run_end : end;
        auto const* bytes = reinterpret_cast<unsigned char const*>(address);
        for (std::size_t off = 0; address + off + n <= limit; ++off) {
            if (bytes[off] == sig[0] && std::memcmp(bytes + off, sig, n) == 0) {
                if (count == 0 && first) *first = address + off;
                ++count;
            }
        }
        address = run_end;
    }
    return count;
}

// The wrapper's address, or 0 with *reason set. `simulate_missing` scans for the signature with
// its last byte changed, so the real not-found path runs (the in-game fail-closed test).
inline std::uintptr_t find_wrapper(bool simulate_missing, char const** reason) {
    if (reason) *reason = "FFXiMain.dll is not loaded";
    HMODULE module = GetModuleHandleA("FFXiMain.dll");
    if (!module) return 0;
    auto const base = reinterpret_cast<std::uintptr_t>(module);
    if (!span_readable(base, sizeof(IMAGE_DOS_HEADER))) return 0;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    std::uintptr_t const nt_at = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (!span_readable(nt_at, sizeof(IMAGE_NT_HEADERS32))) return 0;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(nt_at);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    unsigned char sig[kSigBytes];
    std::memcpy(sig, kWrapperSig, kSigBytes);
    if (simulate_missing) sig[kSigBytes - 1] = 0xCC;

    int count = 0;
    std::uintptr_t hit = 0;
    auto const* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
        std::uintptr_t const start = base + section->VirtualAddress;
        std::uintptr_t first = 0;
        int const c = scan_count(start, start + section->Misc.VirtualSize, sig, kSigBytes, &first);
        if (c > 0 && count == 0) hit = first;
        count += c;
    }
    if (count == 1) return hit;
    if (reason) {
        *reason = count == 0
            ? (simulate_missing ? "EndScene wrapper signature not found (simulated)"
                                : "EndScene wrapper signature not found (client update, or patched by another tool)")
            : "EndScene wrapper signature is not unique";
    }
    return 0;
}

// The dispatcher: one copy per module. Entered from the thunk on the render thread.
inline void UIHOOK_ALIGN_STACK __cdecl dispatch(void* renderer) {
    UiBus* bus = cached_bus();
    if (!bus) return;
    InterlockedIncrement(&bus->in_flight);
    InterlockedIncrement(&bus->dispatches);
    void* device = nullptr;
    std::uintptr_t const at = reinterpret_cast<std::uintptr_t>(renderer) + 0x0C;
    if (renderer && span_readable(at, 4)) std::memcpy(&device, reinterpret_cast<void const*>(at), 4);
    for (int i = 0; i < UIHOOK_MAX_CLIENTS; ++i) {
        UiClient& c = bus->clients[i];
        if (c.occupied != 2 || !c.enabled || !c.draw) continue;
        c.draw(c.user, renderer, device);      // each client restores the device state it changes
    }
    InterlockedDecrement(&bus->in_flight);
}

// Hand the frame to a live client other than `leaving`, or to nobody (pass-through).
inline void reassign_owner(UiBus* bus, int leaving) {
    if (!bus || !bus->dispatch_ptr) return;
    for (int i = 0; i < UIHOOK_MAX_CLIENTS; ++i) {
        if (i == leaving) continue;
        UiClient& c = bus->clients[i];
        if (c.occupied == 2 && c.dispatch) {
            *bus->dispatch_ptr = reinterpret_cast<void*>(c.dispatch);
            InterlockedExchange(&bus->owner_slot, i);
            return;
        }
    }
    *bus->dispatch_ptr = nullptr;
    InterlockedExchange(&bus->owner_slot, -1);
}

}  // namespace uihook_detail

// Attach to the process-wide bus, creating it on first use. Null if the mapping fails or a bus
// with another ABI is resident. The mapping and view are leaked on purpose: they belong to the
// process, so the hook outlives every module.
inline UiBus* uihook_attach() {
    UiBus*& cached = uihook_detail::cached_bus();
    if (cached) return cached;
    char name[64];
    std::snprintf(name, sizeof(name), "Local\\FFXiUiHook_v1_%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(UiBus), name);
    if (!mapping) return nullptr;
    bool const existed = GetLastError() == ERROR_ALREADY_EXISTS;
    auto* bus = static_cast<UiBus*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UiBus)));
    if (!bus) { CloseHandle(mapping); return nullptr; }
    if (!existed) {
        bus->abi_version = UIHOOK_ABI_VERSION;
        bus->size = sizeof(UiBus);
        bus->owner_slot = -1;
        std::snprintf(bus->status, sizeof(bus->status), "bus created");
        MemoryBarrier();
        InterlockedExchange(&bus->ready, 1);
    } else {
        for (int spin = 0; spin < 2000 && InterlockedCompareExchange(&bus->ready, 0, 0) == 0; ++spin) Sleep(1);
        if (InterlockedCompareExchange(&bus->ready, 0, 0) == 0) { UnmapViewOfFile(bus); CloseHandle(mapping); return nullptr; }
    }
    if (bus->abi_version != UIHOOK_ABI_VERSION || bus->size < sizeof(UiBus)) {
        UnmapViewOfFile(bus);
        CloseHandle(mapping);
        return nullptr;
    }
    cached = bus;
    return bus;
}

// Patch the wrapper if nobody has yet. Safe from every module on every load; only the first call
// works. True when the patch is live. With simulate_missing it always fails, through the real
// not-found path, and never patches.
inline bool uihook_ensure_hook(UiBus* bus, bool simulate_missing = false) {
    using namespace uihook_detail;
    if (!bus) return false;
    if (simulate_missing) {
        char const* reason = "";
        find_wrapper(true, &reason);
        std::snprintf(bus->status, sizeof(bus->status), "%s", reason);
        return false;
    }
    if (InterlockedCompareExchange(&bus->hook_installed, 0, 0) != 0) return true;
    if (InterlockedCompareExchange(&bus->install_lock, 1, 0) != 0) {
        for (int spin = 0; spin < 2000 && InterlockedCompareExchange(&bus->hook_installed, 0, 0) == 0; ++spin) Sleep(1);
        return InterlockedCompareExchange(&bus->hook_installed, 0, 0) != 0;
    }
    char const* reason = "EndScene wrapper not found";
    std::uintptr_t const site = find_wrapper(false, &reason);
    if (site == 0) {
        std::snprintf(bus->status, sizeof(bus->status), "%s", reason);
        InterlockedExchange(&bus->install_lock, 0);
        return false;
    }
    auto* block = static_cast<unsigned char*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!block) {
        std::snprintf(bus->status, sizeof(bus->status), "hook block alloc failed");
        InterlockedExchange(&bus->install_lock, 0);
        return false;
    }
    std::memset(block, 0xCC, 64);
    std::memset(block, 0, 4);                                        // dispatch = null
    unsigned char* t = block + kThunkAt;
    auto const slot = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(block));
    unsigned char const head[] = {0x60, 0xA1};
    std::memcpy(t, head, 2);
    std::memcpy(t + 2, &slot, 4);
    unsigned char const call[] = {0x85, 0xC0, 0x74, 0x06, 0x51, 0xFF, 0xD0, 0x83, 0xC4, 0x04, 0x61};
    std::memcpy(t + 6, call, sizeof(call));
    unsigned char* const tail = t + 6 + sizeof(call);
    std::memcpy(tail, reinterpret_cast<void const*>(site), kDisplaced);
    tail[kDisplaced] = 0xE9;
    std::int32_t const back = static_cast<std::int32_t>(
        (site + kDisplaced) - (reinterpret_cast<std::uintptr_t>(tail) + kDisplaced + 5));
    std::memcpy(tail + kDisplaced + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), block, 64);

    bus->block = block;
    bus->dispatch_ptr = reinterpret_cast<void* volatile*>(block);
    bus->site = site;
    MemoryBarrier();

    DWORD previous = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), kDisplaced, PAGE_EXECUTE_READWRITE, &previous)) {
        std::snprintf(bus->status, sizeof(bus->status), "VirtualProtect failed");
        bus->block = nullptr;
        bus->dispatch_ptr = nullptr;
        bus->site = 0;
        VirtualFree(block, 0, MEM_RELEASE);
        InterlockedExchange(&bus->install_lock, 0);
        return false;
    }
    auto* patch = reinterpret_cast<unsigned char*>(site);
    std::int32_t const to_thunk = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(t) - (site + 5));
    patch[0] = 0xE9;
    std::memcpy(patch + 1, &to_thunk, 4);
    patch[5] = 0x90;
    VirtualProtect(reinterpret_cast<void*>(site), kDisplaced, previous, &previous);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), kDisplaced);

    InterlockedIncrement(&bus->installs);
    std::snprintf(bus->status, sizeof(bus->status), "hook installed at %p", reinterpret_cast<void*>(site));
    InterlockedExchange(&bus->hook_installed, 1);
    InterlockedExchange(&bus->install_lock, 0);
    return true;
}

// A slot index, or -1. The first client when nobody owns the frame takes it.
inline int uihook_register(UiBus* bus, uihook_draw_fn draw, void* user) {
    if (!bus || !draw || !bus->dispatch_ptr) return -1;
    for (int i = 0; i < UIHOOK_MAX_CLIENTS; ++i) {
        if (InterlockedCompareExchange(&bus->clients[i].occupied, 1, 0) != 0) continue;
        UiClient& c = bus->clients[i];
        c.draw = draw;
        c.dispatch = &uihook_detail::dispatch;
        c.user = user;
        InterlockedExchange(&c.enabled, 1);
        MemoryBarrier();
        InterlockedExchange(&c.occupied, 2);
        if (InterlockedCompareExchange(&bus->owner_slot, 0, 0) < 0) {
            *bus->dispatch_ptr = reinterpret_cast<void*>(c.dispatch);
            InterlockedExchange(&bus->owner_slot, i);
        }
        return i;
    }
    return -1;
}

// Leave the bus. Also call from DllMain(DLL_PROCESS_DETACH) with may_wait = false (loader lock).
inline void uihook_unregister(UiBus* bus, int slot, bool may_wait) {
    if (!bus || slot < 0 || slot >= UIHOOK_MAX_CLIENTS) return;
    UiClient& c = bus->clients[slot];
    InterlockedExchange(&c.enabled, 0);
    if (InterlockedCompareExchange(&bus->owner_slot, 0, 0) == slot) uihook_detail::reassign_owner(bus, slot);
    c.draw = nullptr;
    c.dispatch = nullptr;
    c.user = nullptr;
    MemoryBarrier();
    InterlockedExchange(&c.occupied, 0);
    if (may_wait) {
        for (int spin = 0; spin < 200 && InterlockedCompareExchange(&bus->in_flight, 0, 0) != 0; ++spin) Sleep(1);
    }
}

inline void uihook_set_enabled(UiBus* bus, int slot, bool enabled) {
    if (!bus || slot < 0 || slot >= UIHOOK_MAX_CLIENTS) return;
    InterlockedExchange(&bus->clients[slot].enabled, enabled ? 1 : 0);
}

inline int uihook_active_clients(UiBus const* bus) {
    if (!bus) return 0;
    int n = 0;
    for (int i = 0; i < UIHOOK_MAX_CLIENTS; ++i)
        if (bus->clients[i].occupied == 2 && bus->clients[i].enabled) ++n;
    return n;
}

#endif  // UIHOOK_H
