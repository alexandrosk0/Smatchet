#include "AiPrefsTestConnection.h"

#include "AiPrefsTestConnectionPure.h"

#if defined(SMATCHET_WITH_AI)

#include "AiClientFactory.h"
#include "AiEndpointPolicy.h"
#include "AiEndpointSanitize.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "IAiClient.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "MainThreadDispatcher.h"
#include "SmatchetUiSession.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {

// Worker-thread probe body: reachability check + a 1-token chat handshake against
// the configured model. Returns empty on success; a populated error message
// otherwise. Catches every exception (third-party cpr/libcurl + SSE parser) so a
// throw never escapes the background task and calls std::terminate.
std::string RunProbe(AiProvider provider, const AiClientConfig& clientCfg, const std::string& modelId,
                     const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::string errMsg;
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
        // SMATCHET_DEVIATION(rule=duplication; reason=test-connection probe twins (AiPrefsTestConnection vs Preferences
        // UI worker) predate this pass; the identical localized catch-handling keeps both twins consistent until the
        // planned twin dedup; owner=user-text-error-pass; revisit=2026-09-30)
    } catch (const std::exception& ex) {
        LOG_WARN("AiPrefsTestConnection: %s", ex.what());
        errMsg = SmatchetLocalization::T("prefs.assistant.test_internal_error",
                                         "the request could not be completed — check the endpoint URL and try again");
    } catch (...) {
        LOG_WARN("AiPrefsTestConnection: unknown exception");
        errMsg = SmatchetLocalization::T("prefs.assistant.test_internal_error",
                                         "the request could not be completed — check the endpoint URL and try again");
    }
    return errMsg;
}

// UI-thread result dispatch: short-circuit on cancel, otherwise surface
// Verified / Failed into the g_ui prefs-test fields and persist any defaulted
// base URL back into cfg.
void PublishProbeResult(const std::string& errMsg, AiProvider provider, const std::string& defaultedBaseUrl,
                        const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (cancel && cancel->load()) {
        return;
    }
    g_ui.assistantPrefsTestInFlight = false;
    if (errMsg.empty()) {
        LOG_INFO("AiPrefsTestConnection: VERIFIED providerKind=%d defaultedBaseUrl='%s'", static_cast<int>(provider),
                 defaultedBaseUrl.c_str());
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
}

} // namespace

namespace AiPrefsTestConnection {

void TriggerProbe(UiDrawSession& d, AppController& app, AiProvider provider) {
    LOG_INFO("AiPrefsTestConnection::TriggerProbe start providerKind=%d", static_cast<int>(provider));
    TrackerConfig probeCfg = d.cfg; // snapshot by value
    d.assistantPrefsTestInFlight = true;
    d.assistantPrefsTestResult = "Testing...";
    d.assistantPrefsTestResultType = 0;
    d.assistantPrefsTestCancel = std::make_shared<std::atomic<bool>>(false);
    auto cancel = d.assistantPrefsTestCancel;

    // Same per-provider endpoint policy the live request uses (host pin + insecure-http
    // consent) so Test-connection accepts/rejects exactly what the real turn would. The
    // credential-slot / default-URL / sanitiser decisions live in the tested pure half.
    const smatchet::ai::pure::EndpointPolicy policy = smatchet::ai::EndpointPolicyForProvider(probeCfg, provider);
    const AiPrefsTestConnectionPure::ProbePlan plan = AiPrefsTestConnectionPure::PlanProbe(probeCfg, provider, policy);
    const AiClientConfig clientCfg = plan.ClientCfg;
    const std::string modelId = plan.ModelId;
    const std::string defaultedBaseUrl = plan.DefaultedBaseUrl;

    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;
    // Joined background-task pool, not a raw detached thread — the no-detach lint forbids that.
    // Joined at shutdown, so the &dispatcher capture stays valid for the task's whole life.
    app.LaunchBackgroundTask([provider, clientCfg, cancel, defaultedBaseUrl, modelId, &dispatcher]() {
        const std::string errMsg = RunProbe(provider, clientCfg, modelId, cancel);
        dispatcher.PostToMainThread([errMsg, cancel, provider, defaultedBaseUrl]() {
            PublishProbeResult(errMsg, provider, defaultedBaseUrl, cancel);
        });
    });
}

} // namespace AiPrefsTestConnection

#endif // SMATCHET_WITH_AI
