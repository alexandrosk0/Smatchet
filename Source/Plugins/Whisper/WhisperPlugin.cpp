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
#include "Commands/PathConfinement.h"
#include "ConfigManager.h"
#include "DictationInsertionRouter.h"
#include "GlobalHotkey_Win32.h"
#include "HotkeyParse.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "ModelCatalog.h"
#include "ModelDownloader.h"
#include "SilenceTrim.h"
#include "WavWriter.h"
#include "WhisperApiClient.h"
#include "WhisperApiKeyResolve.h"
#include "WhisperConsentGate.h"
#include "WhisperLocal.h"
#include "WindowsAudioCapture.h"

#include "imgui.h"
// imgui_internal.h for ImGuiContext::ActiveId access — ImGui 1.92's public
// API does not expose `GetActiveID()`. The post-transcription splice path
// snapshots ActiveId once on the UI thread to pick the correct registered
// entry instead of always picking entries_.front(). Tracked under the
// internal-API caveat: an ImGui major-version bump would need to revisit
// this reach.
#include "imgui_internal.h"

// SMATCHET_DEVIATION(rule=duplication; reason=standard-library + json/filesystem include block overlaps the McpPlugin
// include block; include lists are not factorable behind a helper and a shared umbrella header would couple two
// independent plugins; owner=security-audit; revisit=2026-09-30)
#include <nlohmann/json.hpp>

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Process-wide mock-transcription seam. Set by the
// `whisper-dictation-roundtrip` scenario to bypass real capture + transcription;
// the hotkey press/release path observes this and posts the canned text onto
// the UI thread after the requested delay. Mutex-guarded because the producer
// (UI-thread scenario) and the consumer (hook worker thread + LaunchBackgroundTask
// worker) race on the same string buffer.
std::mutex& MockMutex() {
    static std::mutex m;
    return m;
}
std::atomic<bool>& MockActiveAtom() {
    static std::atomic<bool> a{false};
    return a;
}
WhisperPlugin::MockTranscription& MockStorage() {
    static WhisperPlugin::MockTranscription m;
    return m;
}

} // namespace

void WhisperPlugin::SetMockTranscription(const WhisperPlugin::MockTranscription& mock) {
    std::lock_guard<std::mutex> lk(MockMutex());
    MockStorage() = mock;
    MockActiveAtom().store(true, std::memory_order_release);
    LOG_INFO("WhisperPlugin: mock transcription armed (text=\"%s\", delay=%lld ms)", mock.text.c_str(),
             static_cast<long long>(mock.delay.count()));
}

void WhisperPlugin::ClearMockTranscription() {
    std::lock_guard<std::mutex> lk(MockMutex());
    MockActiveAtom().store(false, std::memory_order_release);
    MockStorage() = WhisperPlugin::MockTranscription();
    LOG_INFO("WhisperPlugin: mock transcription cleared");
}

bool WhisperPlugin::MockTranscriptionActive() { return MockActiveAtom().load(std::memory_order_acquire); }

WhisperPlugin::MockTranscription WhisperPlugin::CurrentMockTranscription() {
    std::lock_guard<std::mutex> lk(MockMutex());
    return MockStorage();
}

// --- Phase E state struct definition ---
// Out-of-class definition of the WhisperPlugin::PhaseEState nested struct,
// hoisted to the top of the TU so every anonymous-namespace command builder
// + worker helper below sees the complete type. The struct itself is opaque
// to consumers outside Source/Plugins/Whisper/ — the gating-OFF build never visits
// this TU.

struct WhisperPlugin::PhaseEState {
    AppController* app = nullptr;
    smatchet::whisper::GlobalHotkey_Win32 hotkey;
    smatchet::whisper::WindowsAudioCapture capture;

    // Guard against re-entrant capture: a stuck hook (very rare) can in
    // theory fire onPress while the previous press is still draining. The
    // atomic gates the start path; the release path only runs when the
    // start path observed the flip.
    std::atomic<bool> captureActive{false};

    // One-flight transcription dispatch. Set on press, cleared once the
    // release worker has posted insertion (or given up). Prevents two
    // hotkey rounds racing two worker pipelines.
    std::atomic<bool> transcribeInFlight{false};
};

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
    /* PILLAR2_WORKER_ONLY */ // est-latency: ~100ms — sole call site is whisper.transcribe-once handler (AsyncSafe =
                              // true, CLI worker thread).
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

smatchet::cmd::Command BuildStatusCommand(WhisperPlugin::PhaseEState* state) {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;

    Command c;
    c.Name = "whisper.status";
    c.Category = "whisper";
    c.Summary = "Report the Whisper dictation plugin's runtime status (no audio access).";
    c.Description = "Returns a JSON blob with: `enabled` (bool — runtime opt-in flag from "
                    "ConfigManager.WhisperEnabled), `mode` (string — auto|local|cloud), "
                    "`model_present` (bool — whether the configured local model file exists on "
                    "disk), `setup_completed` (bool — whether the user has seen and answered "
                    "the first-run setup dialog), `api_key_resolved` (bool — whether the "
                    "5-row API-key fallback rule produces a non-empty key), `is_recording` (bool "
                    "— live Phase E push-to-talk capture state), and `hotkey_registered` (bool — "
                    "whether the global hotkey listener is currently installed). Read-only, no "
                    "side effects.";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [state](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) -> CommandResult {
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
        out["is_recording"] = g_dictationRouter.IsRecording();
        out["hotkey_registered"] = state != nullptr && state->hotkey.IsRegistered();
        return CommandResult::Success(std::move(out));
    };
    return c;
}

// --- Phase C additions: model download / progress / cancel + local mode. ---
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
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;
    using smatchet::cmd::ParamSpec;
    using smatchet::cmd::ParamType;

    Command c;
    c.Name = "whisper.download-model";
    c.Category = "whisper";
    c.Summary = "Download a Whisper ggml model from the huggingface mirror to <userData>/whisper/<id>.bin.";
    c.Description = "Pattern A worker; returns immediately with {started: bool, model, url}. Poll "
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
            return CommandResult::Failure(ErrorCode::HandlerError, "platform shared user-data directory unavailable");
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
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;

    Command c;
    c.Name = "whisper.model-progress";
    c.Category = "whisper";
    c.Summary = "Snapshot of the active model download (state + bytes received / expected).";
    c.Description = "Returns {state: Idle|Downloading|Verifying|Complete|Failed|Cancelled, bytes_received, "
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
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;

    Command c;
    c.Name = "whisper.cancel-download";
    c.Category = "whisper";
    c.Summary = "Cancel an in-flight Whisper model download; partial file is preserved for resume.";
    c.Description = "Flips the worker's cancel atom. Returns {cancelled: true}. Idempotent — safe to call "
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

// Resolve the effective backend for whisper.transcribe-once from the requested
// `mode` + on-disk model presence. Returns a CommandResult::Failure when the
// requested mode can't be satisfied (e.g. `local` with no model); on success
// `outEffectiveMode` is set and the returned result is unused by the caller.
// `outOk` distinguishes "resolved" from "early-failure".
smatchet::cmd::CommandResult ResolveTranscribeOnceMode(const std::string& mode, const TrackerConfig& cfg,
                                                       const std::string& modelDir, std::string& outEffectiveMode,
                                                       std::string& outModelName, bool& outOk) {
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;
    outOk = false;

    // Phase C mode-router decision tree (see docs/plans/shipped/whisper-dictation.md
    // § Mode router decision tree). `auto` prefers local-if-present, else
    // falls back to cloud. `local` is hard-gated on a present model. `cloud`
    // unconditionally takes the cloud path. Cloud-on-fallback after a local
    // failure is handled at the higher Phase E layer; CLI is one-shot.
    // Mirror the streaming path's empty-model fallback (ResolveLocalModelPresent
    // / Transcribe both default an empty cfg.WhisperModel to "ggml-base.en").
    // Without this, an empty WhisperModel made auto/local probe a bogus
    // "<dir>/.bin" path and never resolve to local. Resolve the effective name
    // once and use it for both the presence probe and the on-disk model path.
    outModelName = cfg.WhisperModel.empty() ? std::string("ggml-base.en") : cfg.WhisperModel;
    const bool localPresent = smatchet::whisper::catalog::IsModelPresent(outModelName, modelDir);
    if (mode == "cloud") {
        outEffectiveMode = "cloud";
    } else if (mode == "local") {
        if (!localPresent) {
            return CommandResult::Failure(ErrorCode::ValidationError, "local mode requires a downloaded model; run "
                                                                      "whisper.download-model --name <id> first");
        }
        outEffectiveMode = "local";
    } else { // auto
        outEffectiveMode = localPresent ? "local" : "cloud";
    }
    outOk = true;
    return CommandResult::Success(nlohmann::json::object());
}

// Acquire the audio payload for whisper.transcribe-once — either by reading the
// `--file` WAV from disk or by capturing `--seconds` of mic audio. Fills
// `outWav` (always) + `outPcm` / `outSamples` (capture path only). Returns a
// CommandResult::Failure on any acquisition error; `outOk` is true on success.
smatchet::cmd::CommandResult AcquireTranscribeOnceAudio(const nlohmann::json& args, std::vector<std::uint8_t>& outWav,
                                                        std::vector<std::int16_t>& outPcm, std::size_t& outSamples,
                                                        bool& outOk) {
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;
    outOk = false;
    outSamples = 0;

    const std::string filePath = args.value("file", std::string());
    if (!filePath.empty()) {
        // SECURITY: `file` is caller-supplied and whisper.transcribe-once is reachable
        // from the MCP command surface. Confine the read under a dedicated
        // <userData>/whisper-import/ subdir — not the user-data root, which holds
        // smatchet_config.json and other state a caller has no business exfiltrating to
        // the cloud transcription endpoint. transcribe-once is a smoke/diagnostic path,
        // so a dedicated import directory is the right blast radius.
        std::string resolvedPath, confineErr;
        if (!smatchet::cmd::ConfinePathUnderSubdir(ConfigManager::GetUserDataDirectory(), "whisper-import", filePath,
                                                   resolvedPath, confineErr)) {
            return CommandResult::Failure(ErrorCode::ValidationError, "transcribe-once --file rejected: " + confineErr);
        }
        // Only accept .wav — ReadWavFile expects PCM WAV, and refusing other extensions
        // keeps a caller from coaxing the diagnostic path into shipping arbitrary bytes.
        {
            std::string ext = ghc::filesystem::path(resolvedPath).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (ext != ".wav") {
                return CommandResult::Failure(ErrorCode::ValidationError, "transcribe-once --file must be a .wav file");
            }
        }
        // Fail closed before the file can reach the cloud path: require a regular file
        // (not a fifo/device/dir) and a successful stat. A diagnostic WAV is small, so a
        // stat error or oversize is rejected rather than silently allowed through.
        std::error_code typeEc;
        if (!ghc::filesystem::is_regular_file(resolvedPath, typeEc) || typeEc) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "transcribe-once --file must be a regular .wav file");
        }
        std::error_code sizeEc;
        const auto fileSize = ghc::filesystem::file_size(resolvedPath, sizeEc);
        constexpr std::uintmax_t kMaxTranscribeWavBytes = 256ull * 1024ull * 1024ull;
        if (sizeEc) {
            return CommandResult::Failure(ErrorCode::ValidationError, "transcribe-once --file could not be statted");
        }
        if (fileSize > kMaxTranscribeWavBytes) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "transcribe-once --file exceeds the 256 MiB limit");
        }
        std::string err;
        if (!ReadWavFile(resolvedPath, outWav, err)) {
            return CommandResult::Failure(ErrorCode::HandlerError, err);
        }
        // Confirm the RIFF/WAVE container so a non-audio file merely renamed `.wav`
        // cannot be shipped to the cloud transcription endpoint.
        const bool hasWavMagic =
            outWav.size() >= 12 && outWav[0] == static_cast<std::uint8_t>('R') &&
            outWav[1] == static_cast<std::uint8_t>('I') && outWav[2] == static_cast<std::uint8_t>('F') &&
            outWav[3] == static_cast<std::uint8_t>('F') && outWav[8] == static_cast<std::uint8_t>('W') &&
            outWav[9] == static_cast<std::uint8_t>('A') && outWav[10] == static_cast<std::uint8_t>('V') &&
            outWav[11] == static_cast<std::uint8_t>('E');
        if (!hasWavMagic) {
            return CommandResult::Failure(ErrorCode::ValidationError, "transcribe-once --file is not a WAV file");
        }
        outOk = true;
        return CommandResult::Success(nlohmann::json::object());
    }

    const int seconds = args.value("seconds", 5);
    if (seconds <= 0 || seconds > 600) {
        return CommandResult::Failure(ErrorCode::ValidationError, "seconds must be in (0, 600]");
    }
    smatchet::whisper::WindowsAudioCapture cap;
    std::string err;
    if (!cap.Start(err)) {
        return CommandResult::Failure(ErrorCode::HandlerError, std::string("audio capture start failed: ") + err);
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    cap.Stop();
    if (!cap.DrainCapturedPcm(outPcm)) {
        return CommandResult::Failure(ErrorCode::HandlerError,
                                      "no audio captured (mic permission denied or no default device)");
    }
    outSamples = outPcm.size();
    outWav = smatchet::whisper::pure::EncodeWav(outPcm, smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate,
                                                smatchet::whisper::WindowsAudioCapture::kCaptureChannels);
    outOk = true;
    return CommandResult::Success(nlohmann::json::object());
}

// Handler body for whisper.transcribe-once. Routes mode, resolves the API key +
// consent (cloud path), acquires audio, runs the single transcription, and
// shapes the {text, elapsed_ms, mode, captured_samples} success payload.
smatchet::cmd::CommandResult RunTranscribeOnce(const nlohmann::json& args) {
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;

    const std::string mode = args.value("mode", std::string("cloud"));
    const TrackerConfig cfg = ConfigManager::Load();
    const std::string modelDir = ResolveWhisperModelDir();

    std::string effectiveMode;
    std::string effectiveModelName;
    bool modeOk = false;
    CommandResult modeRes = ResolveTranscribeOnceMode(mode, cfg, modelDir, effectiveMode, effectiveModelName, modeOk);
    if (!modeOk) {
        return modeRes;
    }

    std::string apiKey;
    if (effectiveMode == "cloud") {
        apiKey = ResolveWhisperKeyFromConfig(cfg);
        if (apiKey.empty()) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "no API key available - set WhisperApiKey or AiApiKey (provider=openai)");
        }
        // Phase C consent invariant #3 — no cloud API call without
        // explicit opt-in. The CLI smoke command is exempt in Phase B
        // because there was no opt-in surface; now that the banner +
        // Preferences exist, the gate is the same one the Phase E hotkey
        // path will use. The CLI consent bypass is removed.
        if (!smatchet::whisper::consent::CanCallCloudApi(cfg, apiKey)) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "consent required: enable Whisper dictation via the setup banner or "
                                          "Preferences first");
        }
    }

    std::vector<std::uint8_t> wavBytes;
    std::vector<std::int16_t> capturedPcm; // populated on capture path only
    std::size_t capturedSamples = 0;
    bool audioOk = false;
    CommandResult audioRes = AcquireTranscribeOnceAudio(args, wavBytes, capturedPcm, capturedSamples, audioOk);
    if (!audioOk) {
        return audioRes;
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
        const std::string modelPath = modelDir + "/" + effectiveModelName + ".bin";
        smatchet::whisper::WhisperLocal local;
        std::string loadErr;
        if (!local.LoadModel(modelPath, loadErr)) {
            return CommandResult::Failure(ErrorCode::HandlerError, std::string("local model load failed: ") + loadErr);
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
    const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

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
}

smatchet::cmd::Command BuildTranscribeOnceCommand() {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;
    using smatchet::cmd::ParamSpec;
    using smatchet::cmd::ParamType;

    Command c;
    c.Name = "whisper.transcribe-once";
    c.Category = "whisper";
    c.Summary = "Capture audio (or read a WAV) and run a single cloud transcription via OpenAI Whisper.";
    c.Description = "Runs one transcription. With `--file`, reads a WAV from disk and skips capture. Without "
                    "`--file`, captures from the default Windows microphone for `--seconds` (default 5) then "
                    "transcribes. `--mode cloud` is the only mode supported in Phase B; `local` is rejected "
                    "with a clear error; `auto` falls back to cloud. API key resolution mirrors the 5-row "
                    "fallback rule in docs/plans/shipped/whisper-dictation.md § API key fallback rule. On success "
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
    secondsParam.Default = std::make_shared<nlohmann::json>(5);
    ParamSpec modeParam;
    modeParam.Name = "mode";
    modeParam.Type = ParamType::String;
    modeParam.Required = false;
    modeParam.Description = "Backend selection. Phase B only honours 'cloud'; 'local' is rejected.";
    modeParam.Default = std::make_shared<nlohmann::json>("cloud");
    modeParam.Enum = {"cloud", "auto", "local"};
    c.Params = {std::move(fileParam), std::move(secondsParam), std::move(modeParam)};

    c.Handler = [](const nlohmann::json& args, const CommandContext& ctx) -> CommandResult {
        // CPP_CODE_AUDIT.md #15: consent::CanCaptureMic only checks Whisper is enabled +
        // set up — it has no notion of WHO is asking. Without `--file`, this command starts
        // real WASAPI mic capture, and it's reachable from the loopback MCP surface with no
        // hotkey press or visible prompt: a local MCP client could silently record up to 600 s
        // of audio. Gate the capture path (not `--file`, which is separately confined) behind
        // a non-MCP CommandSource — MCP automation must go through `--file` instead.
        const bool isCapturePath = args.value("file", std::string()).empty();
        if (isCapturePath && ctx.Source == smatchet::cmd::CommandSource::Mcp) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "whisper.transcribe-once mic capture is not available over MCP — pass "
                                          "--file to transcribe a WAV instead.");
        }
        return RunTranscribeOnce(args);
    };

    return c;
}

// Run the transcription pipeline for a buffer of int16 PCM captured by the
// Phase E hotkey path. Worker-thread only — never call from UI. Handles the
// same mode-router + consent + key-resolution branches as
// `whisper.transcribe-once` but factored out so the hotkey path doesn't
// duplicate ~100 LOC.
// Returns true + populates `outText` on success; returns false + sets
// `outError` on any failure (consent denied, no API key, no model, HTTP
// error, etc). All branches log so a user-reported "nothing happened on
// release" has a single grep target.
bool RunTranscriptionPipeline_Worker(const std::vector<std::int16_t>& capturedPcm, std::string& outText,
                                     std::string& outError) {
    outText.clear();
    outError.clear();
    if (capturedPcm.empty()) {
        outError = "no audio captured (release fired before any frames arrived)";
        return false;
    }

    const TrackerConfig cfg = ConfigManager::Load();

    // Phase F audio-shaping pass — trim leading/trailing silence (when
    // WhisperTrim is on) and clamp to WhisperMaxClipSec seconds. Both steps
    // run on the captured PCM before mode routing so cloud payload + local
    // inference both see the same shaped buffer. Pure helpers; cheap (< 1 ms
    // on 10 s clips at 16 kHz).
    std::vector<std::int16_t> shapedPcm = capturedPcm;
    if (cfg.WhisperTrim) {
        shapedPcm = smatchet::whisper::pure::TrimLeadingTrailingSilence(
            shapedPcm, smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate);
    }
    if (cfg.WhisperMaxClipSec > 0) {
        const std::size_t maxSamples =
            static_cast<std::size_t>(cfg.WhisperMaxClipSec) *
            static_cast<std::size_t>(smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate);
        if (shapedPcm.size() > maxSamples) {
            LOG_WARN("Whisper: clip exceeded WhisperMaxClipSec=%d (had %zu samples, capping to %zu)",
                     cfg.WhisperMaxClipSec, shapedPcm.size(), maxSamples);
            shapedPcm = smatchet::whisper::pure::CapClipSamples(shapedPcm, maxSamples);
        }
    }
    if (shapedPcm.empty()) {
        outError = "audio buffer empty after trim (entire clip below silence threshold)";
        return false;
    }

    const std::string modelDir = ResolveWhisperModelDir();
    const bool localPresent =
        !cfg.WhisperModel.empty() && smatchet::whisper::catalog::IsModelPresent(cfg.WhisperModel, modelDir);
    const std::string requestedMode = cfg.WhisperMode.empty() ? std::string("auto") : cfg.WhisperMode;

    std::string effectiveMode;
    if (requestedMode == "cloud") {
        effectiveMode = "cloud";
    } else if (requestedMode == "local") {
        if (!localPresent) {
            outError = "local mode requested but no model installed; falling back to cloud";
            effectiveMode = "cloud";
        } else {
            effectiveMode = "local";
        }
    } else {
        effectiveMode = localPresent ? "local" : "cloud";
    }

    // Phase F — forward cfg.WhisperLanguage to whichever backend runs.
    // Default "en" matches the ggml-*.en models we ship; "auto" / empty
    // asks the backend to autodetect (only useful with a multilingual model
    // or against the cloud endpoint).
    const std::string langArg = cfg.WhisperLanguage.empty() ? std::string("en") : cfg.WhisperLanguage;

    if (effectiveMode == "local") {
        const std::string modelPath = modelDir + "/" + cfg.WhisperModel + ".bin";
        smatchet::whisper::WhisperLocal local;
        std::string loadErr;
        if (!local.LoadModel(modelPath, loadErr)) {
            outError = std::string("local model load failed: ") + loadErr;
            return false;
        }
        std::string err;
        if (!local.Transcribe(shapedPcm, langArg, outText, err)) {
            outError = err;
            return false;
        }
        return true;
    }

    // Cloud path.
    const std::string apiKey = ResolveWhisperKeyFromConfig(cfg);
    if (apiKey.empty()) {
        outError = "no API key available - set WhisperApiKey or AiApiKey (provider=openai)";
        return false;
    }
    if (!smatchet::whisper::consent::CanCallCloudApi(cfg, apiKey)) {
        outError = "consent required: enable Whisper dictation in Preferences first";
        return false;
    }

    const std::vector<std::uint8_t> wavBytes =
        smatchet::whisper::pure::EncodeWav(shapedPcm, smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate,
                                           smatchet::whisper::WindowsAudioCapture::kCaptureChannels);
    if (wavBytes.empty()) {
        outError = "WAV payload is empty (zero captured samples)";
        return false;
    }

    smatchet::whisper::WhisperApiClient client;
    return client.Transcribe(wavBytes, apiKey, langArg, outText, outError);
}

// Phase F — true when `text` ends with sentence-final punctuation, ignoring
// trailing whitespace. Pure helper; the auto-send-on-punctuation post-insertion
// path uses it to decide whether to fire the AI Assistant Send action.
bool EndsWithSentencePunctuation(const std::string& text) {
    for (std::size_t i = text.size(); i > 0; --i) {
        const char c = text[i - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        return c == '.' || c == '!' || c == '?';
    }
    return false;
}

// Hotkey-press path — worker-thread entry point. Starts capture if consent
// allows + nothing is in flight; mirrors the recording flag to the router so
// the UI indicator + overlay light up. NEVER touches ImGui state.
void RunHotkeyPress_Worker(WhisperPlugin::PhaseEState* state) {
    if (state == nullptr || state->app == nullptr) {
        return;
    }
    // Mock-transcription seam — short-circuit before the consent + WASAPI
    // surface so headless scenario tests don't need a working mic or an
    // approved consent toggle. Only the recording flag flips (so the UI
    // indicator can still be exercised); the actual capture is skipped.
    if (WhisperPlugin::MockTranscriptionActive()) {
        bool expected = false;
        if (!state->captureActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            LOG_DEBUG("Whisper hotkey press ignored — capture already active (mock)");
            return;
        }
        g_dictationRouter.SetRecording(true);
        LOG_DEBUG("Whisper hotkey: capture started (mock seam — WASAPI bypassed)");
        return;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    if (!smatchet::whisper::consent::CanCaptureMic(cfg)) {
        LOG_WARN("Whisper hotkey: consent gate denied capture — Preferences->Whisper not enabled");
        return;
    }
    bool expected = false;
    if (!state->captureActive.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LOG_DEBUG("Whisper hotkey press ignored — capture already active");
        return;
    }
    std::string capErr;
    if (!state->capture.Start(capErr)) {
        LOG_ERROR("Whisper hotkey: capture start failed: %s", capErr.c_str());
        state->captureActive.store(false, std::memory_order_release);
        return;
    }
    // SetRecording is lock-free atomic — safe from worker; UI thread polls it
    // every frame for the indicator + overlay.
    g_dictationRouter.SetRecording(true);
    LOG_DEBUG("Whisper hotkey: capture started");
}

// Hotkey-release path — worker-thread entry point. Stops capture, drains
// the PCM, runs transcription, and posts the resulting text into the
// focused InputText on the UI thread.
// Mock-transcription lower half of the release path — when the mock seam is
// armed, skip WASAPI drain entirely and queue the canned text for insertion
// after the configured delay. Split out of RunHotkeyRelease_Worker to keep both
// halves under the size cap. Production code never reaches this body (the caller
// gates on WhisperPlugin::MockTranscriptionActive()).
void RunHotkeyRelease_Mock_Worker(WhisperPlugin::PhaseEState* state) {
    bool wasActive = state->captureActive.exchange(false, std::memory_order_acq_rel);
    if (!wasActive) {
        LOG_DEBUG("Whisper hotkey release ignored — no active capture (mock)");
        return;
    }
    g_dictationRouter.SetRecording(false);
    bool expectedFlight = false;
    if (!state->transcribeInFlight.compare_exchange_strong(expectedFlight, true, std::memory_order_acq_rel)) {
        LOG_DEBUG("Whisper hotkey release (mock): prior transcription still in flight");
        return;
    }
    const WhisperPlugin::MockTranscription mock = WhisperPlugin::CurrentMockTranscription();
    AppController* app = state->app;
    WhisperPlugin::PhaseEState* localState = state;
    // Mirror the production indicator wire-up — the menu-bar "REC" → amber
    // "Transcribing..." switchover (SmatchetUI_MainMenu.cpp:492-499) keys
    // off this flag pair. Without it, the mock seam couldn't exercise cell 4
    // (Transcribing indicator) at all. Cleared inside the post-back on the
    // UI thread so the splice and indicator-clear land in the same frame,
    // matching the production semantics in RunHotkeyRelease_Worker.
    g_dictationRouter.SetTranscribing(true);
    app->LaunchBackgroundTask([app, localState, mock]() {
        // Simulate the wall-clock delay a real round-trip would incur so
        // the scenario observes the same press → asynchronous-post → insert
        // ordering as the production cloud / local path. Then hand off via
        // the same MainThreadDispatcher route the real release uses.
        std::this_thread::sleep_for(mock.delay);
        localState->transcribeInFlight.store(false, std::memory_order_release);
        const std::string text = mock.text;
        app->PostToMainThread([text]() {
            ImGuiContext* gctx = ::ImGui::GetCurrentContext();
            const unsigned int activeId = gctx != nullptr ? static_cast<unsigned int>(gctx->ActiveId) : 0u;
            g_dictationRouter.InsertIntoFocusedInputText(text, activeId);
            LOG_DEBUG("Whisper hotkey (mock): inserted %zu bytes of canned transcription", text.size());
            // Mirror production auto-send-on-punctuation gate — required for
            // scenario coverage of the AI Assistant hands-free chat round-trip.
            // Config is re-read here so the scenario can flip the pref between
            // press and release without restarting the plugin.
            const TrackerConfig cfgPost = ConfigManager::Load();
            const bool isAiFocus = g_dictationRouter.IsFocusedTargetAiAssistant();
            const bool endsPunct = EndsWithSentencePunctuation(text);
            if (cfgPost.WhisperAutoSendOnPunctuation && isAiFocus && endsPunct) {
                LOG_DEBUG("Whisper hotkey (mock): auto-send on punctuation triggered for AI "
                          "Assistant");
                g_dictationRouter.TriggerAiAssistantSend();
            }
            g_dictationRouter.SetTranscribing(false);
        });
    });
}

void RunHotkeyRelease_Worker(WhisperPlugin::PhaseEState* state) {
    if (state == nullptr || state->app == nullptr) {
        return;
    }
    // Mock-transcription seam — when armed, skip WASAPI drain entirely and
    // queue the canned text for insertion after the configured delay. This is
    // the lower half of the test-injection contract exposed by
    // `WhisperPlugin::SetMockTranscription`. Production code never reaches
    // this branch.
    if (WhisperPlugin::MockTranscriptionActive()) {
        RunHotkeyRelease_Mock_Worker(state);
        return;
    }

    // Only proceed if WE flipped the active flag — guards against a release
    // event for a press we never observed (rare; stuck-key recovery).
    bool wasActive = state->captureActive.exchange(false, std::memory_order_acq_rel);
    if (!wasActive) {
        LOG_DEBUG("Whisper hotkey release ignored — no active capture");
        return;
    }

    state->capture.Stop();
    std::vector<std::int16_t> pcm;
    state->capture.DrainCapturedPcm(pcm);
    g_dictationRouter.SetRecording(false);

    if (pcm.empty()) {
        LOG_INFO("Whisper hotkey: 0 PCM samples captured (release within < 1 chunk)");
        return;
    }

    bool expectedFlight = false;
    if (!state->transcribeInFlight.compare_exchange_strong(expectedFlight, true, std::memory_order_acq_rel)) {
        LOG_DEBUG("Whisper hotkey release: prior transcription still in flight; dropping new audio");
        return;
    }

    // Hand the actual transcription off to LaunchBackgroundTask so the hook
    // thread returns to the OS message pump quickly. Worker captures pcm by
    // value and `state` by raw pointer — state lives for the plugin's
    // lifetime which outlives every worker (OnStop joins).
    AppController* app = state->app;
    WhisperPlugin::PhaseEState* localState = state;
    std::vector<std::int16_t> pcmMove = std::move(pcm);
    // Tell the UI that a transcription is now in flight — the menu-bar +
    // overlay indicators flip from "REC" to "Transcribing..." until the
    // post-back below clears it. Cleared in every exit path (success,
    // failure, empty-text) so the indicator never gets stuck on.
    g_dictationRouter.SetTranscribing(true);
    app->LaunchBackgroundTask([app, localState, pcmMove]() {
        std::string text;
        std::string err;
        const bool ok = RunTranscriptionPipeline_Worker(pcmMove, text, err);
        // Clear the in-flight flag inside the worker so the next press can
        // start without waiting on the dispatcher drain.
        localState->transcribeInFlight.store(false, std::memory_order_release);
        if (!ok) {
            LOG_WARN("Whisper hotkey: transcription failed: %s", err.c_str());
            g_dictationRouter.SetTranscribing(false);
            return;
        }
        if (text.empty()) {
            LOG_INFO("Whisper hotkey: transcription returned empty string (silence?)");
            g_dictationRouter.SetTranscribing(false);
            return;
        }
        // UI-thread hand-off — the router splices into the focused InputText.
        // Phase F — if the splice target is the AI Assistant chat input AND
        // the transcription ends with sentence-final punctuation AND the
        // cfg.WhisperAutoSendOnPunctuation toggle is on, fire the AI Assistant
        // Send action so hands-free chat round-trips work. Configuration is
        // re-read on the UI thread (cheap; ConfigManager has an in-memory
        // cache) to pick up any Preferences toggle that landed between the
        // hotkey press and the transcription completing.
        app->PostToMainThread([text]() {
            ImGuiContext* gctx = ::ImGui::GetCurrentContext();
            const unsigned int activeId = gctx != nullptr ? static_cast<unsigned int>(gctx->ActiveId) : 0u;
            g_dictationRouter.InsertIntoFocusedInputText(text, activeId);
            LOG_DEBUG("Whisper hotkey: inserted %zu bytes of transcription", text.size());
            const TrackerConfig cfgPost = ConfigManager::Load();
            const bool isAiFocus = g_dictationRouter.IsFocusedTargetAiAssistant();
            const bool endsPunct = EndsWithSentencePunctuation(text);
            if (cfgPost.WhisperAutoSendOnPunctuation && isAiFocus && endsPunct) {
                LOG_DEBUG("Whisper hotkey: auto-send on punctuation triggered for AI Assistant");
                g_dictationRouter.TriggerAiAssistantSend();
            }
            // Indicator cleared here on the success path so the user sees
            // the splice land + the indicator vanish in the same frame.
            g_dictationRouter.SetTranscribing(false);
        });
    });
}

smatchet::cmd::Command BuildSimulatePressCommand(WhisperPlugin::PhaseEState* state) {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;
    using smatchet::cmd::ErrorCode;

    Command c;
    c.Name = "whisper.simulate-press";
    c.Category = "whisper";
    c.Summary = "Simulate a hotkey press for bucket-E testing (no hardware key required).";
    c.Description = "Test-helper that triggers the Phase E onPress callback directly. Does not require the "
                    "configured hotkey to be physically depressed. Pair with `whisper.simulate-release` to "
                    "exercise the full capture + transcribe + insert path without a real mic hold. Idempotent: "
                    "calling twice in a row is a no-op (the second press observes captureActive=true).";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [state](const nlohmann::json& /*args*/, const CommandContext& ctx) -> CommandResult {
        // CPP_CODE_AUDIT.md #15: this is registered AsyncSafe=true in the global command
        // registry and starts real WASAPI mic capture with no hotkey press or visible
        // prompt — reachable from the loopback MCP surface, so a local MCP client could
        // silently trigger the mic. It's a bucket-E test helper (CI drives it via CLI
        // --spawn); block the MCP source specifically rather than every non-UI source, so
        // CLI-driven CI keeps working.
        if (ctx.Source == smatchet::cmd::CommandSource::Mcp) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "whisper.simulate-press is not available over MCP.");
        }
        if (state == nullptr) {
            return CommandResult::Success(nlohmann::json{{"started", false}, {"error", "Phase E state unavailable"}});
        }
        RunHotkeyPress_Worker(state);
        nlohmann::json out;
        out["started"] = state->captureActive.load(std::memory_order_acquire);
        return CommandResult::Success(std::move(out));
    };
    return c;
}

smatchet::cmd::Command BuildSimulateReleaseCommand(WhisperPlugin::PhaseEState* state) {
    using smatchet::cmd::Command;
    using smatchet::cmd::CommandContext;
    using smatchet::cmd::CommandResult;

    Command c;
    c.Name = "whisper.simulate-release";
    c.Category = "whisper";
    c.Summary = "Simulate a hotkey release for bucket-E testing.";
    c.Description = "Test-helper that triggers the Phase E onRelease callback directly. Drains any PCM "
                    "captured since the matching `whisper.simulate-press` and runs the transcription + "
                    "insertion pipeline. Without a real mic, the captured PCM is empty and the pipeline "
                    "logs `0 PCM samples captured` then returns cleanly — useful for exercising the wire-up "
                    "without an audio device.";
    c.Destructive = false;
    c.Idempotent = true;
    c.AsyncSafe = true;
    c.DryRunSupported = false;
    c.Handler = [state](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) -> CommandResult {
        if (state == nullptr) {
            return CommandResult::Success(nlohmann::json{{"released", false}, {"error", "Phase E state unavailable"}});
        }
        RunHotkeyRelease_Worker(state);
        nlohmann::json out;
        out["released"] = true;
        return CommandResult::Success(std::move(out));
    };
    return c;
}

} // namespace

// Phase F — process-wide singleton accessor. Set inside OnStart, cleared
// inside OnStop. The Preferences UI uses this to call ReregisterHotkey
// without threading the plugin pointer through AppController. Mutating two
// callers race-free is not a concern: OnStart / OnStop run on the main
// thread, and ReregisterHotkey is invoked from the UI thread Preferences
// callback — strictly sequenced.
namespace {
WhisperPlugin* g_whisperPluginInstance = nullptr;
} // namespace

WhisperPlugin* WhisperPlugin::InstanceForUi() { return g_whisperPluginInstance; }

WhisperPlugin::WhisperPlugin() : phaseE_(std::make_unique<PhaseEState>()) {}
WhisperPlugin::~WhisperPlugin() = default;

bool WhisperPlugin::ReregisterHotkey(const std::string& descriptor, std::string& outError) {
    outError.clear();
    if (!phaseE_) {
        outError = "WhisperPlugin not started yet";
        return false;
    }
    smatchet::whisper::hotkey::Hotkey hk;
    std::string parseErr;
    if (!smatchet::whisper::hotkey::Parse(descriptor, hk, parseErr)) {
        outError = std::string("parse: ") + parseErr;
        return false;
    }
    // Tear down the existing hook before installing the new one — the Win32
    // global-hotkey hook owns a thread + message pump, so re-Register-ing
    // without Unregister-ing first leaks the prior thread and leaves the
    // stale combo firing. Unregister is idempotent.
    phaseE_->hotkey.Unregister();
    PhaseEState* state = phaseE_.get();
    std::string regErr;
    const bool ok = phaseE_->hotkey.Register(
        hk, [state]() { RunHotkeyPress_Worker(state); }, [state]() { RunHotkeyRelease_Worker(state); }, regErr);
    if (!ok) {
        outError = std::string("register: ") + regErr;
        LOG_WARN("WhisperPlugin::ReregisterHotkey: failed to re-install hotkey '%s': %s", descriptor.c_str(),
                 regErr.c_str());
        return false;
    }
    LOG_INFO("WhisperPlugin::ReregisterHotkey: hotkey re-registered as '%s'", descriptor.c_str());
    return true;
}

void WhisperPlugin::OnStart(AppController& app) {
    if (!phaseE_) {
        phaseE_ = std::make_unique<PhaseEState>();
    }
    phaseE_->app = &app;
    g_whisperPluginInstance = this;

    smatchet::cmd::CommandRegistry& reg = app.Commands();
    reg.Register(BuildStatusCommand(phaseE_.get()));
    reg.Register(BuildTranscribeOnceCommand());
    reg.Register(BuildDownloadModelCommand());
    reg.Register(BuildModelProgressCommand());
    reg.Register(BuildCancelDownloadCommand());
    reg.Register(BuildSimulatePressCommand(phaseE_.get()));
    reg.Register(BuildSimulateReleaseCommand(phaseE_.get()));

    // Phase E — install the push-to-talk hotkey. Parse failure / register
    // failure both leave the plugin running so the CLI / palette flow still
    // works; only the hardware-key path is lost.
    const TrackerConfig cfg = ConfigManager::Load();
    smatchet::whisper::hotkey::Hotkey hk;
    std::string parseErr;
    if (smatchet::whisper::hotkey::Parse(cfg.WhisperHotkey, hk, parseErr)) {
        PhaseEState* state = phaseE_.get();
        std::string regErr;
        const bool ok = phaseE_->hotkey.Register(
            hk,
            [state]() {
                // Hook thread — must not touch ImGui. Worker routines below
                // are pure C++ / WASAPI; UI updates are routed via the
                // dictation router atomic flag (polled on the UI thread).
                RunHotkeyPress_Worker(state);
            },
            [state]() { RunHotkeyRelease_Worker(state); }, regErr);
        if (!ok) {
            LOG_WARN("WhisperPlugin: global hotkey '%s' registration failed: %s — push-to-talk "
                     "disabled (CLI / palette commands still work)",
                     cfg.WhisperHotkey.c_str(), regErr.c_str());
        } else {
            LOG_INFO("WhisperPlugin: global hotkey '%s' registered.", cfg.WhisperHotkey.c_str());
        }
    } else {
        LOG_WARN("WhisperPlugin: hotkey descriptor '%s' failed to parse: %s — push-to-talk disabled",
                 cfg.WhisperHotkey.c_str(), parseErr.c_str());
    }

    LOG_INFO("WhisperPlugin: Phase E started; whisper.{status,transcribe-once,download-model,"
             "model-progress,cancel-download,simulate-press,simulate-release} registered "
             "(local backend %s).",
             smatchet::whisper::WhisperLocal::BackendBuildState());
}

void WhisperPlugin::OnStop() {
    if (phaseE_) {
        // Tear down the hotkey FIRST — the hook thread must stop before the
        // capture instance is destroyed, otherwise a late hook fire could
        // touch a half-destructed WindowsAudioCapture.
        phaseE_->hotkey.Unregister();
        // Drain any active capture so the router's recording flag clears
        // before SmatchetUI's last frame.
        if (phaseE_->captureActive.exchange(false, std::memory_order_acq_rel)) {
            phaseE_->capture.Stop();
            std::vector<std::int16_t> drained;
            phaseE_->capture.DrainCapturedPcm(drained);
        }
        g_dictationRouter.SetRecording(false);
        // Clear the in-flight transcription indicator too. If a release worker
        // is mid-pipeline when the plugin tears down (shutdown / hot-reload),
        // its post-back will never run; the menu bar would otherwise stay on
        // the amber "Transcribing..." label forever.
        g_dictationRouter.SetTranscribing(false);
        phaseE_->app = nullptr;
    }
    if (g_whisperPluginInstance == this) {
        g_whisperPluginInstance = nullptr;
    }
    LOG_INFO("WhisperPlugin: stopped.");
}
