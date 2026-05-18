// WhisperPlugin — push-to-talk dictation plugin. See header for design
// pointers. Phase A registered `whisper.status`; Phase B adds the end-to-end
// cloud transcription smoke path via `whisper.transcribe-once`. Audio capture
// + cloud transcription + result reporting all run from the CLI thread (not
// the UI thread), so a synchronous handler is allowed here. The Phase E hotkey
// path will wrap Transcribe in AppController::LaunchBackgroundTask + post the
// result back via MainThreadDispatcher::PostToMainThread (Pattern A) to keep
// Pillar 2 compliance once invocation moves to a UI-frame-reachable surface.

#include "WhisperPlugin.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "ModelCatalog.h"
#include "ModelDownloader.h"
#include "WavWriter.h"
#include "WhisperApiClient.h"
#include "WhisperApiKeyResolve.h"
#include "WhisperConsentGate.h"
#include "WhisperLocal.h"
#include "WindowsAudioCapture.h"

#include <nlohmann/json.hpp>

#include <ghc/filesystem.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

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

// Map cfg.AiProviderKind (enum-as-int) to the lowercase string the Whisper
// key fallback rule keys on ("openai" / "anthropic" / ...). Mirrors
// AiClientFactory::ProviderToString without dragging the AiClientFactory TU
// into this plugin's link set. Unknown enum values map to empty -> Whisper
// fallback rule returns no key (safe default).
std::string ProviderEnumKindToString(int kind) {
    switch (kind) {
    case 0:
        return "openai";
    case 1:
        return "anthropic";
    case 2:
        return "ollama-openai";
    case 3:
        return "ollama-native";
    default:
        return std::string();
    }
}

std::string ResolveWhisperKeyFromConfig(const TrackerConfig& cfg) {
#if defined(SMATCHET_WITH_WHISPER)
    const std::string providerStr = ProviderEnumKindToString(cfg.AiProviderKind);
    return smatchet::whisper::pure::ResolveWhisperApiKey(cfg.WhisperApiKey, providerStr, cfg.AiApiKey);
#else
    (void)cfg;
    return std::string();
#endif
}

// Read a WAV file from disk into a byte buffer. No format validation — the
// caller (WhisperApiClient) trusts the multipart content-type and Whisper's
// server-side decoder. Returns false on stat failure / read truncation.
bool ReadWavFile(const std::string& path, std::vector<std::uint8_t>& out, std::string& outError) {
    std::error_code ec;
    if (!ghc::filesystem::exists(path, ec)) {
        outError = "WAV file not found: " + path;
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        outError = "failed to open WAV file: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streampos sz = f.tellg();
    if (sz <= 0) {
        outError = "WAV file is empty: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    if (!f) {
        outError = "WAV file read truncated: " + path;
        return false;
    }
    return true;
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
                    "disk), `setup_completed` (bool — whether the user has seen and answered "
                    "the first-run setup dialog), and `api_key_resolved` (bool — whether the "
                    "5-row API-key fallback rule produces a non-empty key). Read-only, no side effects.";
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
        out["api_key_resolved"] = !ResolveWhisperKeyFromConfig(cfg).empty();
        // Surface whether whisper.cpp was linked into this build (sub-option
        // SMATCHET_WHISPER_LOCAL_BACKEND). Lets consumers distinguish
        // "model file missing on disk" from "local backend stub".
        out["local_backend"] = smatchet::whisper::WhisperLocal::BackendBuildState();
        return CommandResult::Success(std::move(out));
    };
    return c;
}

// --- Phase C additions: model download / progress / cancel + local mode. ---
//
// A single ModelDownloader instance is owned at file scope so download state
// survives a CLI re-invocation (the user issues `whisper.download-model` and
// then `whisper.model-progress` repeatedly on the same process). The setup
// banner has its own instance (SmatchetWhisperSetupBanner.cpp); banner vs.
// CLI are intentionally distinct so the banner is not interrupted by a CLI
// poll that happens to land on the same model.

smatchet::whisper::ModelDownloader& PluginOwnedDownloader() {
    static smatchet::whisper::ModelDownloader d;
    return d;
}

const char* DownloaderStateToString(smatchet::whisper::ModelDownloader::State st) {
    using State = smatchet::whisper::ModelDownloader::State;
    switch (st) {
    case State::Idle:
        return "Idle";
    case State::Downloading:
        return "Downloading";
    case State::Verifying:
        return "Verifying";
    case State::Complete:
        return "Complete";
    case State::Failed:
        return "Failed";
    case State::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

std::string ResolveWhisperModelDir() {
    const std::string sharedDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    if (sharedDir.empty()) {
        return std::string();
    }
    std::string out = sharedDir;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') {
        out.push_back('/');
    }
    out += "whisper";
    return out;
}

smatchet::cmd::Command BuildDownloadModelCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::ErrorCode;
    using smatchet::cmd::ParamSpec;
    using smatchet::cmd::ParamType;

    Command c;
    c.Name = "whisper.download-model";
    c.Category = "whisper";
    c.Summary = "Download a Whisper ggml model from the huggingface mirror to <userData>/whisper/<id>.bin.";
    c.Description =
        "Pattern A worker; returns immediately with {started: bool, model, url}. Poll "
        "`whisper.model-progress` for {state, bytes_received, bytes_expected, error}. Cancel "
        "via `whisper.cancel-download`. Requires fresh consent: the caller stamps "
        "cfg.WhisperConsentTimestampSec via the setup banner / Preferences first. SHA-256 "
        "verified against the catalog hash before the partial is renamed onto the final path.";
    c.Destructive = false;
    c.Idempotent = false;
    c.AsyncSafe = true;
    c.DryRunSupported = false;

    ParamSpec nameParam;
    nameParam.Name = "name";
    nameParam.Type = ParamType::String;
    nameParam.Required = true;
    nameParam.Description = "Model id (ggml-tiny.en | ggml-base.en | ggml-small.en).";
    c.Params = {std::move(nameParam)};

    c.Handler = [](const nlohmann::json& args, const CommandContext& ctx) -> CommandResult {
        const std::string modelId = args.value("name", std::string());
        if (modelId.empty()) {
            return CommandResult::Failure(ErrorCode::ValidationError, "--name is required");
        }
        const smatchet::whisper::catalog::Entry* e = smatchet::whisper::catalog::Find(modelId);
        if (e == nullptr) {
            return CommandResult::Failure(ErrorCode::ValidationError, "unknown model id: " + modelId);
        }
        const std::string destDir = ResolveWhisperModelDir();
        if (destDir.empty()) {
            return CommandResult::Failure(ErrorCode::HandlerError,
                                          "platform shared user-data directory unavailable");
        }
        if (ctx.App == nullptr) {
            return CommandResult::Failure(ErrorCode::HandlerError, "AppController unavailable");
        }
        std::string err;
        if (!PluginOwnedDownloader().Start(*ctx.App, modelId, destDir, err)) {
            return CommandResult::Failure(ErrorCode::HandlerError, err);
        }
        nlohmann::json out;
        out["started"] = true;
        out["model"] = modelId;
        out["url"] = e->Url;
        return CommandResult::Success(std::move(out));
    };
    return c;
}

smatchet::cmd::Command BuildModelProgressCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::CommandContext;

    Command c;
    c.Name = "whisper.model-progress";
    c.Category = "whisper";
    c.Summary = "Snapshot of the active model download (state + bytes received / expected).";
    c.Description =
        "Returns {state: Idle|Downloading|Verifying|Complete|Failed|Cancelled, bytes_received, "
        "bytes_expected, error, model}. Cheap; safe to poll at UI frame rate.";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) -> CommandResult {
        const auto prog = PluginOwnedDownloader().GetProgress();
        nlohmann::json out;
        out["state"] = DownloaderStateToString(prog.state);
        out["bytes_received"] = static_cast<std::int64_t>(prog.bytesReceived);
        out["bytes_expected"] = static_cast<std::int64_t>(prog.bytesExpected);
        out["error"] = prog.error;
        out["model"] = prog.modelId;
        return CommandResult::Success(std::move(out));
    };
    return c;
}

smatchet::cmd::Command BuildCancelDownloadCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::CommandContext;

    Command c;
    c.Name = "whisper.cancel-download";
    c.Category = "whisper";
    c.Summary = "Cancel an in-flight Whisper model download; partial file is preserved for resume.";
    c.Description =
        "Flips the worker's cancel atom. Returns {cancelled: true}. Idempotent — safe to call "
        "when no download is running.";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) -> CommandResult {
        PluginOwnedDownloader().Cancel();
        nlohmann::json out;
        out["cancelled"] = true;
        return CommandResult::Success(std::move(out));
    };
    return c;
}

smatchet::cmd::Command BuildTranscribeOnceCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::ErrorCode;
    using smatchet::cmd::ParamSpec;
    using smatchet::cmd::ParamType;

    Command c;
    c.Name = "whisper.transcribe-once";
    c.Category = "whisper";
    c.Summary = "Capture audio (or read a WAV) and run a single cloud transcription via OpenAI Whisper.";
    c.Description =
        "Runs one transcription. With `--file`, reads a WAV from disk and skips capture. Without "
        "`--file`, captures from the default Windows microphone for `--seconds` (default 5) then "
        "transcribes. `--mode cloud` is the only mode supported in Phase B; `local` is rejected "
        "with a clear error; `auto` falls back to cloud. API key resolution mirrors the 5-row "
        "fallback rule in docs/design/whisper-dictation.md § API key fallback rule. On success "
        "returns `{text, elapsed_ms, mode, captured_samples}`; on failure returns a Failure "
        "result with the transport / parse / key error.";
    c.Destructive = false;
    c.Idempotent = false;
    c.AsyncSafe = true; // synchronous handler; runs on the CLI thread
    c.DryRunSupported = false;

    ParamSpec fileParam;
    fileParam.Name = "file";
    fileParam.Type = ParamType::String;
    fileParam.Required = false;
    fileParam.Description = "Optional path to a WAV file. When supplied, skips audio capture.";
    ParamSpec secondsParam;
    secondsParam.Name = "seconds";
    secondsParam.Type = ParamType::Int;
    secondsParam.Required = false;
    secondsParam.Description = "Capture duration in seconds when --file is not used (default 5).";
    secondsParam.Default = 5;
    ParamSpec modeParam;
    modeParam.Name = "mode";
    modeParam.Type = ParamType::String;
    modeParam.Required = false;
    modeParam.Description = "Backend selection. Phase B only honours 'cloud'; 'local' is rejected.";
    modeParam.Default = "cloud";
    modeParam.Enum = {"cloud", "auto", "local"};
    c.Params = {std::move(fileParam), std::move(secondsParam), std::move(modeParam)};

    c.Handler = [](const nlohmann::json& args, const CommandContext& /*ctx*/) -> CommandResult {
        const std::string mode = args.value("mode", std::string("cloud"));
        const TrackerConfig cfg = ConfigManager::Load();

        // Phase C mode-router decision tree (see docs/design/whisper-dictation.md
        // § Mode router decision tree). `auto` prefers local-if-present, else
        // falls back to cloud. `local` is hard-gated on a present model. `cloud`
        // unconditionally takes the cloud path. Cloud-on-fallback after a local
        // failure is handled at the higher Phase E layer; CLI is one-shot.
        const std::string modelDir = ResolveWhisperModelDir();
        const bool localPresent = !cfg.WhisperModel.empty() &&
                                  smatchet::whisper::catalog::IsModelPresent(cfg.WhisperModel, modelDir);
        std::string effectiveMode;
        if (mode == "cloud") {
            effectiveMode = "cloud";
        } else if (mode == "local") {
            if (!localPresent) {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "local mode requires a downloaded model; run "
                                              "whisper.download-model --name <id> first");
            }
            effectiveMode = "local";
        } else { // auto
            effectiveMode = localPresent ? "local" : "cloud";
        }

        std::string apiKey;
        if (effectiveMode == "cloud") {
            apiKey = ResolveWhisperKeyFromConfig(cfg);
            if (apiKey.empty()) {
                return CommandResult::Failure(
                    ErrorCode::ValidationError,
                    "no API key available - set WhisperApiKey or AiApiKey (provider=openai)");
            }
            // Phase C consent invariant #3 — no cloud API call without
            // explicit opt-in. The CLI smoke command is exempt in Phase B
            // because there was no opt-in surface; now that the banner +
            // Preferences exist, the gate is the same one the Phase E hotkey
            // path will use. The CLI consent bypass is removed.
            if (!smatchet::whisper::consent::CanCallCloudApi(cfg, apiKey)) {
                return CommandResult::Failure(
                    ErrorCode::ValidationError,
                    "consent required: enable Whisper dictation via the setup banner or "
                    "Preferences first");
            }
        }

        std::vector<std::uint8_t> wavBytes;
        std::vector<std::int16_t> capturedPcm; // populated on capture path only
        std::size_t capturedSamples = 0;
        const std::string filePath = args.value("file", std::string());
        if (!filePath.empty()) {
            std::string err;
            if (!ReadWavFile(filePath, wavBytes, err)) {
                return CommandResult::Failure(ErrorCode::HandlerError, err);
            }
        } else {
            const int seconds = args.value("seconds", 5);
            if (seconds <= 0 || seconds > 600) {
                return CommandResult::Failure(ErrorCode::ValidationError,
                                              "seconds must be in (0, 600]");
            }
            smatchet::whisper::WindowsAudioCapture cap;
            std::string err;
            if (!cap.Start(err)) {
                return CommandResult::Failure(ErrorCode::HandlerError,
                                              std::string("audio capture start failed: ") + err);
            }
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            cap.Stop();
            if (!cap.DrainCapturedPcm(capturedPcm)) {
                return CommandResult::Failure(ErrorCode::HandlerError,
                                              "no audio captured (mic permission denied or no default device)");
            }
            capturedSamples = capturedPcm.size();
            wavBytes = smatchet::whisper::pure::EncodeWav(
                capturedPcm,
                smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate,
                smatchet::whisper::WindowsAudioCapture::kCaptureChannels);
        }

        const auto t0 = std::chrono::steady_clock::now();
        std::string text;
        std::string err;
        bool ok = false;

        if (effectiveMode == "local") {
            if (capturedPcm.empty()) {
                // --file input to local mode is intentionally out of scope for
                // Phase C — we don't ship a WAV decoder yet. Use captured audio
                // or `--mode cloud` for file input.
                return CommandResult::Failure(
                    ErrorCode::ValidationError,
                    "local mode currently only accepts captured audio (omit --file). A WAV decoder "
                    "for --file lands with the Phase D scenario harness.");
            }
            const std::string modelPath = modelDir + "/" + cfg.WhisperModel + ".bin";
            smatchet::whisper::WhisperLocal local;
            std::string loadErr;
            if (!local.LoadModel(modelPath, loadErr)) {
                return CommandResult::Failure(ErrorCode::HandlerError,
                                              std::string("local model load failed: ") + loadErr);
            }
            ok = local.Transcribe(capturedPcm, text, err);
        } else {
            if (wavBytes.empty()) {
                return CommandResult::Failure(ErrorCode::HandlerError, "WAV payload is empty");
            }
            smatchet::whisper::WhisperApiClient client;
            ok = client.Transcribe(wavBytes, apiKey, text, err);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const long long elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (!ok) {
            // ErrorCode::HandlerError for transport / HTTP / parse / local — surface
            // the redacted error message the inner client assembled.
            return CommandResult::Failure(ErrorCode::HandlerError, err);
        }

        nlohmann::json out;
        out["text"] = text;
        out["elapsed_ms"] = elapsedMs;
        out["mode"] = effectiveMode;
        out["captured_samples"] = static_cast<std::int64_t>(capturedSamples);
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
    reg.Register(BuildTranscribeOnceCommand());
    reg.Register(BuildDownloadModelCommand());
    reg.Register(BuildModelProgressCommand());
    reg.Register(BuildCancelDownloadCommand());
    LOG_INFO("WhisperPlugin: Phase C started; whisper.{status,transcribe-once,download-model,model-progress,"
             "cancel-download} registered (local backend %s).",
             smatchet::whisper::WhisperLocal::BackendBuildState());
}

void WhisperPlugin::OnStop() {
    LOG_INFO("WhisperPlugin: stopped.");
}
