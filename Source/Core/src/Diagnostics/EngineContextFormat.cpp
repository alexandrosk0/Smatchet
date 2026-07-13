// EngineContextFormat — see EngineContextFormat.h. Pure formatter with no I/O and
// no globals. Every helper tolerates missing/mistyped snapshot keys by skipping the field.

#include "Diagnostics/EngineContextFormat.h"

#include "ConfigManager.h"

#include <cstddef>

namespace smatchet {
namespace hostctx {

namespace {

const std::size_t kMaxSelectedActors = 25;
const int kMinLogTailLines = 1;
const int kMaxLogTailLines = 300;

std::string StrField(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

void AppendBullet(std::string& out, const char* label, const std::string& value) {
    if (value.empty()) {
        return;
    }
    out += "- ";
    out += label;
    out += ": ";
    out += value;
    out += "\n";
}

// "- Engine: Unreal Engine 5.4.2-..." from `engineVersion`.
void AppendEngineVersion(std::string& out, const nlohmann::json& snapshot) {
    AppendBullet(out, "Engine", StrField(snapshot, "engineVersion"));
}

// "- Project: MyGame (`D:/Proj/MyGame/`)" from `projectName` + `projectDir`.
void AppendProject(std::string& out, const nlohmann::json& snapshot) {
    std::string value = StrField(snapshot, "projectName");
    const std::string dir = StrField(snapshot, "projectDir");
    if (!dir.empty()) {
        value += value.empty() ? ("`" + dir + "`") : (" (`" + dir + "`)");
    }
    AppendBullet(out, "Project", value);
}

// "- Platform: Windows (Windows 11 …) — Development build" from platform/osVersion/buildConfig.
void AppendPlatform(std::string& out, const nlohmann::json& snapshot) {
    std::string value = StrField(snapshot, "platform");
    const std::string os = StrField(snapshot, "osVersion");
    if (!os.empty()) {
        value += value.empty() ? os : (" (" + os + ")");
    }
    const std::string buildConfig = StrField(snapshot, "buildConfig");
    if (!buildConfig.empty()) {
        if (!value.empty()) {
            value += " — ";
        }
        value += buildConfig + " build";
    }
    AppendBullet(out, "Platform", value);
}

// "- PIE: active (simulating)" / "- PIE: not running" from the `pie` object.
void AppendPieState(std::string& out, const nlohmann::json& snapshot) {
    const auto it = snapshot.find("pie");
    if (it == snapshot.end() || !it->is_object()) {
        return;
    }
    const bool active = it->value("active", false);
    std::string value = active ? "active" : "not running";
    if (active && it->value("simulating", false)) {
        value += " (simulating)";
    }
    AppendBullet(out, "PIE", value);
}

// "- Selected actors (3):" + one indented bullet per actor, capped at kMaxSelectedActors.
void AppendSelectedActors(std::string& out, const nlohmann::json& snapshot) {
    const auto it = snapshot.find("selectedActors");
    if (it == snapshot.end() || !it->is_array() || it->empty()) {
        return;
    }
    out += "- Selected actors (" + std::to_string(it->size()) + "):\n";
    std::size_t emitted = 0;
    for (const auto& actor : *it) {
        if (emitted >= kMaxSelectedActors) {
            out += "  - …and " + std::to_string(it->size() - emitted) + " more\n";
            break;
        }
        std::string label;
        std::string cls;
        if (actor.is_object()) {
            label = actor.value("label", std::string());
            cls = actor.value("class", std::string());
        } else if (actor.is_string()) {
            label = actor.get<std::string>();
        }
        if (label.empty() && cls.empty()) {
            continue;
        }
        out += "  - " + (label.empty() ? cls : label);
        if (!label.empty() && !cls.empty()) {
            out += " (" + cls + ")";
        }
        out += "\n";
        ++emitted;
    }
}

// "#### Output Log (last N lines)" + fenced block of the trailing N `logTail` entries.
void AppendLogTail(std::string& out, const nlohmann::json& snapshot, int maxLines) {
    const auto it = snapshot.find("logTail");
    if (it == snapshot.end() || !it->is_array() || it->empty()) {
        return;
    }
    if (maxLines < kMinLogTailLines) {
        maxLines = kMinLogTailLines;
    }
    if (maxLines > kMaxLogTailLines) {
        maxLines = kMaxLogTailLines;
    }
    const std::size_t total = it->size();
    const std::size_t take = (total > static_cast<std::size_t>(maxLines)) ? static_cast<std::size_t>(maxLines) : total;
    const std::size_t start = total - take;

    if (!out.empty()) {
        out += "\n";
    }
    out += "#### Output Log (last " + std::to_string(take) + " lines)\n```\n";
    for (std::size_t i = start; i < total; ++i) {
        const auto& line = (*it)[i];
        if (line.is_string()) {
            out += line.get<std::string>();
        }
        out += "\n";
    }
    out += "```\n";
}

} // namespace

EngineContextToggles TogglesFromConfig(const TrackerConfig& cfg) {
    EngineContextToggles t;
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=member-copy assignment run token-matches an unrelated OfflineQueueUi block, and the seven toggle copies are this function's whole purpose; owner=quick-create-issue-unreal-context; revisit=2026-12-31)
    t.EngineVersion = cfg.QuickCreateCtxEngineVersion;
    // clang-format on
    t.Project = cfg.QuickCreateCtxProject;
    t.Platform = cfg.QuickCreateCtxPlatform;
    t.Level = cfg.QuickCreateCtxLevel;
    t.PieState = cfg.QuickCreateCtxPieState;
    t.SelectedActors = cfg.QuickCreateCtxSelectedActors;
    t.LogTail = cfg.QuickCreateCtxLogTail;
    t.LogTailLines = cfg.QuickCreateCtxLogLines;
    return t;
}

std::string BuildEngineContextMarkdown(const nlohmann::json& snapshot, const EngineContextToggles& toggles) {
    if (!snapshot.is_object() || snapshot.empty()) {
        return std::string();
    }
    std::string body;
    if (toggles.EngineVersion) {
        AppendEngineVersion(body, snapshot);
    }
    if (toggles.Project) {
        AppendProject(body, snapshot);
    }
    if (toggles.Platform) {
        AppendPlatform(body, snapshot);
    }
    if (toggles.Level) {
        AppendBullet(body, "Map", StrField(snapshot, "mapName"));
    }
    if (toggles.PieState) {
        AppendPieState(body, snapshot);
    }
    if (toggles.SelectedActors) {
        AppendSelectedActors(body, snapshot);
    }
    if (toggles.LogTail) {
        AppendLogTail(body, snapshot, toggles.LogTailLines);
    }
    if (body.empty()) {
        return std::string();
    }
    return "### Engine context\n" + body;
}

} // namespace hostctx
} // namespace smatchet
