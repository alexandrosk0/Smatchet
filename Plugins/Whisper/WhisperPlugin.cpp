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
#include "WavWriter.h"
#include "WhisperApiClient.h"
#include "WhisperApiKeyResolve.h"
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
        if (mode == "local") {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "local mode arrives in Phase C; pass --mode cloud or --mode auto");
        }
        // `auto` and `cloud` both reach the cloud path in Phase B.

        const TrackerConfig cfg = ConfigManager::Load();
        const std::string apiKey = ResolveWhisperKeyFromConfig(cfg);
        if (apiKey.empty()) {
            return CommandResult::Failure(
                ErrorCode::ValidationError,
                "no API key available - set WhisperApiKey or AiApiKey (provider=openai)");
        }

        std::vector<std::uint8_t> wavBytes;
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
            std::vector<std::int16_t> pcm;
            if (!cap.DrainCapturedPcm(pcm)) {
                return CommandResult::Failure(ErrorCode::HandlerError,
                                              "no audio captured (mic permission denied or no default device)");
            }
            capturedSamples = pcm.size();
            wavBytes = smatchet::whisper::pure::EncodeWav(
                pcm,
                smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate,
                smatchet::whisper::WindowsAudioCapture::kCaptureChannels);
        }
        if (wavBytes.empty()) {
            return CommandResult::Failure(ErrorCode::HandlerError, "WAV payload is empty");
        }

        smatchet::whisper::WhisperApiClient client;
        const auto t0 = std::chrono::steady_clock::now();
        std::string text;
        std::string err;
        const bool ok = client.Transcribe(wavBytes, apiKey, text, err);
        const auto t1 = std::chrono::steady_clock::now();
        const long long elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (!ok) {
            // ErrorCode::HandlerError for transport / HTTP / parse — surface the
            // redacted error message Transcribe assembled.
            return CommandResult::Failure(ErrorCode::HandlerError, err);
        }

        nlohmann::json out;
        out["text"] = text;
        out["elapsed_ms"] = elapsedMs;
        out["mode"] = (mode == "auto") ? std::string("cloud") : mode;
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
    LOG_INFO("WhisperPlugin: Phase B started; whisper.status + whisper.transcribe-once registered.");
}

void WhisperPlugin::OnStop() {
    LOG_INFO("WhisperPlugin: stopped.");
}
