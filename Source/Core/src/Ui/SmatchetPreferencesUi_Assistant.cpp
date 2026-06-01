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
#include "AiEndpointSanitize.h"
#include "AiModelCatalog.h"
#include "AiPrefsValidator.h"
#include "AiTypes.h"
#include "IAiClient.h"
#include "AppController.h"
#include "Logger.h"
#include "SmatchetUiSession.h"
#include "SmatchetLocalization.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

void DrawAssistantPreferencesTab(AppController& app, UiDrawSession& d) {
    // ----- Assistant tab — minimal config. -----
    // Goal: "select provider from dropdown + press Test connection" Just Works
    // for LM Studio out of the box. Per-provider sensible defaults are seeded
    // on provider switch. Field edits commit through MarkPrefsDirty (debounced
    // ~100 ms save). No explicit Save button — the validator banner + the
    // Test-connection result line provide all the feedback.
    // Static InputText buffers live at function scope across frames + tab
    // toggles. Re-seeded from `d.cfg.Ai*` on first paint + on provider change.
    static char s_agentsMdGlobalBuf[1024] = {};
    static char s_projectAgentsMdBuf[1024] = {};
    static bool s_agentsBufsSeeded = false;
    static char s_openAiKeyBuf[1024] = {};
    static char s_anthropicKeyBuf[1024] = {};
    static char s_openAiModelBuf[256] = {};
    static char s_anthropicModelBuf[256] = {};
    static char s_ollamaModelBuf[256] = {};
    static char s_baseUrlBuf[512] = {};
    static char s_ollamaBaseUrlBuf[512] = {};
    // DeepSeek-specific buffers — same shape as the OpenAI / Anthropic pair.
    // Sized matching the existing key / model / URL buffers (1024 / 256 / 1024
    // respectively — the URL buffer doubles the others to accommodate path
    // suffixes some operators add to their endpoint).
    static char s_deepseekKeyBuf[1024] = {};
    static char s_deepseekBaseUrlBuf[1024] = {};
    static char s_deepseekModelBuf[256] = {};
    static bool s_aiBufsSeeded = false;
    static int s_lastSeededProvider = -1;
    if (!s_agentsBufsSeeded) {
        s_agentsBufsSeeded = true;
        std::snprintf(s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf), "%s", d.cfg.AgentsMdGlobalPath.c_str());
        std::snprintf(s_projectAgentsMdBuf, sizeof(s_projectAgentsMdBuf), "%s", d.cfg.ProjectAgentsMdPath.c_str());
    }
        // Reseed on first paint, on provider switch, or when the Test-connection
        // success callback persisted a fallback default value back into cfg (so
        // the buffer reflects the just-saved value on the next paint).
        if (!s_aiBufsSeeded || s_lastSeededProvider != d.cfg.AiProviderKind || d.assistantPrefsForceBufferReseed) {
            s_aiBufsSeeded = true;
            s_lastSeededProvider = d.cfg.AiProviderKind;
            d.assistantPrefsForceBufferReseed = false;
            std::snprintf(s_openAiKeyBuf, sizeof(s_openAiKeyBuf), "%s", d.cfg.AiApiKey.c_str());
            std::snprintf(s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf), "%s", d.cfg.AiAnthropicApiKey.c_str());
            std::snprintf(s_openAiModelBuf, sizeof(s_openAiModelBuf), "%s", d.cfg.AiModelOpenAi.c_str());
            std::snprintf(s_anthropicModelBuf, sizeof(s_anthropicModelBuf), "%s", d.cfg.AiModelAnthropic.c_str());
            std::snprintf(s_ollamaModelBuf, sizeof(s_ollamaModelBuf), "%s", d.cfg.AiModelOllama.c_str());
            std::snprintf(s_baseUrlBuf, sizeof(s_baseUrlBuf), "%s", d.cfg.AiBaseUrl.c_str());
            std::snprintf(s_ollamaBaseUrlBuf, sizeof(s_ollamaBaseUrlBuf), "%s", d.cfg.AiOllamaBaseUrl.c_str());
            std::snprintf(s_deepseekKeyBuf, sizeof(s_deepseekKeyBuf), "%s", d.cfg.AiDeepSeekApiKey.c_str());
            std::snprintf(s_deepseekBaseUrlBuf, sizeof(s_deepseekBaseUrlBuf), "%s", d.cfg.AiDeepSeekBaseUrl.c_str());
            std::snprintf(s_deepseekModelBuf, sizeof(s_deepseekModelBuf), "%s", d.cfg.AiModelDeepSeek.c_str());
        }

        if (ImGui::BeginTabItem("Assistant")) {
            // --- Sticky validation banner — rendered FIRST so it stays at the top
            // of the tab. Validator runs against the live cfg (auto-saved on every
            // field edit) so the user gets live feedback for the text they're
            // typing.
            const smatchet::ai::PrefsValidation validation = smatchet::ai::ValidateAiPrefs(d.cfg);

            auto renderBanner = [&]() {
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
                    ImGui::TextWrapped(
                        "(!) %d configuration error%s - see details below:", static_cast<int>(validation.Errors.size()),
                        validation.Errors.size() == 1 ? "" : "s");
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
            };
            renderBanner();

            // Any field edit invalidates a stale Test-connection result.
            auto clearStaleTestResult = [&]() {
                if (!d.assistantPrefsTestInFlight) {
                    d.assistantPrefsTestResult.clear();
                    d.assistantPrefsTestResultType = 0;
                }
            };

            // Async probe helper — runs ProbeReachability on a worker thread and
            // posts the result back via MainThreadDispatcher. Captured by value into
            // the worker lambda so a concurrent UI edit during the probe doesn't
            // change the bytes the worker sees. Cancel-on-close (above) flips the
            // cancel atom so the posted callback short-circuits if Preferences
            // closes mid-probe.
            auto runProbe = [&app, &d](TrackerConfig probeCfg, AiProvider provider) {
                LOG_INFO("Preferences: Test connection start providerKind=%d", static_cast<int>(provider));
                d.assistantPrefsTestInFlight = true;
                d.assistantPrefsTestResult = "Testing...";
                d.assistantPrefsTestResultType = 0;
                d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
                auto cancel = d.assistantPrefsTestCancel;
                // Provider-aware ApiKey / BaseUrl / ModelId pick. Mirrors
                // `BuildClientConfig` + `ResolveModelId` in `AiAssistantController.cpp`.
                std::string apiKey;
                std::string baseUrl;
                std::string modelId;
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
                std::string defaultedBaseUrl;
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
                // Strip header-smuggling control characters from the key (cheap
                // defence-in-depth; libcurl rejects them too).
                std::string sanitisedKey;
                sanitisedKey.reserve(apiKey.size());
                std::copy_if(apiKey.begin(), apiKey.end(), std::back_inserter(sanitisedKey),
                             [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
                std::string sanitisedBase;
                if (!baseUrl.empty()) {
                    std::string normalised;
                    const smatchet::ai::pure::EndpointVerdict v =
                        smatchet::ai::pure::SanitizeAiEndpointUrl(baseUrl, normalised);
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
                    std::string errMsg;
                    // Defensive try/catch — `MakeAiClient` / `ProbeReachability` /
                    // `SendStreaming` all run third-party transport (cpr/libcurl) +
                    // SSE parser code. An uncaught exception here would propagate
                    // out of the detached thread and call `std::terminate`. Trap
                    // it, surface as a failure result via the existing dispatcher
                    // path so UI state (in-flight flag + result line) recovers.
                    try {
                        std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
                        if (!client) {
                            errMsg = "Provider not available in this build.";
                        } else {
                            // Step 1: reachability — server alive + auth accepted on the
                            // listing endpoint (cheap GET).
                            errMsg = client->ProbeReachability(clientCfg);
                            // Step 2: real chat handshake — sends a 1-token "ping" against
                            // the configured model so model-not-found / chat-disabled /
                            // missing-loaded-model errors surface BEFORE the user types
                            // their first real prompt. This is what made earlier Test-
                            // connection passes mislead users into thinking the full chat
                            // path worked (it didn't — /v1/models OK ≠ /v1/chat/completions
                            // OK against a loaded model).
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
                            LOG_ERROR("Preferences: Test connection FAILED providerKind=%d errMsg='%s'",
                                      static_cast<int>(provider), errMsg.c_str());
                            g_ui.assistantPrefsTestResult = std::string("Failed: ") + errMsg;
                            g_ui.assistantPrefsTestResultType = 2;
                        }
                    });
                });
            };

            // --- Provider Combo (top). Picking from this dropdown seeds sensible
            // defaults for the chosen provider so the LM Studio / Ollama happy paths
            // need zero extra fields.
            const std::vector<AiClientFactory::ProviderEntry> providers = AiClientFactory::EnumeratedProviders();
            std::vector<const char*> providerLabels;
            providerLabels.reserve(providers.size());
            std::transform(providers.begin(), providers.end(), std::back_inserter(providerLabels),
                           [](const AiClientFactory::ProviderEntry& p) { return p.Display.c_str(); });
            int providerIdx = 0;
            auto providerIt =
                std::find_if(providers.begin(), providers.end(), [&](const AiClientFactory::ProviderEntry& p) {
                    return static_cast<int>(p.Kind) == d.cfg.AiProviderKind;
                });
            if (providerIt != providers.end()) {
                providerIdx = static_cast<int>(std::distance(providers.begin(), providerIt));
            }
            if (ImGui::Combo("AI provider", &providerIdx, providerLabels.data(),
                             static_cast<int>(providerLabels.size()))) {
                const int newKind = static_cast<int>(providers[providerIdx].Kind);
                LOG_INFO("Preferences: AiProviderKind %d -> %d", d.cfg.AiProviderKind, newKind);
                d.cfg.AiProviderKind = newKind;
                MarkPrefsDirty(d);
                clearStaleTestResult();
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
                runProbe(d.cfg, selectedKind);
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

            // --- Per-provider credentials (auto-saved on every edit). ---
            // Model picker shape: when the provider ships a published catalog
            // (`KnownModels(provider)` non-empty), render a Combo + a collapsing
            // "Custom model ID" header for free-form override. Otherwise (local
            // OpenAI-compat / Ollama-native, models are user-side) render plain
            // InputText with a hint.
            auto renderModelPicker = [&](const char* comboLabel, const char* freeFormLabel, const char* freeFormHint,
                                         AiProvider catalogProvider, char* modelBuf, std::size_t modelBufCap,
                                         std::string& cfgField) {
                const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(catalogProvider);
                if (catalog.empty()) {
                    if (ImGui::InputTextWithHint(freeFormLabel, freeFormHint, modelBuf,
                                                 static_cast<int>(modelBufCap))) {
                        cfgField = modelBuf;
                        MarkPrefsDirty(d);
                        clearStaleTestResult();
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
                    clearStaleTestResult();
                }
                if (selectedIdx < 0 && modelBuf[0] != '\0') {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(custom: %s)", modelBuf);
                }
                if (ImGui::CollapsingHeader("Custom model ID (advanced)")) {
                    if (ImGui::InputText("##model_custom", modelBuf, static_cast<int>(modelBufCap))) {
                        cfgField = modelBuf;
                        MarkPrefsDirty(d);
                        clearStaleTestResult();
                    }
                }
            };

            if (selectedKind == AiProvider::OpenAi || selectedKind == AiProvider::OllamaOpenAiCompat) {
                const bool isLocalCompat = (selectedKind == AiProvider::OllamaOpenAiCompat);
                const char* keyLabel = isLocalCompat ? "API key (optional for local)" : "OpenAI API key";
                if (ImGui::InputText(keyLabel, s_openAiKeyBuf, sizeof(s_openAiKeyBuf), ImGuiInputTextFlags_Password)) {
                    d.cfg.AiApiKey = s_openAiKeyBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                // OllamaOpenAiCompat keeps an empty catalog — the local server
                // names its own models. `renderModelPicker` falls back to free-form
                // hint in that case. OpenAi has a catalog so the Combo renders.
                const char* modelComboLabel = isLocalCompat ? "Model" : "OpenAI model";
                const char* modelFreeFormLabel = modelComboLabel;
                const char* modelHint = isLocalCompat ? "e.g. local-model, llama3, qwen2.5" : "";
                renderModelPicker(modelComboLabel, modelFreeFormLabel, modelHint, selectedKind, s_openAiModelBuf,
                                  sizeof(s_openAiModelBuf), d.cfg.AiModelOpenAi);
                const char* urlHint = isLocalCompat ? "http://127.0.0.1:1234 (LM Studio)" : "https://api.openai.com";
                if (ImGui::InputTextWithHint("Base URL", urlHint, s_baseUrlBuf, sizeof(s_baseUrlBuf))) {
                    d.cfg.AiBaseUrl = s_baseUrlBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
            } else if (selectedKind == AiProvider::Anthropic) {
                if (ImGui::InputText("Anthropic API key", s_anthropicKeyBuf, sizeof(s_anthropicKeyBuf),
                                     ImGuiInputTextFlags_Password)) {
                    d.cfg.AiAnthropicApiKey = s_anthropicKeyBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                renderModelPicker("Anthropic model", "Anthropic model", "", AiProvider::Anthropic, s_anthropicModelBuf,
                                  sizeof(s_anthropicModelBuf), d.cfg.AiModelAnthropic);
            } else if (selectedKind == AiProvider::OllamaNative) {
                renderModelPicker("Ollama model", "Ollama model", "e.g. llama3, qwen2.5, mistral",
                                  AiProvider::OllamaNative, s_ollamaModelBuf, sizeof(s_ollamaModelBuf),
                                  d.cfg.AiModelOllama);
                if (ImGui::InputTextWithHint("Ollama base URL", "http://localhost:11434", s_ollamaBaseUrlBuf,
                                             sizeof(s_ollamaBaseUrlBuf))) {
                    d.cfg.AiOllamaBaseUrl = s_ollamaBaseUrlBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
            } else if (selectedKind == AiProvider::DeepSeek) {
                if (ImGui::InputText("DeepSeek API key", s_deepseekKeyBuf, sizeof(s_deepseekKeyBuf),
                                     ImGuiInputTextFlags_Password)) {
                    d.cfg.AiDeepSeekApiKey = s_deepseekKeyBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                renderModelPicker("DeepSeek model", "DeepSeek model", "", AiProvider::DeepSeek, s_deepseekModelBuf,
                                  sizeof(s_deepseekModelBuf), d.cfg.AiModelDeepSeek);
                if (ImGui::InputTextWithHint("Base URL", "https://api.deepseek.com", s_deepseekBaseUrlBuf,
                                             sizeof(s_deepseekBaseUrlBuf))) {
                    d.cfg.AiDeepSeekBaseUrl = s_deepseekBaseUrlBuf;
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
            }

            // --- Default reasoning effort (all providers) ---
            // Stored as a string enum: "auto" | "low" | "medium" | "high". "auto"
            // omits the wire parameter (server picks). Forwarded as the
            // `reasoning_effort` body field by OpenAiClient; providers that
            // don't understand it ignore it. The chat-window header has a per-
            // turn Combo that overrides this default for one Send.
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
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
                    LOG_INFO("Preferences: AiReasoningEffort %s -> %s", d.cfg.AiReasoningEffort.c_str(),
                             kEffortIds[effortIdx]);
                    d.cfg.AiReasoningEffort = kEffortIds[effortIdx];
                    MarkPrefsDirty(d);
                    clearStaleTestResult();
                }
                ImGui::SetItemTooltip("OpenAI `reasoning_effort` body parameter for o-series / reasoning-tuned "
                                      "models. LM Studio + LocalAI pass it through to local reasoning models "
                                      "(Qwen3, gemma-3, etc.). Providers that don't understand the parameter "
                                      "ignore it.");
            }

            // --- agents.md harness (optional) ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextUnformatted("agents.md harness (optional)");
            ImGui::TextWrapped("Layered system prompt injected into every Assistant turn. Global layer defaults to "
                               "%%LOCALAPPDATA%%/Smatchet/agents.md when blank. Each layer capped at 64 KB.");
            ImGui::Spacing();
            if (ImGui::InputText("Global agents.md path", s_agentsMdGlobalBuf, sizeof(s_agentsMdGlobalBuf))) {
                d.cfg.AgentsMdGlobalPath = s_agentsMdGlobalBuf;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("Default %LOCALAPPDATA%/Smatchet/agents.md when blank. Override to point at a "
                                  "checked-in shared file.");
            if (ImGui::InputText("Project agents.md path (override)", s_projectAgentsMdBuf,
                                 sizeof(s_projectAgentsMdBuf))) {
                d.cfg.ProjectAgentsMdPath = s_projectAgentsMdBuf;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("When set, this exact path is used as the project layer. Leave blank to disable the "
                                  "project layer entirely unless Auto-discover is enabled below.");
            bool autoDiscover = d.cfg.AgentsMdAutoDiscoverProject;
            if (ImGui::Checkbox("Auto-discover project agents.md (walk up from cwd)", &autoDiscover)) {
                d.cfg.AgentsMdAutoDiscoverProject = autoDiscover;
                MarkPrefsDirty(d);
                if (app.HasAiAssistantController()) {
                    app.GetAiAssistantController().InvalidateAgentsMdCache();
                }
            }
            ImGui::SetItemTooltip("OFF (default): only the Global file + explicit Project path are used. ON: walks up "
                                  "the cwd chain looking for agents.md / AGENTS.md.");

            ImGui::EndTabItem();
        }
}

#endif // SMATCHET_WITH_AI