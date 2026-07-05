#include "Commands/CommandRegistry.h"

#include "Commands/FuzzyMatch.h"
#include "Commands/ParamBoundsPure.h"
#include "ConfigManager.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"

#include <cstdio>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace cmd {

CommandRegistry::CommandRegistry() = default;
CommandRegistry::~CommandRegistry() = default;

void CommandRegistry::Register(Command cmd) {
    if (cmd.Name.empty() || !cmd.Handler) {
        throw std::runtime_error("CommandRegistry::Register requires Name + Handler");
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (byName_.find(cmd.Name) != byName_.end()) {
        throw std::runtime_error("CommandRegistry: duplicate command name '" + cmd.Name + "'");
    }
    // Aliases — first writer wins; later collisions log + skip rather than throw so
    // that legitimate command registration isn't blocked by a stale alias.
    for (const std::string& a : cmd.Aliases) {
        if (a.empty() || a == cmd.Name)
            continue;
        auto inserted = aliasToName_.emplace(a, cmd.Name);
        if (!inserted.second) {
            LOG_WARN("CommandRegistry: alias '%s' already maps to '%s' (ignoring re-bind from '%s')", a.c_str(),
                     inserted.first->second.c_str(), cmd.Name.c_str());
        }
    }
    byName_.emplace(cmd.Name, std::move(cmd));
}

bool CommandRegistry::HasExact(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return byName_.find(name) != byName_.end();
}

bool CommandRegistry::Contains(const std::string& name) const {
    // Same lookup FindLocked performs (canonical name, else a registered alias),
    // returned as a bool under the lock so no pointer escapes to a caller that is
    // not holding the registry mutex.
    std::lock_guard<std::mutex> lk(mutex_);
    if (byName_.find(name) != byName_.end())
        return true;
    auto a = aliasToName_.find(name);
    return a != aliasToName_.end() && byName_.find(a->second) != byName_.end();
}

const Command* CommandRegistry::FindLocked(const std::string& name) const {
    // Caller must serialize externally if they want stable pointers.
    auto it = byName_.find(name);
    if (it != byName_.end())
        return &it->second;
    auto a = aliasToName_.find(name);
    if (a != aliasToName_.end()) {
        auto it2 = byName_.find(a->second);
        if (it2 != byName_.end())
            return &it2->second;
    }
    return nullptr;
}

std::vector<Command> CommandRegistry::All() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Command> out;
    out.reserve(byName_.size());
    for (const auto& kv : byName_) {
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const Command& a, const Command& b) { return a.Name < b.Name; });
    return out;
}

std::vector<Command> CommandRegistry::ByCategory(const std::string& category) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<Command> out;
    for (const auto& kv : byName_) {
        if (kv.second.Category == category) {
            out.push_back(kv.second);
        }
    }
    std::sort(out.begin(), out.end(), [](const Command& a, const Command& b) { return a.Name < b.Name; });
    return out;
}

std::vector<std::string> CommandRegistry::FuzzyMatch(const std::string& query, size_t limit) const {
    std::vector<std::pair<int, std::string>> scored;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        scored.reserve(byName_.size());
        for (const auto& kv : byName_) {
            const int s = FuzzyScore(query, kv.first);
            if (s > 0) {
                scored.emplace_back(s, kv.first);
            }
        }
    }
    std::sort(scored.begin(), scored.end(),
              [](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) {
                  if (a.first != b.first)
                      return a.first > b.first;
                  return a.second < b.second;
              });
    std::vector<std::string> out;
    out.reserve(std::min(scored.size(), limit));
    for (size_t i = 0; i < scored.size() && i < limit; ++i) {
        out.push_back(scored[i].second);
    }
    return out;
}

// --- Param validation / coercion ---------------------------------------------

namespace {

bool CoerceJsonValue(const nlohmann::json& in, ParamType type, nlohmann::json& out, std::string& outErr) {
    switch (type) {
    case ParamType::String: {
        if (in.is_string()) {
            out = in;
            return true;
        }
        if (in.is_number_integer() || in.is_number_float() || in.is_boolean()) {
            out = in.dump();
            return true;
        }
        outErr = "expected string";
        return false;
    }
    case ParamType::Int: {
        if (in.is_number_integer()) {
            out = in;
            return true;
        }
        if (in.is_string()) {
            try {
                long long v = std::stoll(in.get<std::string>());
                out = v;
                return true;
            } catch (...) { // catch-all-ok: stoi on user command arg
                outErr = "expected integer (parse failed)";
                return false;
            }
        }
        if (in.is_number_float()) {
            out = static_cast<long long>(in.get<double>());
            return true;
        }
        outErr = "expected integer";
        return false;
    }
    case ParamType::Number: {
        if (in.is_number()) {
            out = in;
            return true;
        }
        if (in.is_string()) {
            try {
                double v = std::stod(in.get<std::string>());
                out = v;
                return true;
            } catch (...) { // catch-all-ok: stod on user command arg
                outErr = "expected number (parse failed)";
                return false;
            }
        }
        outErr = "expected number";
        return false;
    }
    case ParamType::Bool: {
        if (in.is_boolean()) {
            out = in;
            return true;
        }
        if (in.is_string()) {
            const std::string s = in.get<std::string>();
            if (s == "true" || s == "1" || s == "yes" || s == "y") {
                out = true;
                return true;
            }
            if (s == "false" || s == "0" || s == "no" || s == "n") {
                out = false;
                return true;
            }
            outErr = "expected bool (true/false/1/0/yes/no)";
            return false;
        }
        if (in.is_number_integer()) {
            out = (in.get<long long>() != 0);
            return true;
        }
        outErr = "expected bool";
        return false;
    }
    case ParamType::Json: {
        if (in.is_string()) {
            // Bounded SAX parse: a string-typed Json arg is untrusted — it reaches here from
            // CLI, MCP, Lua, or the palette. The recursive json::parse stack-overflows on a
            // deeply-nested string before any try/catch can fire; ParseBounded caps depth,
            // nodes, and bytes, and never throws.
            std::string perr;
            nlohmann::json parsed = json_safe::ParseBounded(in.get<std::string>(), perr);
            if (!perr.empty()) {
                outErr = std::string("expected JSON (") + perr + ")";
                return false;
            }
            out = std::move(parsed);
            return true;
        }
        out = in;
        return true;
    }
    }
    outErr = "unknown param type";
    return false;
}

// Param validation plus coercion plus defaults. On failure it fills the failure result and
// returns false. On success it fills the resolved-args object and returns true.
bool ValidateAndResolveArgs(const Command& snapshot, const nlohmann::json& args, nlohmann::json& argsResolved,
                            CommandResult& outFailure) {
    argsResolved = (args.is_object() ? args : nlohmann::json::object());
    for (const ParamSpec& p : snapshot.Params) {
        if (!argsResolved.contains(p.Name) || argsResolved[p.Name].is_null()) {
            if (p.Required) {
                CommandResult r =
                    CommandResult::Failure(ErrorCode::MissingRequiredArg,
                                           "Missing required argument '" + p.Name + "' for '" + snapshot.Name + "'.",
                                           "Pass --" + p.Name + "=<value>.");
                r.Error.Details = std::make_shared<nlohmann::json>(nlohmann::json{{"param", p.Name}});
                outFailure = std::move(r);
                return false;
            }
            if (p.Default && !p.Default->is_null()) {
                argsResolved[p.Name] = *p.Default;
            }
            continue;
        }
        nlohmann::json coerced;
        std::string err;
        if (!CoerceJsonValue(argsResolved[p.Name], p.Type, coerced, err)) {
            CommandResult r = CommandResult::Failure(ErrorCode::ValidationError, "Argument '" + p.Name + "' for '" +
                                                                                     snapshot.Name + "': " + err + ".");
            r.Error.Details = std::make_shared<nlohmann::json>(nlohmann::json{{"param", p.Name}, {"reason", err}});
            outFailure = std::move(r);
            return false;
        }
        if (!p.Enum.empty() && coerced.is_string()) {
            const std::string s = coerced.get<std::string>();
            bool ok = false;
            for (const std::string& e : p.Enum) {
                if (e == s) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                CommandResult r = CommandResult::Failure(
                    ErrorCode::ValidationError, "Argument '" + p.Name + "' must be one of the allowed enum values.");
                r.Error.Details = std::make_shared<nlohmann::json>(nlohmann::json{{"param", p.Name}, {"allowed", p.Enum}});
                outFailure = std::move(r);
                return false;
            }
        }
        std::string boundsErr;
        nlohmann::json boundsDetail = nlohmann::json::object();
        if (!ParamValueWithinBounds(p, coerced, boundsErr, boundsDetail)) {
            CommandResult r = CommandResult::Failure(
                ErrorCode::ValidationError, "Argument '" + p.Name + "' for '" + snapshot.Name + "' " + boundsErr + ".");
            boundsDetail["param"] = p.Name;
            r.Error.Details = std::make_shared<nlohmann::json>(std::move(boundsDetail));
            outFailure = std::move(r);
            return false;
        }
        argsResolved[p.Name] = std::move(coerced);
    }
    return true;
}

} // namespace

CommandResult CommandRegistry::Dispatch(const std::string& name, const nlohmann::json& args,
                                        const CommandContext& ctx) {
    // (1) resolve + copy under lock so handler reentrancy is safe.
    Command snapshot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const Command* c = FindLocked(name);
        if (c) {
            snapshot = *c;
            found = true;
        }
    }

    // (2) unknown-command path — populate fuzzy suggestions.
    if (!found) {
        std::vector<std::string> suggestions = FuzzyMatch(name, 3);
        CommandResult r = CommandResult::Failure(ErrorCode::UnknownCommand, "No command named '" + name + "'.",
                                                 suggestions.empty() ? std::string()
                                                                     : ("Did you mean '" + suggestions.front() + "'?"),
                                                 std::move(suggestions));
        return r;
    }

    // (3) param validation + coercion + defaults.
    nlohmann::json argsResolved;
    {
        CommandResult validationFailure;
        if (!ValidateAndResolveArgs(snapshot, args, argsResolved, validationFailure)) {
            return validationFailure;
        }
    }

    // (4) destructive guard (dry-run bypasses). Source-aware: the gate is uniform
    // (no source bypasses confirm — see RequiresExplicitConfirm), but a destructive
    // command from a non-UI automation source (CLI/MCP/Lua) is audit-logged whether
    // it is blocked or proceeds, so token/handle-driven destructive automation is
    // traceable (security audit 2026-06-13 #2/#3, Pillar-3/observability).
    if (snapshot.Destructive && IsAutomationSource(ctx.Source)) {
        LOG_WARN("CommandRegistry: destructive command '%s' from automation source=%s confirmed=%d dryRun=%d",
                 snapshot.Name.c_str(), CommandSourceString(ctx.Source), ctx.ConfirmedDestructive ? 1 : 0,
                 ctx.DryRun ? 1 : 0);
    }
    if (RequiresExplicitConfirm(ctx.Source, snapshot.Destructive, ctx.ConfirmedDestructive, ctx.DryRun)) {
        CommandResult r =
            CommandResult::Failure(ErrorCode::ConfirmRequired,
                                   "Command '" + snapshot.Name + "' is destructive and requires explicit confirmation.",
                                   "Re-run with --yes (or --dry-run to preview).");
        return r;
    }

    // (5) dry-run-unsupported guard.
    if (ctx.DryRun && !snapshot.DryRunSupported) {
        CommandResult r = CommandResult::Failure(
            ErrorCode::DryRunUnsupported, "Command '" + snapshot.Name + "' does not support --dry-run.",
            snapshot.Destructive ? std::string() : std::string("Read-only commands have nothing to preview."));
        return r;
    }

    // (6) invoke (lock released).
    CommandResult r;
    try {
        r = snapshot.Handler(argsResolved, ctx);
    } catch (const std::exception& e) {
        r = CommandResult::Failure(ErrorCode::HandlerError,
                                   std::string("Handler for '") + snapshot.Name + "' threw: " + e.what());
    } catch (...) {
        LOG_ERROR("CommandRegistry::Dispatch: handler '%s' threw unknown exception", snapshot.Name.c_str());
        r = CommandResult::Failure(ErrorCode::HandlerError,
                                   std::string("Handler for '") + snapshot.Name + "' threw (unknown).");
    }

    // Successful dispatch is worth recording — failed dispatches aren't (don't pollute recents on typo).
    if (r.Ok) {
        RecordRecent(snapshot.Name);
    }
    return r;
}

void CommandRegistry::RecordRecent(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = recents_.begin(); it != recents_.end(); ++it) {
        if (*it == name) {
            recents_.erase(it);
            break;
        }
    }
    recents_.push_front(name);
    while (recents_.size() > kRecentsMax) {
        recents_.pop_back();
    }
}

std::vector<std::string> CommandRegistry::Recents(size_t limit) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> out;
    out.reserve(std::min(recents_.size(), limit));
    for (const std::string& n : recents_) {
        if (out.size() >= limit)
            break;
        out.push_back(n);
    }
    return out;
}

static std::string RecentsPath() { return ConfigManager::GetUserDataDirectory() + "cmd_recents.json"; }

void CommandRegistry::SaveRecents() const {
    std::vector<std::string> snap;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        snap.assign(recents_.begin(), recents_.end());
    }
    const std::string path = RecentsPath();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        LOG_WARN("CommandRegistry::SaveRecents: cannot open %s", path.c_str());
        return;
    }
    nlohmann::json j = snap;
    const std::string s = j.dump();
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
}

void CommandRegistry::LoadRecents() {
    const std::string path = RecentsPath();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return;
    // SMATCHET_DEVIATION(rule=duplication; reason=pre-existing boilerplate / include-block clone surfaced by the ParseBounded security sweep touching this file; de-duping independent subsystems is DRY-CRITICAL; owner=security-audit; revisit=2026-09-30)
    std::string content;
    char buf[512];
    size_t n;
    // The recents list is a short array of strings (capped at kRecentsMax). Bound
    // the read at 64 KiB so a corrupt/hostile file can't balloon the heap before
    // the bounded parse — well above any legitimate recents document.
    constexpr size_t kRecentsReadCap = 64 * 1024;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
        if (content.size() > kRecentsReadCap)
            break;
    }
    std::fclose(f);
    try {
        std::string parseErr;
        const nlohmann::json j = json_safe::ParseBounded(content, parseErr);
        if (!parseErr.empty()) {
            LOG_DEBUG("CommandRegistry: recents JSON parse failed; starting empty");
            return;
        }
        if (!j.is_array())
            return;
        std::lock_guard<std::mutex> lk(mutex_);
        recents_.clear();
        for (const auto& item : j) {
            if (item.is_string() && recents_.size() < kRecentsMax) {
                recents_.push_back(item.get<std::string>());
            }
        }
    } catch (...) {
        LOG_DEBUG("CommandRegistry: recents JSON parse failed; starting empty");
    }
}

} // namespace cmd
} // namespace smatchet
