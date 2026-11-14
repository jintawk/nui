// hook_harness.hpp - calls FFXiMain's (patched or not) EndScene wrapper the way the game does, with a
// fake renderer whose fake device's vtable slot 0x8C is a recording EndScene stub, and captures every
// register the caller can see. Shared by hook_test.cpp and hook_matrix.cpp; include from one
// translation unit per executable (it defines the asm symbols).
#ifndef HOOK_HARNESS_HPP
#define HOOK_HARNESS_HPP

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
// harness state (read/written by the asm below)
volatile std::uint32_t g_fn, g_ecx_in;
volatile std::uint32_t g_esp_before, g_esp_after;
volatile std::uint32_t g_r_eax, g_r_ecx, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp;
// recording EndScene stub state
volatile std::uint32_t g_seq;
volatile std::uint32_t g_es_calls, g_es_this, g_es_seq;
void __cdecl call_wrapper();
long __stdcall fake_end_scene(void* self);
}

// call_wrapper: ecx = g_ecx_in, callee-saved registers and edx set to markers, call [g_fn],
// record every register and esp after it returns.
// fake_end_scene: __stdcall(this); records this, call count and order; returns 0x1234ABCD;
// touches neither ecx nor edx, so a baseline and a hooked call are register-for-register equal.
asm(R"(
    .intel_syntax noprefix
    .text
    .globl _call_wrapper
_call_wrapper:
    push ebp
    push ebx
    push esi
    push edi
    mov ecx, dword ptr [_g_ecx_in]
    mov ebx, 0x11111111
    mov esi, 0x22222222
    mov edi, 0x33333333
    mov ebp, 0x44444444
    mov edx, 0x55555555
    mov dword ptr [_g_esp_before], esp
    call dword ptr [_g_fn]
    mov dword ptr [_g_esp_after], esp
    mov dword ptr [_g_r_eax], eax
    mov dword ptr [_g_r_ecx], ecx
    mov dword ptr [_g_r_edx], edx
    mov dword ptr [_g_r_ebx], ebx
    mov dword ptr [_g_r_esi], esi
    mov dword ptr [_g_r_edi], edi
    mov dword ptr [_g_r_ebp], ebp
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

    .globl _fake_end_scene@4
_fake_end_scene@4:
    mov eax, dword ptr [esp+4]
    mov dword ptr [_g_es_this], eax
    inc dword ptr [_g_es_calls]
    mov eax, dword ptr [_g_seq]
    mov dword ptr [_g_es_seq], eax
    inc dword ptr [_g_seq]
    mov eax, 0x1234ABCD
    ret 4
    .att_syntax prefix
)");

namespace harness {

// fake renderer / device
inline volatile std::uint32_t g_wrong_slot = 0;
inline void __stdcall trap_slot(void*) { ++g_wrong_slot; }
inline void* g_vtbl[64];
struct FakeDevice { void** vtbl; };
inline FakeDevice g_device;
struct FakeRenderer { unsigned char pad[0x0C]; void* device; unsigned char rest[0x40]; };
inline FakeRenderer g_renderer;

struct Regs { std::uint32_t eax, ecx, edx, ebx, esi, edi, ebp, esp_delta; };

inline void setup() {
    for (void*& s : g_vtbl) s = reinterpret_cast<void*>(&trap_slot);
    g_vtbl[0x8C / 4] = reinterpret_cast<void*>(&fake_end_scene);
    g_device.vtbl = g_vtbl;
    g_renderer.device = &g_device;
}

// One call of the wrapper at `site` (g_seq restarts at 0; callbacks may use it for ordering).
inline Regs run(std::uintptr_t site) {
    g_fn = static_cast<std::uint32_t>(site);
    g_ecx_in = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_renderer));
    g_es_calls = 0;
    g_es_this = 0;
    g_seq = 0;
    call_wrapper();
    return {g_r_eax, g_r_ecx, g_r_edx, g_r_ebx, g_r_esi, g_r_edi, g_r_ebp, g_esp_before - g_esp_after};
}

inline bool same(Regs const& a, Regs const& b) { return std::memcmp(&a, &b, sizeof(Regs)) == 0; }

inline void print_regs(char const* tag, Regs const& r) {
    std::printf("  %-26s eax %08lx ecx %08lx edx %08lx ebx %08lx esi %08lx edi %08lx ebp %08lx esp delta %lu\n", tag,
                (unsigned long)r.eax, (unsigned long)r.ecx, (unsigned long)r.edx, (unsigned long)r.ebx,
                (unsigned long)r.esi, (unsigned long)r.edi, (unsigned long)r.ebp, (unsigned long)r.esp_delta);
}

}  // namespace harness

#endif  // HOOK_HARNESS_HPP
