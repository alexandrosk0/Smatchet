#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#if defined(SMATCHET_WITH_WHISPER)

#include "SmatchetPreferencesUi_detail.h"
#include "Ui/SmatchetSecretInput.h"
#include "Commands/IAppThreading.h"
#include "HotkeyParse.h"
#include "Logger.h"
#include "ModelCatalog.h"
#include "ModelDownloader.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetWhisperSetupBanner.h"
#include "SmatchetUiSession.h"
#include "WavWriter.h"
#include "WhisperApiClient.h"
#include "WhisperApiKeyResolve.h"
#include "WhisperConsentGate.h"
#include "WhisperLocal.h"
#include "WhisperPlugin.h"
#include "WindowsAudioCapture.h"
#include <cpr/cpr.h>

#include "Ui/SmatchetIconButtons.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Persistent per-tab state, hoisted out of function-local statics so the draw
// body stays layout-only and the section helpers share one owner.
struct WhisperPrefsTabState {
    // Hotkey capture.
    bool capturing = false;
    std::string hotkeyError;
    char hotkeyDisplay[64] = {0};
    bool hotkeyDisplaySeeded = false;

    // API key input buffer.
    char keyBuf[512] = {0};
    bool keyBufSeeded = false;

    // Test-connection probe.
    std::atomic<bool> testInFlight{false};
    std::string testResult; // empty | success | failure
    int testResultType = 0; // 0=neutral, 1=ok, 2=fail

    // Test-microphone probe.
    std::atomic<bool> micTestInFlight{false};
    std::string micTestResult;
    int micTestResultType = 0; // 0=neutral, 1=ok, 2=fail

    // Test-end-to-end probe.
    std::atomic<bool> e2eInFlight{false};
    std::string e2eResult;
    int e2eResultType = 0;
};

void DrawWhisperEnableAndMode(UiDrawSession& d) {
    if (ImGui::Checkbox(SmatchetLocalization::T("whisper.preferences.enableToggle", "Enable voice dictation"),
                        &d.cfg.WhisperEnabled)) {
        if (d.cfg.WhisperEnabled) {
            d.cfg.WhisperSetupCompleted = true;
            d.cfg.WhisperSetupChoice = "enabled";
        } else {
            d.cfg.WhisperSetupChoice = "disabled";
        }
        MarkPrefsDirty(d);
    }

    // Mode selector.
    {
        int modeIdx = 0;
        if (d.cfg.WhisperMode == "local") {
            modeIdx = 1;
        } else if (d.cfg.WhisperMode == "cloud") {
            modeIdx = 2;
        }
        const char* labels[3] = {
            SmatchetLocalization::T("whisper.preferences.modeAuto", "Auto (local if present, cloud fallback)"),
            SmatchetLocalization::T("whisper.preferences.modeLocal", "Local only (no network)"),
            SmatchetLocalization::T("whisper.preferences.modeCloud", "Cloud only (OpenAI)")};
        if (ImGui::Combo("Mode", &modeIdx, labels, 3)) {
            d.cfg.WhisperMode = (modeIdx == 1) ? "local" : (modeIdx == 2) ? "cloud" : "auto";
            MarkPrefsDirty(d);
        }
    }
}

// Persist the Recommended default the combo displays when no model is
// configured, so the presence check + Download button act on the model the
// user sees.
void SeedDefaultWhisperModel(UiDrawSession& d, const std::vector<smatchet::whisper::catalog::Entry>& catalog) {
    // Deliberate: seeds directly into the live d.cfg (not a working copy) and marks
    // dirty, so opening the Whisper tab with an empty model triggers a debounced
    // persist with no explicit user action. This differs from the Assistant tab's
    // working-copy pattern by design — the picker must show and use a concrete model.
    if (d.cfg.WhisperModel.empty() && catalog.size() > 1) {
        d.cfg.WhisperModel = catalog[1].Id;
        MarkPrefsDirty(d);
    }
}

void DrawWhisperModelPicker(IAppThreading& app, UiDrawSession& d) {
    // Model picker + download button.
    const std::string sharedDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    std::string modelDir;
    if (!sharedDir.empty()) {
        modelDir = sharedDir;
        if (!modelDir.empty() && modelDir.back() != '/' && modelDir.back() != '\\') {
            modelDir.push_back('/');
        }
        modelDir += "whisper";
    }
    const auto& catalog = smatchet::whisper::catalog::All();
    SeedDefaultWhisperModel(d, catalog);
    int selIdx = 1; // default Recommended
    const auto selectedModelIt =
        std::find_if(catalog.begin(), catalog.end(),
                     [&](const smatchet::whisper::catalog::Entry& entry) { return entry.Id == d.cfg.WhisperModel; });
    if (selectedModelIt != catalog.end()) {
        selIdx = static_cast<int>(std::distance(catalog.begin(), selectedModelIt));
    }
    std::vector<std::string> labelStorage;
    std::vector<const char*> labelPtrs;
    labelStorage.reserve(catalog.size());
    for (std::size_t i = 0; i < catalog.size(); ++i) {
        std::string lbl = catalog[i].DisplayName + " (" + catalog[i].Id + ")";
        if (smatchet::whisper::catalog::IsModelPresent(catalog[i].Id, modelDir)) {
            lbl += " ";
            lbl += SmatchetLocalization::T("whisper.preferences.modelPresent", "(installed)");
        }
        labelStorage.push_back(std::move(lbl));
    }
    labelPtrs.reserve(labelStorage.size());
    std::transform(labelStorage.begin(), labelStorage.end(), std::back_inserter(labelPtrs),
                   [](const std::string& s) { return s.c_str(); });
    if (ImGui::Combo(SmatchetLocalization::T("whisper.preferences.model", "Speech model"), &selIdx, labelPtrs.data(),
                     static_cast<int>(labelPtrs.size()))) {
        if (selIdx >= 0 && static_cast<std::size_t>(selIdx) < catalog.size()) {
            d.cfg.WhisperModel = catalog[static_cast<std::size_t>(selIdx)].Id;
            MarkPrefsDirty(d);
        }
    }

    smatchet::whisper::ModelDownloader& dl = smatchet::whisper::banner::BannerOwnedDownloader();
    const auto prog = dl.GetProgress();
    const bool currentlyDownloading = (prog.state == smatchet::whisper::ModelDownloader::State::Downloading) ||
                                      (prog.state == smatchet::whisper::ModelDownloader::State::Verifying);
    const bool modelPresent = smatchet::whisper::catalog::IsModelPresent(d.cfg.WhisperModel, modelDir);

    ImGui::BeginDisabled(modelPresent || currentlyDownloading || modelDir.empty());
    if (ImGui::Button(SmatchetLocalization::T("whisper.preferences.downloadModel", "Download model"))) {
        // Stamp fresh consent for the gate before the worker reads it.
        d.cfg.WhisperConsentTimestampSec = smatchet::whisper::consent::NowEpochSec();
        ConfigManager::Save(d.cfg);
        std::string err;
        if (!dl.Start(app, d.cfg.WhisperModel, modelDir, err)) {
            LOG_WARN("Whisper preferences: download dispatch failed: %s", err.c_str());
        }
    }
    ImGui::EndDisabled();
    if (currentlyDownloading) {
        ImGui::SameLine();
        if (ImGui::Button(SmatchetLocalization::T("whisper.preferences.cancelDownload", "Cancel download"))) {
            dl.Cancel();
        }
        const float frac =
            (prog.bytesExpected > 0)
                ? (std::min)(1.0f, static_cast<float>(prog.bytesReceived) / static_cast<float>(prog.bytesExpected))
                : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(-1, 0));
    }
    if (!prog.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", prog.error.c_str());
    }
}

// Walk the small supported non-modifier key set and return the first VK pressed
// this frame, or 0 if none. Keeps the capture loop out of the draw body.
unsigned int CaptureHotkeyVkThisFrame() {
    unsigned int capturedVk = 0;
    // Letters A..Z map ImGuiKey_A..ImGuiKey_Z to ASCII VKs.
    for (int k = 0; k < 26 && capturedVk == 0; ++k) {
        const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_A + k);
        if (ImGui::IsKeyPressed(ik, false)) {
            capturedVk = static_cast<unsigned int>('A' + k);
        }
    }
    // Digits 0..9.
    for (int k = 0; k < 10 && capturedVk == 0; ++k) {
        const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_0 + k);
        if (ImGui::IsKeyPressed(ik, false)) {
            capturedVk = static_cast<unsigned int>('0' + k);
        }
    }
    // Function keys F1..F12 (ImGui exposes up to F24 in some builds, but
    // F1..F12 covers the realistic rebind set).
    for (int k = 0; k < 12 && capturedVk == 0; ++k) {
        const ImGuiKey ik = static_cast<ImGuiKey>(ImGuiKey_F1 + k);
        if (ImGui::IsKeyPressed(ik, false)) {
            capturedVk = smatchet::whisper::hotkey::vk::kF1 + static_cast<unsigned int>(k);
        }
    }
    if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        capturedVk = smatchet::whisper::hotkey::vk::kSpace;
    }
    if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        capturedVk = smatchet::whisper::hotkey::vk::kTab;
    }
    if (capturedVk == 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        capturedVk = smatchet::whisper::hotkey::vk::kEnter;
    }
    return capturedVk;
}

// Commit a captured VK + the current modifier state into cfg.WhisperHotkey and
// hot-rebind the live Win32 hook. Updates state.hotkeyError / hotkeyDisplay.
void CommitCapturedHotkey(UiDrawSession& d, WhisperPrefsTabState& state, unsigned int capturedVk) {
    smatchet::whisper::hotkey::Hotkey hk;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl)
        hk.mods |= smatchet::whisper::hotkey::mod::kControl;
    if (io.KeyAlt)
        hk.mods |= smatchet::whisper::hotkey::mod::kAlt;
    if (io.KeyShift)
        hk.mods |= smatchet::whisper::hotkey::mod::kShift;
    if (io.KeySuper)
        hk.mods |= smatchet::whisper::hotkey::mod::kWin;
    hk.vk = capturedVk;

    // Reject combos with no modifier — a bare key is a global hotkey landmine.
    if (hk.mods == 0) {
        state.hotkeyError = SmatchetLocalization::T("whisper.preferences.hotkeyErrorModifiersOnly",
                                                    "Hotkey must include a modifier key (Ctrl, Alt, Shift, or Win)");
        state.capturing = false;
        return;
    }
    const std::string newDescriptor = smatchet::whisper::hotkey::Stringify(hk);
    if (newDescriptor.empty()) {
        state.hotkeyError =
            SmatchetLocalization::T("whisper.preferences.hotkeyErrorParse", "Could not parse the captured key combo");
        state.capturing = false;
        return;
    }
    d.cfg.WhisperHotkey = newDescriptor;
    MarkPrefsDirty(d);
    std::snprintf(state.hotkeyDisplay, sizeof(state.hotkeyDisplay), "%s", newDescriptor.c_str());
    state.hotkeyError.clear();
    // Phase F — live hot-rebind: re-register the Win32 global hook against the
    // new descriptor immediately so users don't need to restart. On
    // register-failure surface the error inline (the previous hook is already
    // torn down by ReregisterHotkey on its way through; user is left without
    // push-to-talk until they pick a working combo or restart).
    WhisperPlugin* plug = WhisperPlugin::InstanceForUi();
    if (plug != nullptr) {
        std::string rebindErr;
        if (!plug->ReregisterHotkey(newDescriptor, rebindErr)) {
            state.hotkeyError = std::string(SmatchetLocalization::T("whisper.preferences.hotkeyErrorRebind",
                                                                    "Hotkey rebind failed: ")) +
                                rebindErr;
            LOG_WARN("Whisper preferences: hot rebind failed: %s", rebindErr.c_str());
        } else {
            LOG_INFO("Whisper preferences: hot rebind applied to '%s'", newDescriptor.c_str());
        }
    }
    state.capturing = false;
}

void DrawWhisperHotkey(UiDrawSession& d, WhisperPrefsTabState& state) {
    // Hotkey display + Phase E rebind. Button label flips to the
    // capturing-prompt while the user is binding a new combo. Esc cancels.
    // Capture completes on the first non-modifier key press, at which point we
    // snapshot the modifier state, Stringify, and persist. Re-registration of
    // the global hotkey happens on the next plugin OnStop/OnStart cycle (or at
    // app restart) — Phase E does not hot-rebind the live hook to keep the
    // surface tight.
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.hotkey", "Push-to-talk hotkey"));
    if (!state.hotkeyDisplaySeeded || std::strcmp(state.hotkeyDisplay, d.cfg.WhisperHotkey.c_str()) != 0) {
        std::snprintf(state.hotkeyDisplay, sizeof(state.hotkeyDisplay), "%s", d.cfg.WhisperHotkey.c_str());
        state.hotkeyDisplaySeeded = true;
    }

    if (!state.capturing) {
        ImGui::TextDisabled("%s", state.hotkeyDisplay);
        ImGui::SameLine();
        if (ImGui::SmallButton(SmatchetLocalization::T("whisper.preferences.hotkeyRebindButton", "Click to rebind"))) {
            state.capturing = true;
            state.hotkeyError.clear();
        }
    } else {
        ImGui::TextColored(
            ImVec4(0.95f, 0.85f, 0.30f, 1.0f), "%s",
            SmatchetLocalization::T("whisper.preferences.hotkeyCapturing", "Press a key combo... (Esc to cancel)"));

        // Esc cancels capture without clobbering the existing key.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.capturing = false;
            state.hotkeyError.clear();
        } else {
            const unsigned int capturedVk = CaptureHotkeyVkThisFrame();
            if (capturedVk != 0) {
                CommitCapturedHotkey(d, state, capturedVk);
            }
        }
    }
    if (!state.hotkeyError.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s", state.hotkeyError.c_str());
    }
}

void DrawWhisperApiKey(UiDrawSession& d, WhisperPrefsTabState& state) {
    // API key — passworded input, with fallback hint when empty + AiProvider=openai.
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.apiKey", "OpenAI API key (cloud mode)"));
    if (!state.keyBufSeeded) {
        std::snprintf(state.keyBuf, sizeof(state.keyBuf), "%s", d.cfg.WhisperApiKey.c_str());
        state.keyBufSeeded = true;
    }
    if (SmatchetSecretInputText("##WhisperApiKey", state.keyBuf, sizeof(state.keyBuf))) {
        d.cfg.WhisperApiKey = state.keyBuf;
        MarkPrefsDirty(d);
    }
    if (d.cfg.WhisperApiKey.empty() && d.cfg.AiProviderKind == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.preferences.apiKeyFallback",
                                                          "(uses AI Assistant OpenAI key when empty)"));
    }
}

void DrawWhisperTestConnection(IAppThreading& app, UiDrawSession& d, WhisperPrefsTabState& state) {
    // Phase F — Test connection button. Dispatches a tiny worker that hits
    // OpenAI's /v1/models with the resolved key (via WhisperApiKeyResolve::Resolve
    // fallback rule) and reports success/failure inline. Mirrors the AI Assistant
    // tab's pattern (LaunchBackgroundTask + PostToMainThread post-back); the
    // status string lives in tab state so the result survives the user navigating
    // other tabs while the probe runs.
    const bool inFlight = state.testInFlight.load(std::memory_order_acquire);
    if (inFlight) {
        ImGui::BeginDisabled(true);
    }
    if (ImGui::Button(SmatchetLocalization::T("whisper.preferences.apiKey.testButton", "Test connection"))) {
        // Resolve the key with the same 5-row fallback the runtime uses
        // (WhisperApiKey > AiApiKey when provider=openai).
        const std::string providerStr = (d.cfg.AiProviderKind == 0) ? std::string("openai") : std::string("anthropic");
        const std::string resolvedKey =
            smatchet::whisper::pure::ResolveWhisperApiKey(d.cfg.WhisperApiKey, providerStr, d.cfg.AiApiKey);
        if (resolvedKey.empty()) {
            state.testResult =
                std::string(SmatchetLocalization::T("whisper.preferences.testConnection.failure", "x ")) +
                "no API key configured";
            state.testResultType = 2;
        } else {
            state.testInFlight.store(true, std::memory_order_release);
            state.testResult = "Testing...";
            state.testResultType = 0;
            WhisperPrefsTabState* st = &state;
            // Worker — minimal GET to /v1/models. cpr is already linked; reuse
            // WhisperApiClient's transport idioms (5 s connect, 15 s total) so a
            // hung DNS doesn't freeze the result for a minute. Joined
            // background-task pool, not a raw detached thread — the no-detach lint
            // forbids that. Joined at shutdown so &app stays valid.
            app.LaunchBackgroundTask([resolvedKey, &app, st]() {
                std::string okMsg;
                std::string errMsg;
                try {
                    /* PILLAR2_WORKER_ONLY */ // est-latency: ~15000ms — enclosing background task
                                              // (LaunchBackgroundTask); 15s total timeout.
                    cpr::Response r = cpr::Get(cpr::Url{"https://api.openai.com/v1/models"},
                                               cpr::Header{{"Authorization", std::string("Bearer ") + resolvedKey}},
                                               cpr::ConnectTimeout{5000}, cpr::Timeout{15000});
                    if (r.error.code != cpr::ErrorCode::OK) {
                        errMsg = std::string("transport: ") + r.error.message;
                    } else if (r.status_code < 200 || r.status_code >= 300) {
                        errMsg = std::string("HTTP ") + std::to_string(r.status_code);
                    } else {
                        okMsg = "Connected.";
                    }
                    // SMATCHET_DEVIATION(rule=duplication; reason=same localized internal-error catch shape as the
                    // Assistant test-connection twin — surface-specific keys, no shared seam worth coupling two
                    // Preferences tabs for; owner=user-text-error-pass; revisit=2026-09-30)
                } catch (const std::exception& ex) {
                    LOG_WARN("Whisper test-connection: %s", ex.what());
                    errMsg =
                        SmatchetLocalization::T("prefs.whisper.test_internal_error",
                                                "the request could not be completed — check the API key and try again");
                } catch (...) {
                    LOG_WARN("Whisper test-connection: unknown exception");
                    errMsg =
                        SmatchetLocalization::T("prefs.whisper.test_internal_error",
                                                "the request could not be completed — check the API key and try again");
                }
                app.PostToMainThread([okMsg, errMsg, st]() {
                    st->testInFlight.store(false, std::memory_order_release);
                    if (errMsg.empty()) {
                        st->testResult = std::string(SmatchetLocalization::T(
                                             "whisper.preferences.testConnection.success", "Connected")) +
                                         " (" + okMsg + ")";
                        st->testResultType = 1;
                    } else {
                        st->testResult = std::string(SmatchetLocalization::T(
                                             "whisper.preferences.testConnection.failure", "Failed: ")) +
                                         errMsg;
                        st->testResultType = 2;
                    }
                });
            });
        }
    }
    if (inFlight) {
        ImGui::EndDisabled();
    }
    if (!state.testResult.empty()) {
        ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
        if (state.testResultType == 1) {
            col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
        } else if (state.testResultType == 2) {
            col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
        }
        ImGui::SameLine();
        ImGui::TextColored(col, "%s", state.testResult.c_str());
    }
}

// Background worker for the mic capture-smoke test: capture 3 s via WindowsAudioCapture, measure
// peak amplitude, and post a classified result string back to the UI thread. Extracted from the
// LaunchBackgroundTask lambda in DrawWhisperTestMicrophone (over-100-line decomposition); identical.
void RunMicCaptureTestWorker(IAppThreading& app, WhisperPrefsTabState* st) {
    smatchet::whisper::WindowsAudioCapture cap;
    std::string startErr;
    if (!cap.Start(startErr)) {
        const std::string err = startErr.empty() ? std::string("capture start failed") : startErr;
        app.PostToMainThread([err, st]() {
            st->micTestInFlight.store(false, std::memory_order_release);
            st->micTestResult = std::string("Failed: ") + err;
            st->micTestResultType = 2;
        });
        return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    cap.Stop();
    std::vector<std::int16_t> pcm;
    cap.DrainCapturedPcm(pcm);
    std::int32_t peakAbs = 0;
    for (std::int16_t s : pcm) {
        const std::int32_t m = s >= 0 ? static_cast<std::int32_t>(s) : -static_cast<std::int32_t>(s);
        if (m > peakAbs) {
            peakAbs = m;
        }
    }
    const float peakNorm = static_cast<float>(peakAbs) / 32768.0f;
    const std::size_t samples = pcm.size();
    app.PostToMainThread([samples, peakAbs, peakNorm, st]() {
        st->micTestInFlight.store(false, std::memory_order_release);
        char buf[256];
        if (samples == 0) {
            std::snprintf(buf, sizeof(buf),
                          "Failed: 0 samples captured (mic unplugged / muted / "
                          "consent denied)");
            st->micTestResultType = 2;
        } else if (peakAbs == 0) {
            std::snprintf(buf, sizeof(buf),
                          "Failed: %zu samples but peak=0 (format unwrap broken or mic "
                          "muted at hardware)",
                          samples);
            st->micTestResultType = 2;
        } else if (peakNorm < 0.01f) {
            std::snprintf(buf, sizeof(buf),
                          "Warn: %zu samples, peak=%.3f (very quiet — speak "
                          "louder or check mic gain)",
                          samples, peakNorm);
            st->micTestResultType = 2;
        } else {
            std::snprintf(buf, sizeof(buf),
                          "OK: %zu samples, peak=%.3f (ready for "
                          "transcription)",
                          samples, peakNorm);
            st->micTestResultType = 1;
        }
        st->micTestResult = buf;
    });
}

void DrawWhisperTestMicrophone(IAppThreading& app, WhisperPrefsTabState& state) {
    // --- Test microphone end-to-end (capture-only). Captures 3 s of audio via
    // WindowsAudioCapture, reports total samples + peak amplitude inline so the
    // user can confirm the mic + format path is producing real samples BEFORE
    // saving + relying on it for transcription. No HTTP, no transcription — pure
    // capture smoke. Surfaces silent-format regressions (WAVE_FORMAT_EXTENSIBLE
    // unwrap, EAX-suppressed mic, muted OS capture, etc.) without burning a
    // Whisper API call.
    const bool inFlight = state.micTestInFlight.load(std::memory_order_acquire);
    if (inFlight) {
        ImGui::BeginDisabled(true);
    }
    const char* micSpinnerGlyph = "|";
    if (inFlight) {
        const int slot = static_cast<int>(::ImGui::GetTime() * 8.0) & 3;
        micSpinnerGlyph = (slot == 0) ? "|" : (slot == 1) ? "/" : (slot == 2) ? "-" : "\\";
    }
    char micLabel[128];
    std::snprintf(micLabel, sizeof(micLabel), inFlight ? "%s  %s" : "%s",
                  SmatchetLocalization::T("whisper.preferences.testMic.button", "Test microphone (3 s)"),
                  micSpinnerGlyph);
    if (ImGui::Button(micLabel)) {
        state.micTestInFlight.store(true, std::memory_order_release);
        state.micTestResult = "Capturing...";
        state.micTestResultType = 0;
        WhisperPrefsTabState* st = &state;
        // Use the app-owned background task pool instead of a raw detached
        // thread so AppController::JoinBackgroundTasks can join us at shutdown.
        // PostToMainThread is itself shutdown-safe (no-ops once the dispatcher's
        // shuttingDown_ atomic flips), but the worker thread dangling on a dead
        // dispatcher would still UB on the captured-by-reference access — the
        // LaunchBackgroundTask contract eliminates both halves.
        app.LaunchBackgroundTask([&app, st]() { RunMicCaptureTestWorker(app, st); });
    }
    if (inFlight) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(captures 3 s; reports sample count + peak)");
    if (!state.micTestResult.empty()) {
        ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
        if (state.micTestResultType == 1)
            col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
        if (state.micTestResultType == 2)
            col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
        ImGui::TextColored(col, "%s", state.micTestResult.c_str());
    }
}

// Resolved E2E route. effectiveMode is "cloud"/"local" when usable; fastFail
// carries a user-facing reason string when the route can't run (mutually
// exclusive with effectiveMode).
struct WhisperE2ERoute {
    std::string modelDir;
    std::string resolvedKey;
    std::string effectiveMode;
    std::string fastFail;
};

// Pure route resolution — no ImGui, no capture. Mirrors ResolveWhisperModelDir
// from WhisperPlugin.cpp (anon namespace; inline here so Preferences stays
// self-contained). Pick effective route per the user's spec:
//   - cloud: require key
//   - local: require model file present
//   - auto:  prefer local when present, fall back to cloud (per the mode label)
WhisperE2ERoute ResolveE2ERoute(const TrackerConfig& cfgSnap) {
    WhisperE2ERoute route;
    const std::string requestedMode = cfgSnap.WhisperMode.empty() ? std::string("auto") : cfgSnap.WhisperMode;
    const std::string providerStr = (cfgSnap.AiProviderKind == 0) ? std::string("openai") : std::string("anthropic");
    route.resolvedKey =
        smatchet::whisper::pure::ResolveWhisperApiKey(cfgSnap.WhisperApiKey, providerStr, cfgSnap.AiApiKey);
    route.modelDir = ConfigManager::GetPlatformSharedUserDataDirectory();
    if (!route.modelDir.empty() && route.modelDir.back() != '/' && route.modelDir.back() != '\\') {
        route.modelDir.push_back('/');
    }
    route.modelDir += "whisper";
    const bool localPresent = !cfgSnap.WhisperModel.empty() &&
                              smatchet::whisper::catalog::IsModelPresent(cfgSnap.WhisperModel, route.modelDir);

    if (requestedMode == "cloud") {
        if (route.resolvedKey.empty()) {
            route.fastFail = "Cloud mode requires an API key. Set Whisper or AI API key "
                             "(provider=openai) and retry.";
        } else {
            route.effectiveMode = "cloud";
        }
    } else if (requestedMode == "local") {
        if (!localPresent) {
            route.fastFail = "Local mode requires the selected model on disk. Open the "
                             "model picker above and click Download first.";
        } else {
            route.effectiveMode = "local";
        }
    } else { // auto
        if (localPresent) {
            route.effectiveMode = "local";
        } else if (!route.resolvedKey.empty()) {
            route.effectiveMode = "cloud";
        } else {
            route.fastFail = "Auto mode needs either an API key (for cloud) or a "
                             "downloaded local model. Neither was found.";
        }
    }
    return route;
}

// Pure transcription — runs the cloud or local backend over captured PCM. No
// ImGui, no capture. Returns true on success; fills text / txErr.
bool RunE2ETranscription(const WhisperE2ERoute& route, const TrackerConfig& cfgSnap,
                         const std::vector<std::int16_t>& pcm, std::string& text, std::string& txErr) {
    const std::string lang = cfgSnap.WhisperLanguage.empty() ? std::string("en") : cfgSnap.WhisperLanguage;
    if (route.effectiveMode == "cloud") {
        // Encode + POST.
        const std::vector<std::uint8_t> wav =
            smatchet::whisper::pure::EncodeWav(pcm, smatchet::whisper::WindowsAudioCapture::kCaptureSampleRate, 1);
        smatchet::whisper::WhisperApiClient client;
        return client.Transcribe(wav, route.resolvedKey, lang, text, txErr);
    }
    // Local — WhisperLocal::LoadModel + Transcribe on the int16 overload
    // (matches the worker pipeline path). When SMATCHET_WHISPER_LOCAL_BACKEND=OFF
    // the helper returns "local backend not built", which we surface verbatim so
    // the user knows to flip the sub-option.
    const std::string modelPath = route.modelDir + "/" + cfgSnap.WhisperModel + ".bin";
    smatchet::whisper::WhisperLocal local;
    std::string loadErr;
    if (!local.LoadModel(modelPath, loadErr)) {
        txErr = std::string("LoadModel: ") + loadErr;
        return false;
    }
    return local.Transcribe(pcm, lang, text, txErr);
}

// Background worker for the end-to-end transcription test: resolve route (fast-fail before capture),
// capture 4 s, run the full transcription pipeline, and post per-phase + final classified status back
// to the UI thread. Extracted from the LaunchBackgroundTask lambda in DrawWhisperTestE2E
// (over-100-line decomposition); behaviour-identical.
void RunE2ETestWorker(IAppThreading& app, const TrackerConfig& cfgSnap, WhisperPrefsTabState* st) {
    // --- Resolve route BEFORE spending 4 s capturing, so cloud-only with
    // no key / local-only with no model fail fast.
    const WhisperE2ERoute route = ResolveE2ERoute(cfgSnap);
    const std::string effectiveMode = route.effectiveMode;
    if (!route.fastFail.empty()) {
        const std::string fastFail = route.fastFail;
        app.PostToMainThread([fastFail, st]() {
            st->e2eInFlight.store(false, std::memory_order_release);
            st->e2eResult = std::string("Failed: ") + fastFail;
            st->e2eResultType = 2;
        });
        return;
    }

    // --- Capture once, route by effectiveMode below. Per-phase status
    // updates keep the user informed — local-model transcription on
    // medium.en can take 5+ seconds, and the "button disabled, nothing
    // visible" silence was confusing.
    const std::string modeForUi = effectiveMode;
    app.PostToMainThread([modeForUi, st]() {
        st->e2eResult = std::string("[1/3] Capturing 4 s (route=") + modeForUi + ") — speak now...";
        st->e2eResultType = 0;
    });
    smatchet::whisper::WindowsAudioCapture cap;
    std::string err;
    if (!cap.Start(err)) {
        const std::string e = err.empty() ? std::string("capture start failed") : err;
        app.PostToMainThread([e, st]() {
            st->e2eInFlight.store(false, std::memory_order_release);
            st->e2eResult = std::string("Failed (capture): ") + e;
            st->e2eResultType = 2;
        });
        return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(4));
    cap.Stop();
    std::vector<std::int16_t> pcm;
    cap.DrainCapturedPcm(pcm);
    app.PostToMainThread([modeForUi, st]() {
        st->e2eResult = std::string("[2/3] Captured; transcribing via ") + modeForUi + "...";
        st->e2eResultType = 0;
    });
    if (pcm.empty()) {
        app.PostToMainThread([st]() {
            st->e2eInFlight.store(false, std::memory_order_release);
            st->e2eResult = "Failed: 0 PCM samples (mic / consent / format issue — "
                            "click Test microphone for narrower diagnosis)";
            st->e2eResultType = 2;
        });
        return;
    }

    const std::size_t samples = pcm.size();
    std::string text;
    std::string txErr;
    const bool ok = RunE2ETranscription(route, cfgSnap, pcm, text, txErr);

    const std::string mode = effectiveMode;
    app.PostToMainThread([ok, txErr, text, samples, mode, st]() {
        st->e2eInFlight.store(false, std::memory_order_release);
        char buf[1024];
        if (!ok) {
            std::snprintf(buf, sizeof(buf),
                          "[3/3] Failed (%s, transcribe): captured %zu "
                          "samples; %s",
                          mode.c_str(), samples, txErr.c_str());
            st->e2eResultType = 2;
        } else if (text.empty()) {
            std::snprintf(buf, sizeof(buf),
                          "[3/3] Warn (%s): captured %zu samples + "
                          "transcribe ok, but empty result (silence at "
                          "recogniser)",
                          mode.c_str(), samples);
            st->e2eResultType = 2;
        } else {
            std::snprintf(buf, sizeof(buf), "[3/3] OK (%s, %zu samples): \"%s\"", mode.c_str(), samples, text.c_str());
            st->e2eResultType = 1;
        }
        st->e2eResult = buf;
    });
}

void DrawWhisperTestE2E(IAppThreading& app, UiDrawSession& d, WhisperPrefsTabState& state) {
    // --- Test transcription end-to-end. Captures 4 s, runs the full
    // transcription pipeline — silence trim, mode router, cloud or local — and
    // surfaces the resulting text inline. The first end-to-end "does this
    // actually work" button — bridges the gap between "Test connection" (HTTP key
    // probe only) and "hold hotkey + speak" (no feedback if it failed silently).
    const bool inFlight = state.e2eInFlight.load(std::memory_order_acquire);
    if (inFlight) {
        ImGui::BeginDisabled(true);
    }
    // Cheap rotating glyph spinner so the disabled-button state doesn't look
    // frozen during the multi-second local-model transcribe pipeline.
    // ImGui::GetTime() is in seconds; pick one of four glyphs per ~120 ms.
    const char* spinnerGlyph = "|";
    if (inFlight) {
        const int slot = static_cast<int>(::ImGui::GetTime() * 8.0) & 3;
        spinnerGlyph = (slot == 0) ? "|" : (slot == 1) ? "/" : (slot == 2) ? "-" : "\\";
    }
    char e2eLabel[128];
    std::snprintf(
        e2eLabel, sizeof(e2eLabel), inFlight ? "%s  %s" : "%s",
        SmatchetLocalization::T("whisper.preferences.testE2E.button", "Test end-to-end (capture 4 s + transcribe)"),
        spinnerGlyph);
    if (ImGui::Button(e2eLabel)) {
        state.e2eInFlight.store(true, std::memory_order_release);
        state.e2eResult = "Recording 4 s — speak now...";
        state.e2eResultType = 0;
        TrackerConfig cfgSnap = d.cfg;
        WhisperPrefsTabState* st = &state;
        // App-owned background task (see Test microphone block above for
        // rationale): JoinBackgroundTasks at shutdown waits for the worker, and
        // PostToMainThread is shutdown-safe via the dispatcher's shuttingDown_
        // atomic. No more dangling-reference UAF.
        app.LaunchBackgroundTask([cfgSnap, &app, st]() { RunE2ETestWorker(app, cfgSnap, st); });
    }
    if (inFlight) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.preferences.testE2E.hint",
                                                      "(records 4 s; the cloud route uploads audio to OpenAI, "
                                                      "the local route stays on-device)"));
    if (!state.e2eResult.empty()) {
        ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
        if (state.e2eResultType == 1)
            col = ImVec4(0.35f, 0.90f, 0.45f, 1.0f);
        if (state.e2eResultType == 2)
            col = ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
        ImGui::TextColored(col, "%s", state.e2eResult.c_str());
    }
}

void DrawWhisperAdvancedRows(UiDrawSession& d) {
    // --- Phase F language / trim / max-clip / auto-send rows. ---
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.language.label", "Language:"));
    ImGui::SameLine();
    {
        // Static set of common ISO codes the bundled English models +
        // multilingual cloud both understand. "auto" asks the backend to detect.
        // Users with a multilingual local model can add more codes via direct
        // config edit; the dropdown captures the common 80 % rather than
        // enumerating every supported tag.
        static const char* kLanguageCodes[] = {
            "en", "auto", "fr", "de", "es", "it", "pt", "nl", "ja", "zh", "ko", "ru", "pl", "ar", "hi", "tr",
        };
        int langIdx = 0;
        for (int i = 0; i < static_cast<int>(sizeof(kLanguageCodes) / sizeof(kLanguageCodes[0])); ++i) {
            if (d.cfg.WhisperLanguage == kLanguageCodes[i]) {
                langIdx = i;
                break;
            }
        }
        if (ImGui::Combo("##WhisperLanguage", &langIdx, kLanguageCodes,
                         static_cast<int>(sizeof(kLanguageCodes) / sizeof(kLanguageCodes[0])))) {
            d.cfg.WhisperLanguage = kLanguageCodes[langIdx];
            MarkPrefsDirty(d);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled(
        "%s", SmatchetLocalization::T("whisper.preferences.language.autoHint", "(or \"auto\" for autodetect)"));

    if (ImGui::Checkbox(SmatchetLocalization::T("whisper.preferences.trim.label", "Trim leading/trailing silence"),
                        &d.cfg.WhisperTrim)) {
        MarkPrefsDirty(d);
    }

    // Max clip length: int input with explicit clamp on edit. 0 = unlimited.
    {
        int maxClip = d.cfg.WhisperMaxClipSec;
        ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.maxClipSec.label", "Max clip length:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("##WhisperMaxClipSec", &maxClip, 0, 0)) {
            if (maxClip < 0)
                maxClip = 0;
            if (maxClip > 600)
                maxClip = 600;
            d.cfg.WhisperMaxClipSec = maxClip;
            MarkPrefsDirty(d);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.preferences.maxClipSec.unit", "seconds"));
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            SmatchetLocalization::T("whisper.preferences.maxClipSec.hint", "(0 = unlimited; max 600)"));
    }

    if (ImGui::Checkbox(SmatchetLocalization::T("whisper.preferences.autoSend.label",
                                                "Auto-send AI chat on punctuation (\".\", \"!\", \"?\")"),
                        &d.cfg.WhisperAutoSendOnPunctuation)) {
        MarkPrefsDirty(d);
    }
}

void DrawWhisperPrivacyAndRerun(UiDrawSession& d) {
    // Privacy disclosure — three-bullet list.
    ImGui::Separator();
    ImGui::TextUnformatted(SmatchetLocalization::T("whisper.preferences.privacyHeading", "Privacy disclosure"));
    ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyLocal",
                                                    "Local mode: audio stays on your machine; "
                                                    "no network call is made."));
    ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyCloud",
                                                    "Cloud mode: audio is uploaded to OpenAI "
                                                    "for transcription."));
    ImGui::BulletText("%s", SmatchetLocalization::T("whisper.preferences.privacyDisabled",
                                                    "Disabled: no microphone access, no "
                                                    "network call, no model download."));

    // Phase F — "Re-run setup banner" debug helper. Flips WhisperSetupCompleted
    // back to false so the first-run banner appears on next launch. Useful for
    // QA / repro flows; not for routine users (no harm if pressed — the banner
    // just re-asks the consent question).
    ImGui::Separator();
    if (SmatchetIconButton(ICON_FA_ARROWS_ROTATE, "Re-run setup banner", nullptr)) {
        d.cfg.WhisperSetupCompleted = false;
        d.cfg.WhisperSetupChoice.clear();
        MarkPrefsDirty(d);
        LOG_INFO("Whisper preferences: Re-run setup banner pressed — "
                 "WhisperSetupCompleted reset; banner returns next launch");
    }
    ImGui::SetItemTooltip("%s", SmatchetLocalization::T("whisper.preferences.rerunSetup.tooltip",
                                                        "Forces WhisperSetupCompleted=false; "
                                                        "banner appears next launch"));
}

} // namespace

void DrawWhisperPreferencesTab(IAppThreading& app, UiDrawSession& d) {
    // Whisper dictation Preferences tab — Phase C. Hotkey rebind UI lands in
    // Phase E, read-only display here. Master toggle persists through
    // MarkPrefsDirty (debounced save). Download / cancel buttons share the
    // banner-owned ModelDownloader so a fetch started from the banner continues
    // to show progress on this tab.
    static WhisperPrefsTabState s_state;
    SmatchetPreferencesUiDetail::PrefsSection(d, "ai_voice.dictation", [&] {
        ImGui::TextUnformatted("Push-to-talk dictation: hold the hotkey, speak, release.");
        ImGui::SameLine();
        SmatchetHelpMarker::Render("prefs.whisper.ptt.help",
                                   "Push-to-talk dictation. Hold the configured hotkey, speak, release. "
                                   "Transcription runs locally when a Whisper model is on disk; falls back "
                                   "to OpenAI Whisper API when no model is present (cloud mode requires an "
                                   "API key).");
        ImGui::Spacing();

        DrawWhisperEnableAndMode(d);
        DrawWhisperModelPicker(app, d);
        DrawWhisperHotkey(d, s_state);
        DrawWhisperApiKey(d, s_state);
        DrawWhisperAdvancedRows(d);
        DrawWhisperPrivacyAndRerun(d);
    });
    SmatchetPreferencesUiDetail::PrefsSection(d, "ai_voice.diagnostics", [&] {
        DrawWhisperTestConnection(app, d, s_state);
        DrawWhisperTestMicrophone(app, s_state);
        DrawWhisperTestE2E(app, d, s_state);
    });
}

#endif // SMATCHET_WITH_WHISPER
