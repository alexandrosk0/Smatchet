#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#if defined(SMATCHET_WITH_AI)

#include "SmatchetPreferencesUi_detail.h"
#include "SmatchetUI.h"
#include "AiAssistantController.h"
#include "AiClientFactory.h"
#include "AiEndpointPolicy.h"
#include "AiEndpointSanitize.h"
#include "AiModelCatalog.h"
#include "AiPrefsValidator.h"
#include "AiTypes.h"
#include "IAiClient.h"
#include "AppController.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetLocalization.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Persistent across frames + tab toggles — was a cluster of function-scope
// `static` locals. Re-seeded from `d.cfg.Ai*` on first paint + on provider
// change (see SeedAssistantBuffers).
struct AssistantPrefsBuffers {
    char agentsMdGlobalBuf[1024] = {};
    char projectAgentsMdBuf[1024] = {};
    bool agentsBufsSeeded = false;
    char openAiKeyBuf[1024] = {};
    char anthropicKeyBuf[1024] = {};
    char openAiModelBuf[256] = {};
    char anthropicModelBuf[256] = {};
    char ollamaModelBuf[256] = {};
    char baseUrlBuf[512] = {};
    char ollamaBaseUrlBuf[512] = {};
    // DeepSeek-specific buffers — same shape as the OpenAI / Anthropic pair.
    // Sized matching the existing key / model / URL buffers (1024 / 256 / 1024
    // respectively — the URL buffer doubles the others to accommodate path
    // suffixes some operators add to their endpoint).
    char deepseekKeyBuf[1024] = {};
    char deepseekBaseUrlBuf[1024] = {};
    char deepseekModelBuf[256] = {};
    bool aiBufsSeeded = false;
    int lastSeededProvider = -1;
};

void SeedAssistantBuffers(AssistantPrefsBuffers& b, UiDrawSession& d) {
    if (!b.agentsBufsSeeded) {
        b.agentsBufsSeeded = true;
        std::snprintf(b.agentsMdGlobalBuf, sizeof(b.agentsMdGlobalBuf), "%s", d.cfg.AgentsMdGlobalPath.c_str());
        std::snprintf(b.projectAgentsMdBuf, sizeof(b.projectAgentsMdBuf), "%s", d.cfg.ProjectAgentsMdPath.c_str());
    }
    // Reseed on first paint, on provider switch, or when the Test-connection
    // success callback persisted a fallback default value back into cfg (so
    // the buffer reflects the just-saved value on the next paint).
    if (!b.aiBufsSeeded || b.lastSeededProvider != d.cfg.AiProviderKind || d.assistantPrefsForceBufferReseed) {
        b.aiBufsSeeded = true;
        b.lastSeededProvider = d.cfg.AiProviderKind;
        d.assistantPrefsForceBufferReseed = false;
        std::snprintf(b.openAiKeyBuf, sizeof(b.openAiKeyBuf), "%s", d.cfg.AiApiKey.c_str());
        std::snprintf(b.anthropicKeyBuf, sizeof(b.anthropicKeyBuf), "%s", d.cfg.AiAnthropicApiKey.c_str());
        std::snprintf(b.openAiModelBuf, sizeof(b.openAiModelBuf), "%s", d.cfg.AiModelOpenAi.c_str());
        std::snprintf(b.anthropicModelBuf, sizeof(b.anthropicModelBuf), "%s", d.cfg.AiModelAnthropic.c_str());
        std::snprintf(b.ollamaModelBuf, sizeof(b.ollamaModelBuf), "%s", d.cfg.AiModelOllama.c_str());
        std::snprintf(b.baseUrlBuf, sizeof(b.baseUrlBuf), "%s", d.cfg.AiBaseUrl.c_str());
        std::snprintf(b.ollamaBaseUrlBuf, sizeof(b.ollamaBaseUrlBuf), "%s", d.cfg.AiOllamaBaseUrl.c_str());
        std::snprintf(b.deepseekKeyBuf, sizeof(b.deepseekKeyBuf), "%s", d.cfg.AiDeepSeekApiKey.c_str());
        std::snprintf(b.deepseekBaseUrlBuf, sizeof(b.deepseekBaseUrlBuf), "%s", d.cfg.AiDeepSeekBaseUrl.c_str());
        std::snprintf(b.deepseekModelBuf, sizeof(b.deepseekModelBuf), "%s", d.cfg.AiModelDeepSeek.c_str());
    }
}

// Any field edit invalidates a stale Test-connection result.
void ClearStaleTestResult(UiDrawSession& d) {
    if (!d.assistantPrefsTestInFlight) {
        d.assistantPrefsTestResult.clear();
        d.assistantPrefsTestResultType = 0;
    }
}

// Provider-aware ApiKey / BaseUrl / ModelId pick. Mirrors `BuildClientConfig`
// + `ResolveModelId` in `AiAssistantController.cpp`. Pure — no ImGui. Records
// the canonical local default it falls back to in `defaultedBaseUrl` so the
// success callback can persist it.
void ResolveProbeEndpoint(const TrackerConfig& probeCfg, AiProvider provider, std::string& apiKey, std::string& baseUrl,
                          std::string& modelId, std::string& defaultedBaseUrl) {
    switch (provider) {
    case AiProvider::Anthropic:
        apiKey = probeCfg.AiAnthropicApiKey;
        baseUrl = probeCfg.AiBaseUrl;
        modelId = probeCfg.AiModelAnthropic;
        break;
    case AiProvider::OllamaNative:
        apiKey.clear();
        baseUrl = probeCfg.AiOllamaBaseUrl;
        modelId = probeCfg.AiModelOllama;
        break;
    case AiProvider::OllamaOpenAiCompat:
        apiKey = probeCfg.AiApiKey;
        baseUrl = probeCfg.AiBaseUrl.empty() ? probeCfg.AiOllamaBaseUrl : probeCfg.AiBaseUrl;
        modelId = probeCfg.AiModelOpenAi;
        break;
    case AiProvider::DeepSeek:
        apiKey = probeCfg.AiDeepSeekApiKey;
        baseUrl = probeCfg.AiDeepSeekBaseUrl;
        modelId = probeCfg.AiModelDeepSeek;
        break;
    case AiProvider::OpenAi:
    default:
        apiKey = probeCfg.AiApiKey;
        baseUrl = probeCfg.AiBaseUrl;
        modelId = probeCfg.AiModelOpenAi;
        break;
    }
    // When the configured base URL is empty for a local provider, fall back
    // to the canonical default so the user can click Test connection right
    // after picking the provider — no manual URL entry needed. The default
    // is also recorded so the success callback can persist it back into cfg
    // (and the buffer reseed reflects it in the field).
    if (baseUrl.empty()) {
        if (provider == AiProvider::OllamaOpenAiCompat) {
            defaultedBaseUrl = "http://127.0.0.1:1234";
        } else if (provider == AiProvider::OllamaNative) {
            defaultedBaseUrl = "http://localhost:11434";
        } else if (provider == AiProvider::DeepSeek) {
            defaultedBaseUrl = "https://api.deepseek.com";
        }
        baseUrl = defaultedBaseUrl;
    }
}

// Worker-thread body — reachability GET + a real 1-token chat handshake.
// Pure of ImGui; runs on the joined background-task pool. Posts the verdict
// back through the dispatcher.
void RunProbeWorker(AiProvider provider, AiClientConfig clientCfg, std::shared_ptr<std::atomic<bool>> cancel,
                    std::string defaultedBaseUrl, std::string modelId, MainThreadDispatcher& dispatcher) {
    std::string errMsg;
    // Defensive try/catch — `MakeAiClient` / `ProbeReachability` /
    // `SendStreaming` all run third-party transport (cpr/libcurl) +
    // SSE parser code. An uncaught exception here would propagate out of the
    // background task and call `std::terminate`. Trap it, surface as a failure
    // result via the existing dispatcher path so UI state recovers.
    try {
        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
        if (!client) {
            errMsg = "Provider not available in this build.";
        } else {
            // Step 1: reachability — server alive plus auth accepted on the
            // cheap model-listing endpoint.
            errMsg = client->ProbeReachability(clientCfg);
            // Step 2: real chat handshake — sends a 1-token "ping" against
            // the configured model so model-not-found / chat-disabled /
            // missing-loaded-model errors surface BEFORE the user types
            // their first real prompt. This is what made earlier Test-
            // connection passes mislead users into thinking the full chat
            // path worked when it did not — a healthy model-list endpoint does
            // not imply a healthy chat endpoint against a loaded model.
            if (errMsg.empty()) {
                if (modelId.empty()) {
                    errMsg = "chat: model id is empty (set 'Model' field)";
                } else {
                    AiChatRequest req;
                    req.Model = modelId;
                    AiMessage userMsg;
                    userMsg.Role = "user";
                    userMsg.Content = "ping";
                    req.History.push_back(std::move(userMsg));
                    req.MaxTokens = 4;
                    std::atomic<bool> sawDelta(false);
                    std::string chatErr;
                    auto onDelta = [&](const AiStreamDelta& d2) {
                        if (!d2.TokenChunk.empty() || d2.IsFinal) {
                            sawDelta.store(true);
                        }
                    };
                    auto onError = [&](const AiStreamError& e) { chatErr = e.Message; };
                    client->SendStreaming(clientCfg, req, onDelta, onError, cancel);
                    if (!chatErr.empty()) {
                        errMsg = std::string("chat: ") + chatErr;
                    } else if (!sawDelta.load()) {
                        errMsg = "chat: server returned no content";
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        errMsg = std::string("internal error: ") + ex.what();
    } catch (...) {
        errMsg = "internal error: unknown exception";
    }
    dispatcher.PostToMainThread([errMsg, cancel, provider, defaultedBaseUrl]() {
        if (cancel && cancel->load()) {
            return;
        }
        g_ui.assistantPrefsTestInFlight = false;
        if (errMsg.empty()) {
            LOG_INFO("Preferences: Test connection VERIFIED providerKind=%d defaultedBaseUrl='%s'",
                     static_cast<int>(provider), defaultedBaseUrl.c_str());
            g_ui.assistantPrefsTestResult = "Verified";
            g_ui.assistantPrefsTestResultType = 1;
            // On success with a defaulted URL, persist the default into
            // cfg + force a buffer reseed so the field shows the value
            // the probe used.
            if (!defaultedBaseUrl.empty()) {
                if (provider == AiProvider::OllamaOpenAiCompat) {
                    g_ui.cfg.AiBaseUrl = defaultedBaseUrl;
                } else if (provider == AiProvider::OllamaNative) {
                    g_ui.cfg.AiOllamaBaseUrl = defaultedBaseUrl;
                } else if (provider == AiProvider::DeepSeek) {
                    g_ui.cfg.AiDeepSeekBaseUrl = defaultedBaseUrl;
                }
                MarkPrefsDirty(g_ui);
                g_ui.assistantPrefsForceBufferReseed = true;
            }
        } else {
            LOG_ERROR("Preferences: Test connection FAILED providerKind=%d errMsg='%s'", static_cast<int>(provider),
                      errMsg.c_str());
            g_ui.assistantPrefsTestResult = std::string("Failed: ") + errMsg;
            g_ui.assistantPrefsTestResultType = 2;
        }
    });
}

// Async probe entry — resolves the endpoint, sanitises key + URL, then hands
// the work to the joined background-task pool. Captured by value into the
// worker so a concurrent UI edit during the probe doesn't change the bytes the
// worker sees. Cancel-on-close flips the cancel atom so the posted callback
// short-circuits if Preferences closes mid-probe.
void LaunchTestProbe(AppController& app, UiDrawSession& d, TrackerConfig probeCfg, AiProvider provider) {
    LOG_INFO("Preferences: Test connection start providerKind=%d", static_cast<int>(provider));
    d.assistantPrefsTestInFlight = true;
    d.assistantPrefsTestResult = "Testing...";
    d.assistantPrefsTestResultType = 0;
    d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = d.assistantPrefsTestCancel;

    std::string apiKey;
    std::string baseUrl;
    std::string modelId;
    std::string defaultedBaseUrl;
    ResolveProbeEndpoint(probeCfg, provider, apiKey, baseUrl, modelId, defaultedBaseUrl);

    // Strip header-smuggling control characters from the key (cheap
    // defence-in-depth; libcurl rejects them too).
    std::string sanitisedKey;
    sanitisedKey.reserve(apiKey.size());
    std::copy_if(apiKey.begin(), apiKey.end(), std::back_inserter(sanitisedKey),
                 [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
    std::string sanitisedBase;
    if (!baseUrl.empty()) {
        std::string normalised;
        const smatchet::ai::pure::EndpointPolicy policy = smatchet::ai::EndpointPolicyForProvider(d.cfg, provider);
        const smatchet::ai::pure::EndpointVerdict v =
            smatchet::ai::pure::SanitizeAiEndpointUrl(baseUrl, policy, normalised);
        if (v == smatchet::ai::pure::EndpointVerdict::Allowed) {
            sanitisedBase = normalised;
        } else {
            LOG_WARN("Preferences: Test connection — endpoint URL %s; falling back to provider default.",
                     smatchet::ai::pure::EndpointVerdictDescription(v));
        }
    }
    AiClientConfig clientCfg;
    clientCfg.ApiKey = sanitisedKey;
    clientCfg.BaseUrl = sanitisedBase;
    clientCfg.ConnectTimeoutMs = 5000;
    clientCfg.TotalTimeoutMs = 15000;

    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
    // Joined background-task pool (not a raw detached thread — forbidden by the
    // no-detach lint); joined at shutdown so &dispatcher stays valid for the task.
    app.LaunchBackgroundTask([provider, clientCfg, cancel, defaultedBaseUrl, modelId, &dispatcher]() {
        RunProbeWorker(provider, clientCfg, cancel, defaultedBaseUrl, modelId, dispatcher);
    });
}

// Sticky validation banner — rendered FIRST so it stays at the top of the tab.
void RenderValidationBanner(const smatchet::ai::PrefsValidation& validation) {
    if (validation.Errors.empty() && validation.Warnings.empty()) {
        return;
    }
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float pad = ImGui::GetStyle().FramePadding.y * 2.0f + 4.0f;
    if (!validation.Errors.empty()) {
        const ImVec4 kErrFill(0.35f, 0.10f, 0.10f, 1.0f);
        const ImVec4 kErrText(1.0f, 0.55f, 0.55f, 1.0f);
        // +1 for the header line; bullets compact via line-height.
        const float h = lineH * static_cast<float>(validation.Errors.size() + 1) + pad;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kErrFill);
        ImGui::BeginChild("##AiPrefsErrorBanner", ImVec2(0.0f, h), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, kErrText);
        ImGui::TextWrapped("(!) %d configuration error%s - see details below:",
                           static_cast<int>(validation.Errors.size()), validation.Errors.size() == 1 ? "" : "s");
        for (const auto& e : validation.Errors) {
            ImGui::BulletText("%s", e.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!validation.Warnings.empty()) {
        const ImVec4 kWarnFill(0.32f, 0.26f, 0.06f, 1.0f);
        const ImVec4 kWarnText(1.0f, 0.90f, 0.45f, 1.0f);
        const float h = lineH * static_cast<float>(validation.Warnings.size() + 1) + pad;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kWarnFill);
        ImGui::BeginChild("##AiPrefsWarnBanner", ImVec2(0.0f, h), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, kWarnText);
        ImGui::TextWrapped("Warnings (Save still proceeds):");
        for (const auto& w : validation.Warnings) {
            ImGui::BulletText("%s", w.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

// Provider Combo + the prominent always-enabled Test connection button and its
// result line. Returns the selected provider kind for the credential section.
AiProvider RenderProviderComboAndTest(AppController& app, UiDrawSession& d) {
    // --- Provider Combo (top). Picking from this dropdown seeds sensible
    // defaults for the chosen provider so the LM Studio / Ollama happy paths
    // need zero extra fields.
    const std::vector<AiClientFactory::ProviderEntry> providers = AiClientFactory::EnumeratedProviders();
    std::vector<const char*> providerLabels;
    providerLabels.reserve(providers.size());
    std::transform(providers.begin(), providers.end(), std::back_inserter(providerLabels),
                   [](const AiClientFactory::ProviderEntry& p) { return p.Display.c_str(); });
    int providerIdx = 0;
    auto providerIt = std::find_if(providers.begin(), providers.end(), [&](const AiClientFactory::ProviderEntry& p) {
        return static_cast<int>(p.Kind) == d.cfg.AiProviderKind;
    });
    if (providerIt != providers.end()) {
        providerIdx = static_cast<int>(std::distance(providers.begin(), providerIt));
    }
    if (ImGui::Combo("AI provider", &providerIdx, providerLabels.data(), static_cast<int>(providerLabels.size()))) {
        const int newKind = static_cast<int>(providers[providerIdx].Kind);
        LOG_INFO("Preferences: AiProviderKind %d -> %d", d.cfg.AiProviderKind, newKind);
        d.cfg.AiProviderKind = newKind;
        MarkPrefsDirty(d);
        ClearStaleTestResult(d);
    }
    const AiProvider selectedKind = providers[providerIdx].Kind;

    // --- Test connection button (top, prominent, always enabled). When the
    // configured state can't possibly succeed the probe itself reports a
    // clear server-side error — better UX than disabling the button.
    ImGui::Spacing();
    const bool testInFlight = d.assistantPrefsTestInFlight;
    if (testInFlight) {
        ImGui::BeginDisabled(true);
    }
    const bool testPressed = ImGui::Button("Test connection");
    if (testInFlight) {
        ImGui::EndDisabled();
    }
    if (testPressed && !testInFlight) {
        LaunchTestProbe(app, d, d.cfg, selectedKind);
    }
    ImGui::SameLine();
    if (!d.assistantPrefsTestResult.empty()) {
        const int kind = d.assistantPrefsTestResultType;
        ImVec4 col(0.78f, 0.78f, 0.78f, 1.0f);
        if (kind == 1) {
            col = ImVec4(0.45f, 0.95f, 0.55f, 1.0f);
        } else if (kind == 2) {
            col = ImVec4(1.0f, 0.55f, 0.55f, 1.0f);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(d.assistantPrefsTestResult.c_str());
        ImGui::PopStyleColor();
        if (kind == 2 && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", d.assistantPrefsTestResult.c_str());
        }
    } else {
        ImGui::TextDisabled("Click to verify the configured provider can be reached.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    return selectedKind;
}

// Model picker shape: when the provider ships a published catalog
// (`KnownModels(provider)` non-empty), render a Combo + a collapsing "Custom
// model ID" header for free-form override. Otherwise (local OpenAI-compat /
// Ollama-native, models are user-side) render plain InputText with a hint.
void RenderModelPicker(UiDrawSession& d, const char* comboLabel, const char* freeFormLabel, const char* freeFormHint,
                       AiProvider catalogProvider, char* modelBuf, std::size_t modelBufCap, std::string& cfgField) {
    const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(catalogProvider);
    if (catalog.empty()) {
        if (ImGui::InputTextWithHint(freeFormLabel, freeFormHint, modelBuf, static_cast<int>(modelBufCap))) {
            cfgField = modelBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
        return;
    }
    std::vector<const char*> displayPtrs;
    displayPtrs.reserve(catalog.size());
    std::transform(catalog.begin(), catalog.end(), std::back_inserter(displayPtrs),
                   [](const smatchet::ai::ModelOption& m) { return m.DisplayName.c_str(); });
    int selectedIdx = -1;
    auto it = std::find_if(catalog.begin(), catalog.end(),
                           [&](const smatchet::ai::ModelOption& m) { return m.Id == modelBuf; });
    if (it != catalog.end()) {
        selectedIdx = static_cast<int>(std::distance(catalog.begin(), it));
    }
    int comboIdx = (selectedIdx >= 0) ? selectedIdx : 0;
    if (ImGui::Combo(comboLabel, &comboIdx, displayPtrs.data(), static_cast<int>(displayPtrs.size()))) {
        std::snprintf(modelBuf, modelBufCap, "%s", catalog[static_cast<std::size_t>(comboIdx)].Id.c_str());
        cfgField = modelBuf;
        MarkPrefsDirty(d);
        ClearStaleTestResult(d);
    }
    if (selectedIdx < 0 && modelBuf[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextDisabled("(custom: %s)", modelBuf);
    }
    if (ImGui::CollapsingHeader("Custom model ID (advanced)")) {
        if (ImGui::InputText("##model_custom", modelBuf, static_cast<int>(modelBufCap))) {
            cfgField = modelBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
    }
}

// Custom-endpoint consent toggle + one-time confirm modal (SSRF / cleartext-key
// defense-in-depth). By default OpenAi / Anthropic requests are pinned to the
// canonical host over https. Ticking it on opens a modal that explains the risk,
// the consent flag only commits on explicit confirm. Unticking commits false
// immediately. Local / DeepSeek providers are unpinned and don't render it.
void RenderCustomEndpointConsent(UiDrawSession& d, AiProvider prov, bool& flag, const char* providerLabel,
                                 const char* canonicalHost, int& consentModalProvider) {
    bool checked = flag;
    if (ImGui::Checkbox("Allow custom endpoint (proxy / gateway)", &checked)) {
        if (checked && !flag) {
            consentModalProvider = static_cast<int>(prov);
            ImGui::OpenPopup("Allow custom AI endpoint?");
        } else if (!checked && flag) {
            flag = false;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
    }
    if (consentModalProvider == static_cast<int>(prov)) {
        ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Allow custom AI endpoint?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            // Consent modal: the cleartext-key risk must stay visible (informed
            // consent); only the supporting detail moves into the (?) tooltip.
            ImGui::TextWrapped("%s", SmatchetLocalization::Format(
                                         "prefs.assistant.endpoint_consent.short",
                                         "A custom endpoint can receive your %s API key - possibly in cleartext. "
                                         "Enable only if you trust it.",
                                         providerLabel));
            ImGui::SameLine();
            const std::string consentDetail = SmatchetLocalization::Format(
                "prefs.assistant.endpoint_consent.help",
                "By default Smatchet sends your %s API key only to %s over HTTPS. Enabling custom "
                "endpoints lets a proxy / gateway host (Azure OpenAI, LiteLLM, openrouter) receive "
                "that key, and permits plain http:// to non-loopback hosts - sending the key in "
                "cleartext. Enable only if you trust the endpoint you configure.",
                providerLabel, canonicalHost);
            SmatchetHelpMarker::RenderText(consentDetail.c_str());
            ImGui::Separator();
            if (ImGui::Button("Enable custom endpoint")) {
                flag = true;
                MarkPrefsDirty(d);
                ClearStaleTestResult(d);
                consentModalProvider = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                consentModalProvider = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// Per-provider credential fields (key / model / base URL + custom-endpoint
// consent), auto-saved on every edit. One branch per provider kind.
void RenderProviderCredentials(UiDrawSession& d, AiProvider selectedKind, AssistantPrefsBuffers& b,
                               int& consentModalProvider) {
    if (selectedKind == AiProvider::OpenAi || selectedKind == AiProvider::OllamaOpenAiCompat) {
        const bool isLocalCompat = (selectedKind == AiProvider::OllamaOpenAiCompat);
        const char* keyLabel = isLocalCompat ? "API key (optional for local)" : "OpenAI API key";
        if (ImGui::InputText(keyLabel, b.openAiKeyBuf, sizeof(b.openAiKeyBuf), ImGuiInputTextFlags_Password)) {
            d.cfg.AiApiKey = b.openAiKeyBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
        // OllamaOpenAiCompat keeps an empty catalog — the local server
        // names its own models. `RenderModelPicker` falls back to free-form
        // hint in that case. OpenAi has a catalog so the Combo renders.
        const char* modelComboLabel = isLocalCompat ? "Model" : "OpenAI model";
        const char* modelFreeFormLabel = modelComboLabel;
        const char* modelHint = isLocalCompat ? "e.g. local-model, llama3, qwen2.5" : "";
        RenderModelPicker(d, modelComboLabel, modelFreeFormLabel, modelHint, selectedKind, b.openAiModelBuf,
                          sizeof(b.openAiModelBuf), d.cfg.AiModelOpenAi);
        const char* urlHint = isLocalCompat ? "http://127.0.0.1:1234 (LM Studio)" : "https://api.openai.com";
        if (ImGui::InputTextWithHint("Base URL", urlHint, b.baseUrlBuf, sizeof(b.baseUrlBuf))) {
            d.cfg.AiBaseUrl = b.baseUrlBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
        // Real OpenAi only. The local-compat shim is unpinned - its base URL is the config itself.
        if (!isLocalCompat) {
            RenderCustomEndpointConsent(d, AiProvider::OpenAi, d.cfg.AiAllowCustomEndpointOpenAi, "OpenAI",
                                        "api.openai.com", consentModalProvider);
        }
    } else if (selectedKind == AiProvider::Anthropic) {
        if (ImGui::InputText("Anthropic API key", b.anthropicKeyBuf, sizeof(b.anthropicKeyBuf),
                             ImGuiInputTextFlags_Password)) {
            d.cfg.AiAnthropicApiKey = b.anthropicKeyBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
        RenderModelPicker(d, "Anthropic model", "Anthropic model", "", AiProvider::Anthropic, b.anthropicModelBuf,
                          sizeof(b.anthropicModelBuf), d.cfg.AiModelAnthropic);
        RenderCustomEndpointConsent(d, AiProvider::Anthropic, d.cfg.AiAllowCustomEndpointAnthropic, "Anthropic",
                                    "api.anthropic.com", consentModalProvider);
    } else if (selectedKind == AiProvider::OllamaNative) {
        RenderModelPicker(d, "Ollama model", "Ollama model", "e.g. llama3, qwen2.5, mistral", AiProvider::OllamaNative,
                          b.ollamaModelBuf, sizeof(b.ollamaModelBuf), d.cfg.AiModelOllama);
        if (ImGui::InputTextWithHint("Ollama base URL", "http://localhost:11434", b.ollamaBaseUrlBuf,
                                     sizeof(b.ollamaBaseUrlBuf))) {
            d.cfg.AiOllamaBaseUrl = b.ollamaBaseUrlBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
    } else if (selectedKind == AiProvider::DeepSeek) {
        if (ImGui::InputText("DeepSeek API key", b.deepseekKeyBuf, sizeof(b.deepseekKeyBuf),
                             ImGuiInputTextFlags_Password)) {
            d.cfg.AiDeepSeekApiKey = b.deepseekKeyBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
        RenderModelPicker(d, "DeepSeek model", "DeepSeek model", "", AiProvider::DeepSeek, b.deepseekModelBuf,
                          sizeof(b.deepseekModelBuf), d.cfg.AiModelDeepSeek);
        if (ImGui::InputTextWithHint("Base URL", "https://api.deepseek.com", b.deepseekBaseUrlBuf,
                                     sizeof(b.deepseekBaseUrlBuf))) {
            d.cfg.AiDeepSeekBaseUrl = b.deepseekBaseUrlBuf;
            MarkPrefsDirty(d);
            ClearStaleTestResult(d);
        }
    }
}

// Default reasoning effort (all providers). Stored as a string enum:
// "auto" | "low" | "medium" | "high". "auto" omits the wire parameter (server
// picks). The OpenAI client forwards it as the wire `reasoning_effort` body
// field; providers that don't understand it ignore it. The chat-window has a
// per-turn Combo that overrides this default for one Send.
void RenderReasoningEffort(UiDrawSession& d) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const char* kEffortLabels[] = {"Auto (server picks)", "Low", "Medium", "High"};
    const char* kEffortIds[] = {"auto", "low", "medium", "high"};
    int effortIdx = 0;
    for (int i = 0; i < 4; ++i) {
        if (d.cfg.AiReasoningEffort == kEffortIds[i]) {
            effortIdx = i;
            break;
        }
    }
    if (ImGui::Combo("Default reasoning effort", &effortIdx, kEffortLabels, 4)) {
        LOG_INFO("Preferences: AiReasoningEffort %s -> %s", d.cfg.AiReasoningEffort.c_str(), kEffortIds[effortIdx]);
        d.cfg.AiReasoningEffort = kEffortIds[effortIdx];
        MarkPrefsDirty(d);
        ClearStaleTestResult(d);
    }
    ImGui::SetItemTooltip("OpenAI `reasoning_effort` request parameter.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.assistant.reasoning_effort.help",
                               "OpenAI `reasoning_effort` body parameter for o-series / reasoning-tuned "
                               "models. LM Studio + LocalAI pass it through to local reasoning models "
                               "(Qwen3, gemma-3, etc.). Providers that don't understand the parameter "
                               "ignore it.");
}

// agents.md harness (optional) — layered system-prompt path config. Each edit
// invalidates the assistant controller's agents.md cache so the next turn
// re-reads from disk.
void RenderAgentsMdHarness(AppController& app, UiDrawSession& d, AssistantPrefsBuffers& b) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("agents.md harness (optional)");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.assistant.agents_md.help",
                               "Layered system prompt injected into every Assistant turn. Global layer "
                               "defaults to %LOCALAPPDATA%/Smatchet/agents.md when blank. Each layer "
                               "capped at 64 KB.");
    ImGui::Spacing();
    if (ImGui::InputText("Global agents.md path", b.agentsMdGlobalBuf, sizeof(b.agentsMdGlobalBuf))) {
        d.cfg.AgentsMdGlobalPath = b.agentsMdGlobalBuf;
        MarkPrefsDirty(d);
        if (app.HasAiAssistantController()) {
            app.GetAiAssistantController().InvalidateAgentsMdCache();
        }
    }
    ImGui::SetItemTooltip("Override the global agents.md location.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.assistant.agents_md_global.help",
                               "Default %LOCALAPPDATA%/Smatchet/agents.md when blank. Override to point "
                               "at a checked-in shared file.");
    if (ImGui::InputText("Project agents.md path (override)", b.projectAgentsMdBuf, sizeof(b.projectAgentsMdBuf))) {
        d.cfg.ProjectAgentsMdPath = b.projectAgentsMdBuf;
        MarkPrefsDirty(d);
        if (app.HasAiAssistantController()) {
            app.GetAiAssistantController().InvalidateAgentsMdCache();
        }
    }
    ImGui::SetItemTooltip("Explicit project-layer path.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.assistant.agents_md_project.help",
                               "When set, this exact path is used as the project layer. Leave blank to "
                               "disable the project layer entirely unless Auto-discover is enabled below.");
    bool autoDiscover = d.cfg.AgentsMdAutoDiscoverProject;
    if (ImGui::Checkbox("Auto-discover project agents.md (walk up from cwd)", &autoDiscover)) {
        d.cfg.AgentsMdAutoDiscoverProject = autoDiscover;
        MarkPrefsDirty(d);
        if (app.HasAiAssistantController()) {
            app.GetAiAssistantController().InvalidateAgentsMdCache();
        }
    }
    ImGui::SetItemTooltip("Walk up from cwd looking for agents.md.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("prefs.assistant.agents_md_autodiscover.help",
                               "OFF (default): only the Global file + explicit Project path are used. ON: "
                               "walks up the cwd chain looking for agents.md / AGENTS.md.");
}

} // namespace

void DrawAssistantPreferencesTab(AppController& app, UiDrawSession& d) {
    // Assistant tab — minimal config. Goal: "select provider from dropdown +
    // press Test connection" Just Works for LM Studio out of the box.
    // Per-provider sensible defaults are seeded on provider switch. Field edits
    // commit through MarkPrefsDirty (debounced ~100 ms save). No explicit Save
    // button — the validator banner + the Test-connection result line provide
    // all the feedback.
    static AssistantPrefsBuffers s_bufs;
    static int s_consentModalProvider = -1; // -1 = none; else AiProviderKind awaiting confirm
    SeedAssistantBuffers(s_bufs, d);

    if (ImGui::BeginTabItem("Assistant")) {
        d.preferencesActiveTab = PreferencesActiveTab::Assistant;
        // Validator runs against the live cfg (auto-saved on every field edit)
        // so the user gets live feedback for the text they're typing.
        const smatchet::ai::PrefsValidation validation = smatchet::ai::ValidateAiPrefs(d.cfg);
        RenderValidationBanner(validation);

        const AiProvider selectedKind = RenderProviderComboAndTest(app, d);

        // --- Per-provider credentials (auto-saved on every edit). ---
        RenderProviderCredentials(d, selectedKind, s_bufs, s_consentModalProvider);

        RenderReasoningEffort(d);
        RenderAgentsMdHarness(app, d, s_bufs);

        ImGui::EndTabItem();
    }
}

#endif // SMATCHET_WITH_AI
