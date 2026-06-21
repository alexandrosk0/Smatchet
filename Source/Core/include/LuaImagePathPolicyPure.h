#pragma once

// Pure allowlist policy for the Lua `imgui.image(path)` binding (LuaDrawList::Image).
//
// SECURITY (Pillar 3 / SSRF): the image replay path (SmatchetFieldIconRender::DrawImagePathOrUrl ->
// AppController::ResolveFieldIconAssetPath) passes http(s) URLs straight through to HttpGetBinary,
// which performs an unauthenticated outbound GET. A Lua-console script must NOT be able to drive an
// arbitrary outbound request (server-side request forgery, data exfil via crafted URL, or a tracking
// pixel), so the Lua binding refuses http(s) URLs and permits only non-http(s) local asset paths.
// Internal field-icon-map / priority-icon URL fetches are unaffected — they never route through this
// Lua binding.
//
// Split into a header-only predicate so it is unit-testable on the desktop test build with no Lua
// state, no sol2, and no AppController. The scheme check is whitespace-trimmed and ASCII
// case-insensitive so `  HTTP://x` / `Https://x` cannot bypass it.
namespace LuaImagePathPolicyPure {

// True iff `path` (after trimming leading ASCII whitespace) begins with a case-insensitive
// `http://` or `https://` scheme. Pure; allocation-free.
inline bool IsHttpUrl(const char* path) {
    if (path == nullptr) {
        return false;
    }
    const char* p = path;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '\f' || *p == '\v') {
        ++p;
    }
    // Lowercase-compare a literal prefix against the (possibly mixed-case) input.
    auto matchesPrefix = [](const char* in, const char* lowerPrefix) -> bool {
        for (; *lowerPrefix != '\0'; ++in, ++lowerPrefix) {
            char c = *in;
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != *lowerPrefix) {
                return false;
            }
        }
        return true;
    };
    return matchesPrefix(p, "http://") || matchesPrefix(p, "https://");
}

// True iff the path is permitted for the Lua imgui.image binding (i.e. it is not an http(s) URL).
inline bool IsAllowedLuaImagePath(const char* path) { return !IsHttpUrl(path); }

} // namespace LuaImagePathPolicyPure
