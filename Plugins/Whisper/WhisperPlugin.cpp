// WhisperPlugin — push-to-talk dictation plugin shell. See header for design
// pointers. Phase A registers exactly one CLI command (`whisper.status`); the
// audio-capture / transcription / hotkey machinery lands in later phases.

#include "WhisperPlugin.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <ghc/filesystem.hpp>

#include <string>

namespace {

// Resolve the on-disk model path. Phase A only reads the path; no download is
// performed (model fetch lands in Phase C). Returns false when the platform
// shared user-data dir is unavailable.
bool ResolveLocalModelPresent(const TrackerConfig& cfg) {
    const std::string sharedDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    if (sharedDir.empty()) {
        return false;
    }
    const std::string modelName = cfg.WhisperModel.empty() ? std::string("ggml-base.en") : cfg.WhisperModel;
    const std::string modelPath = sharedDir + "/whisper/" + modelName + ".bin";
    std::error_code ec;
    return ghc::filesystem::exists(modelPath, ec);
}

smatchet::cmd::Command BuildStatusCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::CommandContext;

    Command c;
    c.Name = "whisper.status";
    c.Category = "whisper";
    c.Summary = "Report the Whisper dictation plugin's runtime status (no audio access).";
    c.Description = "Returns a JSON blob with: `enabled` (bool — runtime opt-in flag from "
                    "ConfigManager.WhisperEnabled), `mode` (string — auto|local|cloud), "
                    "`model_present` (bool — whether the configured local model file exists on "
                    "disk), and `setup_completed` (bool — whether the user has seen and answered "
                    "the first-run setup dialog). Phase A: read-only, no side effects.";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) -> CommandResult {
        const TrackerConfig cfg = ConfigManager::Load();
        nlohmann::json out;
        out["enabled"] = cfg.WhisperEnabled;
        out["mode"] = cfg.WhisperMode.empty() ? std::string("auto") : cfg.WhisperMode;
        out["model_present"] = ResolveLocalModelPresent(cfg);
        out["setup_completed"] = cfg.WhisperSetupCompleted;
        return CommandResult::Success(std::move(out));
    };
    return c;
}

} // namespace

WhisperPlugin::WhisperPlugin() = default;
WhisperPlugin::~WhisperPlugin() = default;

void WhisperPlugin::OnStart(AppController& app) {
    smatchet::cmd::CommandRegistry& reg = app.Commands();
    reg.Register(BuildStatusCommand());
    LOG_INFO("WhisperPlugin: Phase A shell started; whisper.status registered.");
}

void WhisperPlugin::OnStop() {
    LOG_INFO("WhisperPlugin: stopped.");
}
