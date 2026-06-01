#include "AiPrefsTestConnection.h"

#if defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiEndpointSanitize.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetUiSession.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace AiPrefsTestConnection {

void TriggerProbe(UiDrawSession& d, AppController& app, AiProvider provider) {
    LOG_INFO("AiPrefsTestConnection::TriggerProbe start providerKind=%d", static_cast<int>(provider));
    TrackerConfig probeCfg = d.cfg; // snapshot by value
    d.assistantPrefsTestInFlight = true;
    d.assistantPrefsTestResult = "Testing...";
    d.assistantPrefsTestResultType = 0;
    d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = d.assistantPrefsTestCancel;

    // Provider-aware ApiKey / BaseUrl / ModelId pick — mirrors `BuildClientConfig`
    // + `ResolveModelId` in `AiAssistantController.cpp`.
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

    // When the configured base URL is empty for a local provider, fall back to the
    // canonical default so the user can click Test connection right after picking
    // the provider — no manual URL entry needed. The default is also recorded so
    // the success callback can persist it back into cfg.
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

    // Strip header-smuggling control characters from the key (cheap defence-in-depth;
    // libcurl rejects them too).
    std::string sanitisedKey;
    sanitisedKey.reserve(apiKey.size());
    std::copy_if(apiKey.begin(), apiKey.end(), std::back_inserter(sanitisedKey),
                 [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
    std::string sanitisedBase;
    if (!baseUrl.empty()) {
        std::string normalised;
        const smatchet::ai::pure::EndpointVerdict v = smatchet::ai::pure::SanitizeAiEndpointUrl(baseUrl, normalised);
        if (v == smatchet::ai::pure::EndpointVerdict::Allowed) {
            sanitisedBase = normalised;
        } else {
            LOG_WARN("AiPrefsTestConnection: endpoint URL %s; falling back to provider default.",
                     smatchet::ai::pure::EndpointVerdictDescription(v));
        }
    }
    AiClientConfig clientCfg;
    clientCfg.ApiKey = sanitisedKey;
    clientCfg.BaseUrl = sanitisedBase;
    clientCfg.ConnectTimeoutMs = 5000;
    clientCfg.TotalTimeoutMs = 15000;

    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
    // Joined background-task pool, not a raw detached thread — the no-detach lint forbids that.
    // Joined at shutdown, so the &dispatcher capture stays valid for the task's whole life.
    app.LaunchBackgroundTask([provider, clientCfg, cancel, defaultedBaseUrl, modelId, &dispatcher]() {
        std::string errMsg;
        // Defensive try/catch — `MakeAiClient` / `ProbeReachability` / `SendStreaming` all
        // run third-party transport (cpr/libcurl) + SSE parser code. An uncaught exception
        // would propagate out of the detached thread and call `std::terminate`. Trap it,
        // surface as a failure result via the existing dispatcher path so UI state recovers.
        try {
            std::unique_ptr<IAiClient> client = AiClientFactory::MakeAiClient(provider);
            if (!client) {
                errMsg = "Provider not available in this build.";
            } else {
                // Step 1: reachability — server alive + auth accepted on the listing endpoint.
                errMsg = client->ProbeReachability(clientCfg);
                // Step 2: real chat handshake — sends a 1-token "ping" against the configured
                // model so model-not-found / chat-disabled / missing-loaded-model errors
                // surface BEFORE the user types their first real prompt.
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
                LOG_INFO("AiPrefsTestConnection: VERIFIED providerKind=%d defaultedBaseUrl='%s'",
                         static_cast<int>(provider), defaultedBaseUrl.c_str());
                g_ui.assistantPrefsTestResult = "Verified";
                g_ui.assistantPrefsTestResultType = 1;
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
                LOG_ERROR("AiPrefsTestConnection: FAILED providerKind=%d errMsg='%s'", static_cast<int>(provider),
                          errMsg.c_str());
                g_ui.assistantPrefsTestResult = std::string("Failed: ") + errMsg;
                g_ui.assistantPrefsTestResultType = 2;
            }
        });
    });
}

} // namespace AiPrefsTestConnection

#endif // SMATCHET_WITH_AI
