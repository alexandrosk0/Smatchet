#pragma once

namespace FieldEditAuditSource {

/** Grid, ticket editor, blame UI — default when no override is active. */
constexpr const char kUi[] = "ui";
/** Model Context Protocol (`run_lua`, registered MCP tools). */
constexpr const char kMcp[] = "mcp";
/** In-process Lua: automation batch, hooks setup/reload, console actions, custom windows. */
constexpr const char kLua[] = "lua";

/** Thread-local initiator for `field_edit_diff` audit rows (default `kUi`). */
const char* Current();

/** Push `source` (`kUi` / `kMcp` / `kLua`) for this scope; nested scopes stack. */
struct ScopedOverride {
    explicit ScopedOverride(const char* source);
    ~ScopedOverride();
    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;
};

} // namespace FieldEditAuditSource
