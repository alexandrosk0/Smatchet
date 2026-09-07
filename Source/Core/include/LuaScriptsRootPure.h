#pragma once

// LuaScriptsRootPure — the pure decision behind "which directory do Lua script basenames
// resolve against?".
//
// Two inputs, because the answer has to exist BEFORE AppController::Initialize latches it
// (issue #2144): plugin `OnEarlyInit` runs first, and resolving off the not-yet-assigned member
// alone made every early-init lookup fail closed for the rest of the session — the Lua editor
// opened blank and the script picker listed no on-disk script, while the app's own probe a few
// log lines later confirmed the files were there all along. Hosts publish the runtime asset
// directory during bootstrap, ahead of `OnEarlyInit`, so the root is derivable at that point.
//
// What this must NOT do is fall back to a cwd-relative "Scripts/": launching Smatchet from an
// untrusted working directory would then surface — and resolve for execution — whatever .lua
// files that directory happened to carry. With no configured root the answer is empty, and every
// caller fails closed on it.
//
// No file I/O and no config access here, so the decision is unit-testable off-target. The
// AppController owns reading `ConfigManager::GetRuntimeAssetDirectory()` and the latching.

#include <string>

namespace smatchet {
namespace lua_scripts {

/// Resolve the scripts root.
/// @param latchedScriptsDirectory  AppController's `luaScriptsDirectory_` — authoritative once
///                                 `Initialize` has assigned it, empty before that.
/// @param runtimeAssetDirectory    `ConfigManager::GetRuntimeAssetDirectory()` (trailing slash
///                                 already normalised in), empty when no asset directory is
///                                 configured.
/// @return The latched value when it is set; otherwise the asset directory joined with
///         `Scripts/`; otherwise empty — never a cwd-relative path.
inline std::string ResolveScriptsRoot(const std::string& latchedScriptsDirectory,
                                      const std::string& runtimeAssetDirectory) {
    if (!latchedScriptsDirectory.empty()) {
        return latchedScriptsDirectory;
    }
    if (runtimeAssetDirectory.empty()) {
        return std::string();
    }
    // Folder name + trailing separator; the asset directory already carries its own.
    return runtimeAssetDirectory + "Scripts/";
}

} // namespace lua_scripts
} // namespace smatchet
