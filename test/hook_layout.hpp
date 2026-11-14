// hook_layout.hpp - UiHook 2's frozen layout as text, from the header it is compiled with. Compared
// line for line with layout_uihook.txt by hook_matrix.exe (for itself and for every test module it
// loads), so a module built with a field moved (UIHOOK_FAULT_LAYOUT_SWAP) is caught.
//
// Rule for layout_uihook.txt: existing lines never change; a new revision may only add lines for
// fields it appends into a reserved tail or after UiBus::status (and shrink the reserved size line).
#ifndef HOOK_LAYOUT_HPP
#define HOOK_LAYOUT_HPP

#include "../src/UiHook/UiHook.h"

#include <cstddef>
#include <cstdio>

// Writes the layout into out (NUL-terminated); returns its length.
inline int uihook_layout_text(char* out, int cap) {
    int n = 0;
    auto line = [&](char const* key, unsigned long value) {
        if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "%s %lu\n", key, value);
    };
#define UL(x) static_cast<unsigned long>(x)
    line("UIHOOK_ABI_VERSION", UL(UIHOOK_ABI_VERSION));
    line("UIHOOK_MAX_CLIENTS", UL(UIHOOK_MAX_CLIENTS));
    line("UIHOOK_MAP_BYTES", UL(UIHOOK_MAP_BYTES));
    if (n < cap) n += std::snprintf(out + n, static_cast<std::size_t>(cap - n), "UIHOOK_MAP_NAME_FMT %s\n", UIHOOK_MAP_NAME_FMT);
    line("sizeof(UiClient)", UL(sizeof(UiClient)));
#define C(f) line("UiClient." #f, UL(offsetof(UiClient, f)))
    C(occupied); C(enabled); C(draw); C(dispatch); C(user); C(rev); C(order); C(module); C(name); C(nonce_at); C(nonce); C(reserved);
#undef C
    line("sizeof(UiClient.name)", UL(sizeof(UiClient::name)));
    line("sizeof(UiClient.reserved)", UL(sizeof(UiClient::reserved)));
#define B(f) line("UiBus." #f, UL(offsetof(UiBus, f)))
    B(ready); B(abi_version); B(size); B(install_lock); B(hook_installed); B(owner_slot); B(in_flight);
    B(installs); B(dispatches); B(block); B(dispatch_ptr); B(site); B(owner_rev); B(reg_lock); B(handovers);
    B(reclaimed); B(blocked); B(foreign_site); B(lock_breaks); B(serial); B(reserved_hdr); B(clients); B(status);
#undef B
    line("sizeof(UiBus.reserved_hdr)", UL(sizeof(UiBus::reserved_hdr)));
    line("sizeof(UiBus.status)", UL(sizeof(UiBus::status)));
    line("UIHOOK_LIVE", UL(UIHOOK_LIVE));
    line("UIHOOK_WAIT", UL(UIHOOK_WAIT));
    line("UIHOOK_BLOCKED", UL(UIHOOK_BLOCKED));
#undef UL
    return n;
}

#endif  // HOOK_LAYOUT_HPP
