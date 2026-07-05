#ifndef SMATCHET_COMMANDS_REGISTRY_H
#define SMATCHET_COMMANDS_REGISTRY_H

#include "Commands/Command.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {
namespace cmd {

/// Thread-safe central registry. One instance lives on `AppController`.
/// **Feature-gated builds** (ADR 0010): optional surfaces (AI, MCP, Whisper) register
/// commands only when their `SMATCHET_WITH_*` flag is ON. OFF builds omit those names;
/// `Dispatch` returns `unknown-command` — no stub “disabled in this build” handlers.
/// **Reentrancy contract** (see plan): `Dispatch` copies the `Command` struct
/// (including its `Handler` `std::function`) under the registry mutex, then
/// releases the lock before invoking the handler. A Lua handler that recurses
/// back into `commands.invoke` can therefore acquire the mutex again without
/// deadlock.
class CommandRegistry {
  public:
    CommandRegistry();
    ~CommandRegistry();

    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;

    /// Register a command. Throws `std::runtime_error` on duplicate name (catches
    /// programmer error early — duplicate names always indicate a registration bug).
    void Register(Command cmd);

    /// Has a command with this exact name (no alias resolution).
    bool HasExact(const std::string& name) const;

    /// Alias-aware existence check — true when a command with this name is registered
    /// (an exact name, or a registered alias resolved the same way FindLocked resolves).
    /// Computed under the registry lock, so it is safe to call from a worker thread that
    /// does not hold it. Prefer this to calling FindLocked purely to null-check its
    /// result: it never hands back a pointer a concurrent Register could invalidate.
    bool Contains(const std::string& name) const;

    /// Resolve a name (alias-aware) to the canonical command. Returns `nullptr` if
    /// not found. **The returned pointer is invalidated by any concurrent
    /// `Register` call**, so callers must either hold the registry lock or copy
    /// the data they need immediately. For a pure existence test off the lock, use
    /// `Contains` instead.
    const Command* FindLocked(const std::string& name) const;

    /// Thread-safe copy of all registered commands (alphabetical by name).
    std::vector<Command> All() const;

    /// Thread-safe copy of all commands in a given category. Empty when none.
    std::vector<Command> ByCategory(const std::string& category) const;

    /// Top-N fuzzy matches by descending score (score 0 entries excluded).
    std::vector<std::string> FuzzyMatch(const std::string& query, size_t limit = 5) const;

    /// Dispatch entry point. See plan §"Guard order inside `Dispatch`".
    /// On `unknown-command`, `Suggestions` is populated with the top 3 fuzzy
    /// matches so agents can self-correct typos.
    CommandResult Dispatch(const std::string& name, const nlohmann::json& args, const CommandContext& ctx);

    /// Push a recent-name onto the bounded ring (front). Drops duplicates by name.
    void RecordRecent(const std::string& name);

    /// Most-recent-first names (up to `limit`).
    std::vector<std::string> Recents(size_t limit = 16) const;

    /// Persist recents deque to `<userData>/cmd_recents.json`.
    void SaveRecents() const;
    /// Load recents deque from `<userData>/cmd_recents.json` (silently ignores missing file).
    void LoadRecents();

  private:
    static constexpr size_t kRecentsMax = 16;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Command> byName_;
    std::unordered_map<std::string, std::string> aliasToName_;
    std::deque<std::string> recents_;
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_REGISTRY_H
