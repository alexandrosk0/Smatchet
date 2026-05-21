#include "AiAssistantController.h"

#if defined(SMATCHET_WITH_AI)

#include "AgentsMdLoader.h"
#include "AiChatTimestamp.h"
#include "AiClientFactory.h"
#include "AiContextBuilder.h"
#include "AiEndpointSanitize.h"
#include "AiModelSignature.h"
#include "AppController.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetUiSession.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

// `g_ui` is the UI-thread-owned UiDrawSession defined unconditionally in
// SmatchetUI.cpp (line 51). SmatchetUiSession.h only declares it `extern`
// behind `#if defined(SMATCHET_WITH_LUA_AUTOMATION)` because that header's
// other consumer (Lua glue) was the historical-only worker that needed it.
// Phase B's MainThreadDispatcher callbacks reach into the same global; we
// declare it directly here so the TU compiles regardless of the
// Lua-automation build flag.
extern UiDrawSession g_ui;

namespace {

AiProvider ProviderFromConfig(const TrackerConfig& cfg) {
    // Clamp identically to ConfigManager::Load — defence in depth in case
    // an out-of-range value sneaks through (e.g. cfg constructed in tests).
    switch (cfg.AiProviderKind) {
    case 0:
        return AiProvider::OpenAi;
    case 1:
        return AiProvider::Anthropic;
    case 2:
        return AiProvider::OllamaOpenAiCompat;
    case 3:
        return AiProvider::OllamaNative;
    case 4:
        return AiProvider::DeepSeek;
    default:
        return AiProvider::OpenAi;
    }
}

// Strip CR/LF/NUL from a header-bound value. libcurl will usually reject them
// already, but defense-in-depth: a user-typed `cfg.Ai*ApiKey` should never
// carry header-smuggling control characters into the outbound request.
std::string SanitizeHeaderValue(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    std::copy_if(v.begin(), v.end(), std::back_inserter(out),
                 [](char c) { return c != '\r' && c != '\n' && c != '\0'; });
    return out;
}

// Validate + (best-effort) normalise the user-configured endpoint URL. Returns
// the sanitised URL on success; empty + logs a structured warning on rejection
// (the empty string causes the provider client to fall back to its built-in
// default, which is the safe choice).
std::string SanitizeBaseUrlOrLog(const std::string& raw, const char* providerLabel) {
    std::string normalised;
    const smatchet::ai::pure::EndpointVerdict v = smatchet::ai::pure::SanitizeAiEndpointUrl(raw, normalised);
    if (v == smatchet::ai::pure::EndpointVerdict::Allowed)
        return normalised;
    LOG_ERROR("AiAssistantController: %s endpoint URL %s — falling back to provider default. Raw URL withheld.",
              providerLabel, smatchet::ai::pure::EndpointVerdictDescription(v));
    return std::string();
}

AiClientConfig BuildClientConfig(const TrackerConfig& cfg, AiProvider provider) {
    AiClientConfig out;
    switch (provider) {
    case AiProvider::Anthropic:
        out.ApiKey = SanitizeHeaderValue(cfg.AiAnthropicApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "anthropic");
        break;
    case AiProvider::OllamaNative:
        out.ApiKey.clear(); // Ollama-native has no API key
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiOllamaBaseUrl, "ollama");
        break;
    case AiProvider::OllamaOpenAiCompat:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        // OllamaOpenAi-compat uses the user's base URL (typically http://localhost:11434/v1)
        out.BaseUrl =
            SanitizeBaseUrlOrLog(cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl, "ollama-openai-compat");
        break;
    case AiProvider::DeepSeek:
        out.ApiKey = SanitizeHeaderValue(cfg.AiDeepSeekApiKey);
        // DeepSeek default endpoint when the user leaves the URL blank. Pass
        // the literal default through the sanitiser too — never bypass the
        // SanitizeAiEndpointUrl gate so a future redirect-block / scheme-pin
        // rule applies uniformly.
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com")
                                                                         : cfg.AiDeepSeekBaseUrl,
                                           "deepseek");
        break;
    case AiProvider::OpenAi:
    default:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "openai");
        break;
    }
    // Streaming chat replies from reasoning-tuned models (claude-opus-4-7,
    // o1-style, DeepSeek-R1, Qwen3) routinely exceed the AiClientConfig default
    // of 120s. cpr's `cpr::Timeout` is a total-envelope cap and fires regardless
    // of stream progress, so a 5-minute reply with 57 KB received gets killed
    // mid-stream with `cpr code 8 - Operation timed out`. Bump to 10 minutes
    // for the chat path; the user-driven Cancel atom is the right abort
    // mechanism for a genuinely stuck stream.
    out.TotalTimeoutMs = 600000;
    return out;
}

} // namespace

AiAssistantController::AiAssistantController(AppController& app) : app_(app) {
    const TrackerConfig cfg = ConfigManager::Load();
    const AiProvider provider = ProviderFromConfig(cfg);
    // Worker thread is not yet spawned, so the lock here is only for
    // memory-ordering hygiene; no contention is possible.
    std::lock_guard<std::mutex> lk(providerStateMutex_);
    cachedProvider_ = provider;
    client_ = AiClientFactory::MakeAiClient(provider);
    if (client_) {
        cachedProviderName_ = client_->GetProviderName();
        clientConfig_ = BuildClientConfig(cfg, provider);
    } else {
        // Phase D providers return null today. The controller stays alive so the
        // panel can render an error strip; Submit() will short-circuit until the
        // user picks a supported provider in Preferences.
        cachedProviderName_.clear();
    }
    worker_ = std::thread(&AiAssistantController::WorkerLoop, this);
}

std::string AiAssistantController::GetActiveProviderName() const {
    std::lock_guard<std::mutex> lk(providerStateMutex_);
    return cachedProviderName_;
}

AiAssistantController::~AiAssistantController() {
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        shuttingDown_ = true;
        if (currentCancel_) {
            currentCancel_->store(true, std::memory_order_release);
        }
    }
    queueCv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AiAssistantController::Submit(uint64_t turnGen, std::string prompt, std::vector<AiContextBlock> context,
                                   std::string modelOverride, std::string effortOverride) {
    // Snapshot cached state under lock — the worker thread may rewrite both pointer
    // and string concurrently when a new turn lands after the user switched provider
    // in Preferences. Reading `client_` raw or `cachedProviderName_` raw here is UB.
    std::string snapshotProviderName;
    bool haveClient = false;
    {
        std::lock_guard<std::mutex> lk(providerStateMutex_);
        haveClient = static_cast<bool>(client_);
        snapshotProviderName = cachedProviderName_;
    }
    if (!haveClient) {
        LOG_WARN("AiAssistantController::Submit dropped — no active client for provider '%s'.",
                 snapshotProviderName.c_str());
        // Returning false signals the UI's `dispatchSend` to NOT flip
        // `assistantInFlight = true`. Without this ack the Send button stuck
        // disabled for the lifetime of the app once Submit dropped a call.
        // The UI sets `assistantLastError` to a user-facing message on the
        // same tick so a visible error strip recovers the panel.
        return false;
    }
    LOG_INFO("AiAssistantController::Submit turnGen=%llu provider='%s' modelOverride='%s' effortOverride='%s' "
             "promptLen=%zu contextBlocks=%zu",
             static_cast<unsigned long long>(turnGen), snapshotProviderName.c_str(),
             modelOverride.empty() ? "<cfg>" : modelOverride.c_str(),
             effortOverride.empty() ? "<cfg>" : effortOverride.c_str(), prompt.size(), context.size());
    Request req;
    req.Prompt = std::move(prompt);
    req.Context = std::move(context);
    req.TurnGen = turnGen;
    req.ModelOverride = std::move(modelOverride);
    req.EffortOverride = std::move(effortOverride);
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (shuttingDown_) {
            LOG_WARN("AiAssistantController::Submit dropped — controller is shutting down (turnGen=%llu).",
                     static_cast<unsigned long long>(turnGen));
            return false;
        }
        // Replace cancel atom — each turn owns its own atom so a Cancel of the
        // previous turn cannot flip the next turn's flag (a race the shared_ptr
        // ownership model neutralises: worker captures the previous shared_ptr;
        // UI rebinds currentCancel_ to a fresh shared_ptr for the new turn).
        currentCancel_ = std::make_shared<std::atomic<bool>>(false);
        pending_.push_back(std::move(req));
    }
    queueCv_.notify_one();
    return true;
}

void AiAssistantController::Cancel() {
    std::lock_guard<std::mutex> lk(queueMutex_);
    if (currentCancel_) {
        currentCancel_->store(true, std::memory_order_release);
    }
}

void AiAssistantController::WorkerLoop() {
    while (true) {
        Request req;
        AiCancelToken cancel;
        {
            std::unique_lock<std::mutex> lk(queueMutex_);
            queueCv_.wait(lk, [this] { return shuttingDown_ || !pending_.empty(); });
            if (shuttingDown_ && pending_.empty()) {
                return;
            }
            req = std::move(pending_.front());
            pending_.pop_front();
            cancel = currentCancel_;
        }
        // Pre-cancel check — Cancel might have fired between Submit and WorkerLoop pickup.
        if (cancel && cancel->load(std::memory_order_acquire)) {
            state_.store(State::Cancelled, std::memory_order_release);
            continue;
        }
        state_.store(State::InFlight, std::memory_order_release);
        RunRequest(req, cancel);
    }
}

void AiAssistantController::RunRequest(const Request& req, const AiCancelToken& cancel) {
    // Worker-thread provider refresh. The user may have switched provider (and/or
    // its API key / base URL / model id) in Preferences between turns. Rebuild
    // `client_` only when the provider enum changed (avoids per-turn churn for
    // the common URL/key/model edit), but always rebuild `clientConfig_` so a
    // fresh key or URL takes effect on the very next turn.
    {
        const TrackerConfig refreshCfg = ConfigManager::Load();
        const AiProvider refreshProvider = ProviderFromConfig(refreshCfg);
        std::lock_guard<std::mutex> lk(providerStateMutex_);
        if (refreshProvider != cachedProvider_ || !client_) {
            std::unique_ptr<IAiClient> rebuilt = AiClientFactory::MakeAiClient(refreshProvider);
            if (rebuilt) {
                LOG_INFO("AiAssistantController: provider switched %d -> %d (client '%s' -> '%s')",
                         static_cast<int>(cachedProvider_), static_cast<int>(refreshProvider),
                         cachedProviderName_.c_str(), rebuilt->GetProviderName().c_str());
                client_ = std::move(rebuilt);
                cachedProvider_ = refreshProvider;
                cachedProviderName_ = client_->GetProviderName();
            } else {
                LOG_ERROR("AiAssistantController: MakeAiClient returned null for provider %d — "
                          "turn aborted, retaining prior client.",
                          static_cast<int>(refreshProvider));
            }
        }
        clientConfig_ = BuildClientConfig(refreshCfg, cachedProvider_);
    }
    if (!client_) {
        state_.store(State::Errored, std::memory_order_release);
        return;
    }

    AiChatRequest chatReq;
    {
        // Re-read model + reasoning effort each turn so a Preferences change
        // while a turn is queued takes effect on the next Submit, without a
        // per-turn config snapshot leaking into the request struct. Per-turn
        // overrides (chat-window Model + Effort Combos) win when non-empty;
        // otherwise the live Preferences value applies.
        const TrackerConfig cfg = ConfigManager::Load();
        const AiProvider provider = ProviderFromConfig(cfg);
        if (!req.ModelOverride.empty()) {
            chatReq.Model = req.ModelOverride;
        } else {
            switch (provider) {
            case AiProvider::Anthropic:
                chatReq.Model = cfg.AiModelAnthropic;
                break;
            case AiProvider::OllamaNative:
            case AiProvider::OllamaOpenAiCompat:
                chatReq.Model = cfg.AiModelOllama;
                break;
            case AiProvider::DeepSeek:
                chatReq.Model = cfg.AiModelDeepSeek;
                break;
            case AiProvider::OpenAi:
            default:
                chatReq.Model = cfg.AiModelOpenAi;
                break;
            }
        }
        chatReq.ReasoningEffort = req.EffortOverride.empty() ? cfg.AiReasoningEffort : req.EffortOverride;
        // Use the canonical provider enum string for both the log and the F2 signature.
        // The client-side `GetProviderName()` returns the BACKING client identity
        // (`OpenAiClient` serves OpenAi, OllamaOpenAiCompat, and DeepSeek — all three
        // print as "openai") which would mask provider transitions for the auto-clear.
        const std::string providerLabel = AiClientFactory::ProviderToString(provider);
        LOG_INFO("AiAssistantController::RunRequest turnGen=%llu provider='%s' model='%s' effort='%s' "
                 "systemPromptCap=64KB",
                 static_cast<unsigned long long>(req.TurnGen), providerLabel.c_str(), chatReq.Model.c_str(),
                 chatReq.ReasoningEffort.c_str());

        // F2 — model-change auto-clear. Compose "<provider>|<effective-model>" and
        // compare against the previous turn's signature. The per-turn model override
        // (chat-window Combo) composes naturally because chatReq.Model already
        // reflects override-wins-cfg above. ReasoningEffort deliberately omitted —
        // effort-only changes must NOT discard chat history (decision Q in plan).
        const smatchet::ai::ModelSignatureChangeResult sigResult =
            smatchet::ai::DetectModelChange(lastModelSignature_, providerLabel, chatReq.Model);
        if (sigResult.ShouldClear) {
            LOG_INFO("AiAssistantController: model signature changed ('%s' -> '%s') — posting chat clear",
                     lastModelSignature_.c_str(), sigResult.NewSignature.c_str());
            // Capture by value: the task may run arbitrarily later on the UI thread
            // and must remain safe even if the controller (or worker locals) go away
            // between post and drain. No references to worker-thread state.
            app_.mainThreadDispatcher.PostToMainThread([]() {
                g_ui.assistantHistory.clear();
                g_ui.assistantStreamBuf.clear();
                g_ui.assistantLastError = "[model changed - chat cleared]";
            });
        }
        lastModelSignature_ = sigResult.NewSignature;
    }

    // System-prompt assembly: agents.md prefix + "## Current Smatchet context" header
    // (when any block has content) + each enabled block wrapped in
    // `<smatchet_context block="...">...</smatchet_context>` tags. File I/O for
    // agents.md happens here on the worker thread to honour pillar 2 (no synchronous
    // I/O reaches the UI thread). Layers are capped at 64 KB each by AgentsMdLoader,
    // so the total system-prompt overhead is bounded.
    chatReq.SystemPrompt.clear();
    {
        // agents.md cache: avoid re-reading the (up to 64 KB × 2 layers) blob on
        // every turn. Re-reads happen only when the cache is invalidated
        // (`InvalidateAgentsMdCache` from the Preferences UI) or when the
        // configured paths change between turns.
        const TrackerConfig agentsCfg = ConfigManager::Load();
        std::string agentsMd;
        {
            std::lock_guard<std::mutex> lk(agentsMdMutex_);
            const bool pathsMatch = (agentsMdCachedGlobalPath_ == agentsCfg.AgentsMdGlobalPath) &&
                                    (agentsMdCachedProjectPath_ == agentsCfg.ProjectAgentsMdPath);
            if (agentsMdCacheValid_.load(std::memory_order_acquire) && pathsMatch) {
                agentsMd = agentsMdCachedBody_;
            } else {
                agentsMd = AgentsMdLoader::LoadLayered(agentsCfg.AgentsMdGlobalPath, agentsCfg.ProjectAgentsMdPath,
                                                       agentsCfg.AgentsMdAutoDiscoverProject);
                agentsMdCachedBody_ = agentsMd;
                agentsMdCachedGlobalPath_ = agentsCfg.AgentsMdGlobalPath;
                agentsMdCachedProjectPath_ = agentsCfg.ProjectAgentsMdPath;
                agentsMdCacheValid_.store(true, std::memory_order_release);
            }
        }
        if (!agentsMd.empty()) {
            chatReq.SystemPrompt.append(agentsMd);
            if (chatReq.SystemPrompt.back() != '\n') {
                chatReq.SystemPrompt.push_back('\n');
            }
            chatReq.SystemPrompt.append("\n---\n\n");
        }
    }

    // Fetch the audit-trail block on the worker thread when requested. The UI-side
    // BuildSendContext deliberately skips audit (the SQLite + filesystem read is too
    // heavy for the UI frame) and instead emits an `AuditTrail`-kind block with the
    // body `"__SMATCHET_DEFERRED__"` (literal sentinel). We rewrite it here using the
    // canonical pure helper. Disabled audit-trail blocks have empty bodies and are
    // skipped by the block-emit loop below, so this is a no-op when the user
    // disabled the toggle.
    std::vector<AiContextBlock> resolvedContext;
    resolvedContext.reserve(req.Context.size());
    for (const auto& block : req.Context) {
        if (block.Kind == AiContextBlockKind::AuditTrail && block.Body == "__SMATCHET_DEFERRED__") {
            std::string body;
            try {
                std::vector<nlohmann::json> events =
                    BackendAuditTrail::ReadRecentEvents(AiContextBuilder::kAuditCap, nullptr);
                std::vector<std::string> lines;
                lines.reserve(events.size());
                for (const auto& ev : events) {
                    lines.push_back(ev.dump());
                }
                body = AiContextBuilder::BuildAuditTrailBody(lines);
            } catch (const std::exception& ex) {
                LOG_WARN("AiAssistantController: audit-trail fetch failed: %s", ex.what());
            }
            AiContextBlock resolved = block;
            resolved.Body = std::move(body);
            resolvedContext.push_back(std::move(resolved));
        } else {
            resolvedContext.push_back(block);
        }
    }
    {
        std::string contextSection;
        for (const auto& block : resolvedContext) {
            if (block.Body.empty()) {
                continue;
            }
            if (!contextSection.empty()) {
                contextSection.push_back('\n');
            }
            contextSection.append("<smatchet_context block=\"");
            contextSection.append(block.Name);
            contextSection.append("\">\n");
            contextSection.append(block.Body);
            if (block.Body.back() != '\n') {
                contextSection.push_back('\n');
            }
            contextSection.append("</smatchet_context>\n");
        }
        if (!contextSection.empty()) {
            chatReq.SystemPrompt.append("## Current Smatchet context\n\n");
            chatReq.SystemPrompt.append(contextSection);
        }
    }
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = req.Prompt;
    chatReq.History.push_back(std::move(userMsg));

    // Capture turnGen by value so stale dispatcher tasks (after a newer Submit
    // or Cancel) can be dropped on the UI side via assistantTurnGen comparison.
    const uint64_t turnGen = req.TurnGen;
    AppController* app = &app_;

    auto onDelta = [app, turnGen](const AiStreamDelta& delta) {
        const std::string chunk = delta.TokenChunk;
        const bool isFinal = delta.IsFinal;
        app->mainThreadDispatcher.PostToMainThread([turnGen, chunk, isFinal]() {
            if (g_ui.assistantTurnGen != turnGen) {
                return; // stale callback — Cancel or newer Submit raced us
            }
            if (!chunk.empty()) {
                // Hard cap on assistantStreamBuf — defense-in-depth against a
                // runaway provider that keeps streaming past any reasonable
                // response size. 4 MiB mirrors the SSE/NDJSON parser caps.
                constexpr std::size_t kMaxStreamBufBytes = 4u * 1024u * 1024u;
                if (g_ui.assistantStreamBuf.size() < kMaxStreamBufBytes) {
                    const std::size_t room = kMaxStreamBufBytes - g_ui.assistantStreamBuf.size();
                    if (chunk.size() <= room) {
                        g_ui.assistantStreamBuf.append(chunk);
                    } else {
                        g_ui.assistantStreamBuf.append(chunk, 0, room);
                        g_ui.assistantStreamBuf.append("\n[truncated — response exceeded 4 MiB cap]");
                    }
                }
            }
            if (isFinal) {
                AiMessage assistantMsg;
                assistantMsg.Role = "assistant";
                assistantMsg.Content = g_ui.assistantStreamBuf;
                assistantMsg.CreatedAtUnixMs = smatchet::ai::NowUnixMs();
                // Phase 3 of ai-chat-claude-desktop-parity. Persist snapshot taken
                // before move; row-id slot grows in lock-step so the worker callback
                // can backfill the SQLite id. Gated on hydrated so a final-flush from
                // a turn that completed during the hydrate window doesn't write a
                // duplicate that the hydrate will then re-load.
                AiMessage persistCopy = assistantMsg;
                const std::size_t newIdx = g_ui.assistantHistory.size();
                g_ui.assistantHistory.push_back(std::move(assistantMsg));
                g_ui.assistantHistoryRowIds.push_back(-1);
                if (g_ui.assistantHistoryHydrated) {
                    smatchet::ai::chat_persist::Op appendOp;
                    appendOp.kind = smatchet::ai::chat_persist::OpKind::Append;
                    appendOp.message = std::move(persistCopy);
                    appendOp.messageIndex = newIdx;
                    smatchet::ai::chat_persist::Enqueue(std::move(appendOp));
                    smatchet::ai::chat_persist::Op trimOp;
                    trimOp.kind = smatchet::ai::chat_persist::OpKind::Trim;
                    trimOp.trimCap = static_cast<std::size_t>(
                        g_ui.cfg.AssistantHistoryMaxRows > 0 ? g_ui.cfg.AssistantHistoryMaxRows : 500);
                    smatchet::ai::chat_persist::Enqueue(std::move(trimOp));
                }
                g_ui.assistantStreamBuf.clear();
                g_ui.assistantInFlight = false;
            }
        });
    };

    auto onError = [this, app, turnGen](const AiStreamError& err) {
        const int httpStatus = err.HttpStatus;
        const std::string message = err.Message;
        const bool wasCancelled = err.WasCancelled;
        if (wasCancelled) {
            state_.store(State::Cancelled, std::memory_order_release);
            LOG_INFO("AiAssistantController: turn %llu cancelled.", static_cast<unsigned long long>(turnGen));
        } else {
            state_.store(State::Errored, std::memory_order_release);
            LOG_ERROR("AiAssistantController: turn %llu errored httpStatus=%d message='%s'",
                      static_cast<unsigned long long>(turnGen), httpStatus, message.c_str());
        }
        app->mainThreadDispatcher.PostToMainThread([turnGen, httpStatus, message, wasCancelled]() {
            if (g_ui.assistantTurnGen != turnGen) {
                return;
            }
            g_ui.assistantInFlight = false;
            if (wasCancelled) {
                // Scenario 3 contract — partial text retained in history.
                if (!g_ui.assistantStreamBuf.empty()) {
                    AiMessage assistantMsg;
                    assistantMsg.Role = "assistant";
                    assistantMsg.Content = g_ui.assistantStreamBuf + "\n[cancelled]";
                    assistantMsg.CreatedAtUnixMs = smatchet::ai::NowUnixMs();
                    // Phase 3 of ai-chat-claude-desktop-parity. Same enqueue shape as
                    // the normal-finalisation site; cancelled-with-partial-text rows
                    // persist so the next launch shows the user where they stopped.
                    AiMessage persistCopy = assistantMsg;
                    const std::size_t newIdx = g_ui.assistantHistory.size();
                    g_ui.assistantHistory.push_back(std::move(assistantMsg));
                    g_ui.assistantHistoryRowIds.push_back(-1);
                    if (g_ui.assistantHistoryHydrated) {
                        smatchet::ai::chat_persist::Op appendOp;
                        appendOp.kind = smatchet::ai::chat_persist::OpKind::Append;
                        appendOp.message = std::move(persistCopy);
                        appendOp.messageIndex = newIdx;
                        smatchet::ai::chat_persist::Enqueue(std::move(appendOp));
                        smatchet::ai::chat_persist::Op trimOp;
                        trimOp.kind = smatchet::ai::chat_persist::OpKind::Trim;
                        trimOp.trimCap = static_cast<std::size_t>(
                            g_ui.cfg.AssistantHistoryMaxRows > 0 ? g_ui.cfg.AssistantHistoryMaxRows : 500);
                        smatchet::ai::chat_persist::Enqueue(std::move(trimOp));
                    }
                }
                g_ui.assistantStreamBuf.clear();
                g_ui.assistantLastError.clear();
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "API Error: %d ", httpStatus);
                g_ui.assistantLastError = std::string(buf) + message;
                g_ui.assistantStreamBuf.clear();
            }
        });
    };

    // Trust the cancel atom captured in WorkerLoop under the queueMutex_. The
    // earlier "re-acquire under lock; fallback to fresh atom if currentCancel_
    // was reset" path could silently swallow a Cancel that arrived after the
    // worker popped its turn but before this re-acquire — Submit() guarantees a
    // non-null currentCancel_ exists by the time WorkerLoop reads it, so the
    // fallback was dead code that masked a real race.
    AiCancelToken liveCancel = cancel;
    if (!liveCancel) {
        liveCancel = std::make_shared<std::atomic<bool>>(false);
    }

    try {
        client_->SendStreaming(clientConfig_, chatReq, onDelta, onError, liveCancel);
    } catch (const std::exception& ex) {
        LOG_ERROR("AiAssistantController: SendStreaming threw: %s", ex.what());
        AiStreamError err;
        err.HttpStatus = 0;
        err.Message = std::string("client exception: ") + ex.what();
        err.WasCancelled = false;
        onError(err);
    } catch (...) {
        LOG_ERROR("AiAssistantController: SendStreaming threw unknown exception");
        AiStreamError err;
        err.HttpStatus = 0;
        err.Message = "client unknown exception";
        err.WasCancelled = false;
        onError(err);
    }

    // Worker keeps running for the next request; UI thread transitions State::Idle
    // when it observes a stable assistantInFlight=false.
    if (state_.load(std::memory_order_acquire) == State::InFlight) {
        state_.store(State::Idle, std::memory_order_release);
    }
}

void AiAssistantController::AddAiContext(const AiContextBlock& block) {
    std::lock_guard<std::mutex> lk(luaContextMutex_);
    luaContext_.push_back(block);
}

void AiAssistantController::ClearAiContext() {
    std::lock_guard<std::mutex> lk(luaContextMutex_);
    luaContext_.clear();
}

std::vector<AiContextBlock> AiAssistantController::GetAiContext() const {
    std::lock_guard<std::mutex> lk(luaContextMutex_);
    return luaContext_;
}

void AiAssistantController::InvalidateAgentsMdCache() { agentsMdCacheValid_.store(false, std::memory_order_release); }

#endif // SMATCHET_WITH_AI
