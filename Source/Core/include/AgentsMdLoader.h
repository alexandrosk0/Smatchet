#ifndef SMATCHET_AGENTS_MD_LOADER_H
#define SMATCHET_AGENTS_MD_LOADER_H

#include <cstddef>
#include <string>

/// Layered `agents.md` loader for the Smatchet Assistant system prompt.
/// Layering (plan § agents.md loader):
///   1. **Global** layer — `globalPath` argument (typically `cfg.AgentsMdGlobalPath`,
///      default-resolved to `%LOCALAPPDATA%/Smatchet/agents.md` at Load time).
///   2. **Project** layer — `projectPathOverride` if non-empty; otherwise walk-up
///      discovery from the current working directory (or `startDir` in the helper)
///      looking for `agents.md` or `AGENTS.md`, capped at 16 levels.
/// Cap rules:
///   * Each layer raw byte cap = **64 KB**.
///   * Over-cap: truncate at 64 KB, append `\n\n---\n[truncated at 64 KB — see <abs-path>]\n`.
///   * Missing layer files: silently skip (agents.md is optional infrastructure — no
///     LOG_ERROR / LOG_WARN; an empty merged result is a normal "no agents.md present"
///     state).
///   * Separator between layers in the merged output is `\n\n---\n\n`.
/// Threading: all methods are pure functions; safe to call from any thread. The
/// caller decides where to invoke (Phase C wires it on the UI thread during
/// `AiAssistantController::Submit` — file I/O is < 10 ms at the 64 KB-per-layer
/// cap, comfortably inside pillar 2's 100 ms gate).
namespace AgentsMdLoader {

/// Default per-layer raw-bytes cap (64 KB). Public so tests can mutation-cap.
constexpr std::size_t kDefaultLayerCapBytes = 64u * 1024u;

/// Default walk-up depth cap for project agents.md discovery.
constexpr std::size_t kDefaultWalkUpMaxDepth = 16u;

/// Load + cap one layer. Empty `path` or missing file returns empty string.
/// Over-cap input is truncated to `capBytes` and gets a sentinel suffix referencing
/// `path`. UTF-8 boundaries are not preserved — callers receive raw bytes.
std::string LoadOneCapped(const std::string& path, std::size_t capBytes = kDefaultLayerCapBytes);

/// Walk up from `startDir` looking for `agents.md` / `AGENTS.md` / `.agents.md`.
/// Lowercase preferred when multiple variants exist in the same directory.
/// Returns absolute path of first match or empty string if depth cap reached.
std::string FindProjectAgentsMd(const std::string& startDir, std::size_t maxDepth = kDefaultWalkUpMaxDepth);

/// Compose the merged system-prompt prefix from global + project layers.
/// `projectPathOverride` non-empty: that exact path is used as the project layer.
/// `projectPathOverride` empty AND `autoDiscoverProject` true: walk up from the
/// process's current working directory to discover a project agents.md.
/// `projectPathOverride` empty AND `autoDiscoverProject` false (default): the
/// project layer is skipped entirely. Auto-discovery defaults OFF because the
/// cwd is rarely the user's project root and they often see surprising injections
/// from a parent repo's `AGENTS.md` (e.g. Smatchet's own dev-rules file when
/// running from inside the Smatchet source tree).
/// Result is `<global>\n\n---\n\n<project>\n` when both layers contribute, or just
/// the non-empty layer (no separator) when only one layer contributes, or empty
/// string when neither layer has content.
std::string LoadLayered(const std::string& globalPath, const std::string& projectPathOverride,
                        bool autoDiscoverProject = false);

} // namespace AgentsMdLoader

#endif
