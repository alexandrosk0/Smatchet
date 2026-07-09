#include "AiAssistantController.h"

#if defined(SMATCHET_WITH_AI)

#include "AgentsMdLoader.h"
#include "AiChatTimestamp.h"
#include "AiClientFactory.h"
#include "AiContextBuilder.h"
#include "AiEndpointPolicy.h"
#include "AiEndpointSanitize.h"
#include "AiModelSignature.h"
#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IAiAssistantUiState.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetChatPersistWorker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <nlohmann/json.hpp>
#include <utility>

// The streaming worker→UI hand-off reaches the chat UI state through IAiAssistantUiState
// (uiState_), NOT the global g_ui directly: production binds the process-lifetime
// GetGlobalAiAssistantUiState() adapter (so a dispatcher task draining after this controller
// is destroyed stays safe), while the ThreadSanitizer test binds a fake — keeping this TU
// free of the ImGui-bound UiDrawSession. See docs/plans/active/tsan-imgui-linked-target.md.

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

// Validate + (best-effort) normalise the user-configured endpoint URL against
// `policy`. Returns the sanitised URL on success; empty + logs a structured
// warning on rejection (the empty string causes the provider client to fall
// back to its built-in default, which is the safe choice).
std::string SanitizeBaseUrlOrLog(const std::string& raw, const char* providerLabel,
                                 const smatchet::ai::pure::EndpointPolicy& policy) {
    std::string normalised;
    const smatchet::ai::pure::EndpointVerdict v = smatchet::ai::pure::SanitizeAiEndpointUrl(raw, policy, normalised);
    if (v == smatchet::ai::pure::EndpointVerdict::Allowed)
        return normalised;
    LOG_ERROR("AiAssistantController: %s endpoint URL %s — falling back to provider default. Raw URL withheld.",
              providerLabel, smatchet::ai::pure::EndpointVerdictDescription(v));
    return std::string();
}

AiClientConfig BuildClientConfig(const TrackerConfig& cfg, AiProvider provider) {
    AiClientConfig out;
    const smatchet::ai::pure::EndpointPolicy policy = smatchet::ai::EndpointPolicyForProvider(cfg, provider);
    switch (provider) {
    case AiProvider::Anthropic:
        out.ApiKey = SanitizeHeaderValue(cfg.AiAnthropicApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "anthropic", policy);
        break;
    case AiProvider::OllamaNative:
        out.ApiKey.clear(); // Ollama-native has no API key
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiOllamaBaseUrl, "ollama", policy);
        break;
    case AiProvider::OllamaOpenAiCompat:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        // OllamaOpenAi-compat uses the user's base URL (typically http://localhost:11434/v1)
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl.empty() ? cfg.AiOllamaBaseUrl : cfg.AiBaseUrl,
                                           "ollama-openai-compat", policy);
        break;
    case AiProvider::DeepSeek:
        out.ApiKey = SanitizeHeaderValue(cfg.AiDeepSeekApiKey);
        // DeepSeek default endpoint when the user leaves the URL blank. Pass
        // the literal default through the sanitiser too — never bypass the
        // SanitizeAiEndpointUrl gate so a future redirect-block / scheme-pin
        // rule applies uniformly.
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiDeepSeekBaseUrl.empty() ? std::string("https://api.deepseek.com")
                                                                         : cfg.AiDeepSeekBaseUrl,
                                           "deepseek", policy);
        break;
    case AiProvider::OpenAi:
    default:
        out.ApiKey = SanitizeHeaderValue(cfg.AiApiKey);
        out.BaseUrl = SanitizeBaseUrlOrLog(cfg.AiBaseUrl, "openai", policy);
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

AiAssistantController::AiAssistantController(MainThreadDispatcher& dispatcher, IAiAssistantUiState& uiState)
    : dispatcher_(dispatcher), uiState_(uiState) {
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
        // Also flip every still-queued turn's own token — CPP_CODE_AUDIT.md #4:
        // without this, a turn that hasn't been popped by WorkerLoop yet would
        // stream to completion after currentCancel_ moves on to it (a shutdown
        // hang, not just a Cancel no-op).
        for (Request& queued : pending_) {
            if (queued.Cancel) {
                queued.Cancel->store(true, std::memory_order_release);
            }
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
    // Each queued turn owns its own cancel token from the moment it's created —
    // CPP_CODE_AUDIT.md #4: the token must NOT be shared/rebound via currentCancel_
    // here, or a later Submit() would silently steal the in-flight field out from
    // under an earlier turn that's still streaming.
    req.Cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        if (shuttingDown_) {
            LOG_WARN("AiAssistantController::Submit dropped — controller is shutting down (turnGen=%llu).",
                     static_cast<unsigned long long>(turnGen));
            return false;
        }
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
    // Also flip every still-queued turn's own token, not just the in-flight one —
    // otherwise a Cancel() that lands while a turn is queued but not yet popped
    // by WorkerLoop would be silently dropped for that turn (CPP_CODE_AUDIT.md #4).
    for (Request& queued : pending_) {
        if (queued.Cancel) {
            queued.Cancel->store(true, std::memory_order_release);
        }
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
            cancel = req.Cancel;
            // Publish this turn's token as THE in-flight token, under the same lock
            // Cancel()/the destructor take — so a Cancel() that arrives right after
            // this pop always reaches the turn that's actually about to run, never a
            // stale or not-yet-existing token (CPP_CODE_AUDIT.md #4).
            currentCancel_ = cancel;
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
    // Thin sequence over the worker-thread phases. First refresh the provider,
    // then build the chat payload with model, effort, system prompt and history,
    // then stream the request and dispatch deltas back to the UI thread.
    //
    // Single per-turn config snapshot: all three phase helpers below ran their
    // own `ConfigManager::Load()` previously (3 reads per turn). They all run
    // sequentially on this worker thread within one RunRequest, so a single
    // snapshot taken here is identical to the prior back-to-back reads — same
    // config values, loaded once — and is threaded through by const&.
    const TrackerConfig cfg = ConfigManager::Load();
    if (!RefreshProviderForTurn(cfg)) {
        state_.store(State::Errored, std::memory_order_release);
        return;
    }

    AiChatRequest chatReq;
    ResolveModelAndEffort(cfg, req, chatReq);
    BuildChatPayload(cfg, req, chatReq);

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
    StreamAndDispatch(chatReq, liveCancel, req.TurnGen);
}

bool AiAssistantController::RefreshProviderForTurn(const TrackerConfig& cfg) {
    // Worker-thread provider refresh. The user may have switched provider (and/or
    // its API key / base URL / model id) in Preferences between turns. Rebuild
    // `client_` only when the provider enum changed (avoids per-turn churn for
    // the common URL/key/model edit), but always rebuild `clientConfig_` so a
    // fresh key or URL takes effect on the very next turn. Reads the single
    // per-turn snapshot taken in RunRequest.
    const AiProvider refreshProvider = ProviderFromConfig(cfg);
    std::lock_guard<std::mutex> lk(providerStateMutex_);
    if (refreshProvider != cachedProvider_ || !client_) {
        std::unique_ptr<IAiClient> rebuilt = AiClientFactory::MakeAiClient(refreshProvider);
        if (rebuilt) {
            LOG_INFO("AiAssistantController: provider switched %d -> %d (client '%s' -> '%s')",
                     static_cast<int>(cachedProvider_), static_cast<int>(refreshProvider), cachedProviderName_.c_str(),
                     rebuilt->GetProviderName().c_str());
            client_ = std::move(rebuilt);
            cachedProvider_ = refreshProvider;
            cachedProviderName_ = client_->GetProviderName();
        } else {
            // Fail CLOSED: a null rebuild means the new provider could not be
            // constructed (missing key/url/model). Dropping the stale client_ and
            // returning false aborts the turn with a clear error rather than
            // silently routing it through the previous provider (#825).
            LOG_ERROR("AiAssistantController: MakeAiClient returned null for provider %d — "
                      "turn aborted, clearing client (fail closed).",
                      static_cast<int>(refreshProvider));
            client_.reset();
            cachedProvider_ = refreshProvider;
            cachedProviderName_.clear();
        }
    }
    clientConfig_ = BuildClientConfig(cfg, cachedProvider_);
    return static_cast<bool>(client_);
}

void AiAssistantController::ResolveModelAndEffort(const TrackerConfig& cfg, const Request& req,
                                                  AiChatRequest& chatReq) {
    // Model + reasoning effort for this turn come from the single per-turn config
    // snapshot taken in RunRequest. Per-turn overrides (chat-window Model + Effort
    // Combos) win when non-empty; otherwise the snapshot Preferences value applies.
    const AiProvider provider = ProviderFromConfig(cfg);
    chatReq.Model = smatchet::ai::pure::ResolveChatModel(provider, req.ModelOverride, cfg.AiModelOpenAi,
                                                         cfg.AiModelAnthropic, cfg.AiModelOllama, cfg.AiModelDeepSeek);
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
        IAiAssistantUiState* ui = &uiState_;
        dispatcher_.PostToMainThread([ui]() {
            ui->AssistantHistory().clear();
            // Parallel to AssistantHistory (rowIds[i] is the SQLite id for history[i]) — must clear
            // in lock-step or the next append misaligns history indices against persisted row ids.
            ui->AssistantHistoryRowIds().clear();
            ui->AssistantStreamBuf().clear();
            ui->AssistantLastError() = "[model changed - chat cleared]";
        });
    }
    lastModelSignature_ = sigResult.NewSignature;
}

std::vector<AiContextBlock> AiAssistantController::ResolveDeferredContext(const Request& req) {
    // Fetch the audit-trail block on the worker thread when requested. The UI-side
    // BuildSendContext deliberately skips audit (the SQLite + filesystem read is too
    // heavy for the UI frame) and instead emits an `AuditTrail`-kind block with the
    // body `"__SMATCHET_DEFERRED__"` (literal sentinel). We rewrite it here using the
    // canonical pure helper. Disabled audit-trail blocks have empty bodies and are
    // skipped by the composer, so this is a no-op when the user disabled the toggle.
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
    return resolvedContext;
}

void AiAssistantController::BuildChatPayload(const TrackerConfig& cfg, const Request& req, AiChatRequest& chatReq) {
    // System-prompt assembly: agents.md prefix + "## Current Smatchet context" header
    // (when any block has content) + each enabled block wrapped in
    // `<smatchet_context block="...">...</smatchet_context>` tags. File I/O for
    // agents.md happens here on the worker thread to honour pillar 2 (no synchronous
    // I/O reaches the UI thread). Layers are capped at 64 KB each by AgentsMdLoader,
    // so the total system-prompt overhead is bounded.
    std::string agentsMd;
    {
        // agents.md cache: avoid re-reading the (up to 64 KB × 2 layers) blob on
        // every turn. Re-reads happen only when the cache is invalidated
        // (`InvalidateAgentsMdCache` from the Preferences UI) or when the
        // configured paths change between turns. Reads the single per-turn
        // snapshot threaded from RunRequest.
        std::lock_guard<std::mutex> lk(agentsMdMutex_);
        const bool cacheUsable = smatchet::ai::pure::AgentsMdCacheStillValid(
            agentsMdCacheValid_.load(std::memory_order_acquire), agentsMdCachedGlobalPath_, agentsMdCachedProjectPath_,
            agentsMdCachedAutoDiscover_, cfg.AgentsMdGlobalPath, cfg.ProjectAgentsMdPath,
            cfg.AgentsMdAutoDiscoverProject);
        if (cacheUsable) {
            agentsMd = agentsMdCachedBody_;
        } else {
            agentsMd = AgentsMdLoader::LoadLayered(cfg.AgentsMdGlobalPath, cfg.ProjectAgentsMdPath,
                                                   cfg.AgentsMdAutoDiscoverProject);
            agentsMdCachedBody_ = agentsMd;
            agentsMdCachedGlobalPath_ = cfg.AgentsMdGlobalPath;
            agentsMdCachedProjectPath_ = cfg.ProjectAgentsMdPath;
            agentsMdCachedAutoDiscover_ = cfg.AgentsMdAutoDiscoverProject;
            agentsMdCacheValid_.store(true, std::memory_order_release);
        }
    }

    const std::vector<AiContextBlock> resolvedContext = ResolveDeferredContext(req);
    chatReq.SystemPrompt = smatchet::ai::pure::ComposeSystemPrompt(agentsMd, resolvedContext);

    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = req.Prompt;
    chatReq.History.push_back(std::move(userMsg));
}

IAiClient::DeltaCallback AiAssistantController::MakeOnDelta(uint64_t turnGen) {
    // Capture turnGen by value so stale dispatcher tasks (after a newer Submit
    // or Cancel) can be dropped on the UI side via assistantTurnGen comparison.
    MainThreadDispatcher* dispatcher = &dispatcher_;
    IAiAssistantUiState* ui = &uiState_;

    // Worker-side coalescing: fast providers stream 30-50 chunks/s and each post
    // costs a dispatcher round-trip + a UI redraw. Buffer chunks locally and post
    // one task per flush window (>4 KB accumulated, ~80 ms elapsed, or the final
    // delta). State lives in a shared_ptr so std::function copies share it.
    struct CoalesceState {
        std::string pending;
        std::chrono::steady_clock::time_point lastPostAt = std::chrono::steady_clock::now();
    };
    auto coalesce = std::make_shared<CoalesceState>();

    return [dispatcher, ui, turnGen, coalesce](const AiStreamDelta& delta) {
        coalesce->pending.append(delta.TokenChunk);
        const bool isFinal = delta.IsFinal;
        constexpr std::size_t kFlushBytes = 4u * 1024u;
        constexpr std::chrono::milliseconds kFlushWindow(80);
        const auto now = std::chrono::steady_clock::now();
        if (!isFinal && coalesce->pending.size() <= kFlushBytes && now - coalesce->lastPostAt < kFlushWindow) {
            return;
        }
        coalesce->lastPostAt = now;
        std::string chunk;
        chunk.swap(coalesce->pending);
        dispatcher->PostToMainThread([ui, turnGen, chunk, isFinal]() {
            if (ui->AssistantTurnGen() != turnGen) {
                return; // stale callback — Cancel or newer Submit raced us
            }
            if (!chunk.empty()) {
                // Hard cap on assistantStreamBuf — defense-in-depth against a
                // runaway provider that keeps streaming past any reasonable
                // response size. 4 MiB mirrors the SSE/NDJSON parser caps.
                constexpr std::size_t kMaxStreamBufBytes = 4u * 1024u * 1024u;
                if (ui->AssistantStreamBuf().size() < kMaxStreamBufBytes) {
                    const std::size_t room = kMaxStreamBufBytes - ui->AssistantStreamBuf().size();
                    if (chunk.size() <= room) {
                        ui->AssistantStreamBuf().append(chunk);
                    } else {
                        ui->AssistantStreamBuf().append(chunk, 0, room);
                        ui->AssistantStreamBuf().append("\n[truncated — response exceeded 4 MiB cap]");
                    }
                }
            }
            if (isFinal) {
                AiMessage assistantMsg;
                assistantMsg.Role = "assistant";
                assistantMsg.Content = ui->AssistantStreamBuf();
                assistantMsg.CreatedAtUnixMs = smatchet::ai::NowUnixMs();
                // Phase 3 of ai-chat-claude-desktop-parity. Persist snapshot taken
                // before move; row-id slot grows in lock-step so the worker callback
                // can backfill the SQLite id. Gated on hydrated so a final-flush from
                // a turn that completed during the hydrate window doesn't write a
                // duplicate that the hydrate will then re-load.
                AiMessage persistCopy = assistantMsg;
                const std::size_t newIdx = ui->AssistantHistory().size();
                ui->AssistantHistory().push_back(std::move(assistantMsg));
                ui->AssistantHistoryRowIds().push_back(-1);
                if (ui->AssistantHistoryHydrated()) {
                    smatchet::ai::chat_persist::EnqueueAppendAndTrim(std::move(persistCopy), newIdx,
                                                                     ui->AssistantHistoryMaxRows());
                }
                ui->AssistantStreamBuf().clear();
                ui->SetAssistantInFlight(false);
            }
        });
    };
}

IAiClient::ErrorCallback AiAssistantController::MakeOnError(uint64_t turnGen) {
    MainThreadDispatcher* dispatcher = &dispatcher_;
    IAiAssistantUiState* ui = &uiState_;

    return [this, dispatcher, ui, turnGen](const AiStreamError& err) {
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
        dispatcher->PostToMainThread([ui, turnGen, httpStatus, message, wasCancelled]() {
            if (ui->AssistantTurnGen() != turnGen) {
                return;
            }
            ui->SetAssistantInFlight(false);
            if (wasCancelled) {
                // Scenario 3 contract — partial text retained in history.
                if (!ui->AssistantStreamBuf().empty()) {
                    AiMessage assistantMsg;
                    assistantMsg.Role = "assistant";
                    assistantMsg.Content = ui->AssistantStreamBuf() + "\n[cancelled]";
                    assistantMsg.CreatedAtUnixMs = smatchet::ai::NowUnixMs();
                    // Phase 3 of ai-chat-claude-desktop-parity. Same enqueue shape as
                    // the normal-finalisation site; cancelled-with-partial-text rows
                    // persist so the next launch shows the user where they stopped.
                    AiMessage persistCopy = assistantMsg;
                    const std::size_t newIdx = ui->AssistantHistory().size();
                    ui->AssistantHistory().push_back(std::move(assistantMsg));
                    ui->AssistantHistoryRowIds().push_back(-1);
                    if (ui->AssistantHistoryHydrated()) {
                        smatchet::ai::chat_persist::EnqueueAppendAndTrim(std::move(persistCopy), newIdx,
                                                                         ui->AssistantHistoryMaxRows());
                    }
                }
                ui->AssistantStreamBuf().clear();
                ui->AssistantLastError().clear();
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "API Error: %d ", httpStatus);
                ui->AssistantLastError() = std::string(buf) + message;
                ui->AssistantStreamBuf().clear();
            }
        });
    };
}

void AiAssistantController::RunStreaming(const AiChatRequest& chatReq, const AiCancelToken& cancel,
                                         const IAiClient::DeltaCallback& onDelta,
                                         const IAiClient::ErrorCallback& onError) {
    try {
        client_->SendStreaming(clientConfig_, chatReq, onDelta, onError, cancel);
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
}

void AiAssistantController::StreamAndDispatch(const AiChatRequest& chatReq, const AiCancelToken& cancel,
                                              uint64_t turnGen) {
    const IAiClient::DeltaCallback onDelta = MakeOnDelta(turnGen);
    const IAiClient::ErrorCallback onError = MakeOnError(turnGen);

    RunStreaming(chatReq, cancel, onDelta, onError);

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
