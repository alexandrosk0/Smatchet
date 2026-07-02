#ifndef SMATCHET_AI_ASSISTANT_CONTROLLER_H
#define SMATCHET_AI_ASSISTANT_CONTROLLER_H

#if !defined(SMATCHET_WITH_AI)
// AiAssistantController is the standalone-only worker for the Smatchet Assistant side
// panel. The DX12 (Unreal) target compiles Source/Core/ TUs with SMATCHET_WITH_AI
// undefined; this header collapses to nothing so dependent TUs (AppController.cpp)
// can `#include` it unconditionally without a parallel macro gate at every call site.
#else

#include "AiTypes.h"
#include "AiXmlAttrEscape.h"
#include "IAiClient.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AppController;
class MainThreadDispatcher;
struct IAiAssistantUiState;
struct TrackerConfig;

namespace smatchet {
namespace ai {
namespace pure {

// `EscapeXmlAttr` now lives in the standalone pure header "AiXmlAttrEscape.h"
// (included above) so the doctest rig can exercise it without this controller
// header's AppController / cpr / Ui-session dependency chain. The call site at
// `ComposeSystemPrompt` below uses it unchanged (#826).

/// Compose the AI system prompt from a (possibly empty) merged agents.md blob and the
/// already-resolved context blocks. Pure (no I/O, no AppController coupling) so it is
/// bucket-A unit-testable. Mirrors the worker-thread assembly in
/// `AiAssistantController::RunRequest`:
///   * When `agentsMd` is non-empty: append it, ensure a trailing newline, then a
///     `\n---\n\n` separator.
///   * For each context block with a non-empty body: emit
///     `<smatchet_context block="<name>">\n<body>\n</smatchet_context>\n`, blocks joined
///     by a single `\n`. The whole context region is prefixed with a
///     `## Current Smatchet context\n\n` header — only when at least one block has content.
/// Blocks with empty bodies are skipped (matches the disabled-toggle no-op behaviour).
/// `inline` (header-defined) so the pure-logic test rig links it without pulling in the
/// controller TU's AppController / Ui-session dependencies.
inline std::string ComposeSystemPrompt(const std::string& agentsMd,
                                       const std::vector<AiContextBlock>& resolvedContext) {
    std::string systemPrompt;
    if (!agentsMd.empty()) {
        systemPrompt.append(agentsMd);
        if (systemPrompt.back() != '\n') {
            systemPrompt.push_back('\n');
        }
        systemPrompt.append("\n---\n\n");
    }

    std::string contextSection;
    for (std::vector<AiContextBlock>::const_iterator it = resolvedContext.begin(); it != resolvedContext.end(); ++it) {
        const AiContextBlock& block = *it;
        if (block.Body.empty()) {
            continue;
        }
        if (!contextSection.empty()) {
            contextSection.push_back('\n');
        }
        contextSection.append("<smatchet_context block=\"");
        contextSection.append(EscapeXmlAttr(block.Name));
        contextSection.append("\">\n");
        contextSection.append(block.Body);
        if (block.Body.back() != '\n') {
            contextSection.push_back('\n');
        }
        contextSection.append("</smatchet_context>\n");
    }
    if (!contextSection.empty()) {
        systemPrompt.append("## Current Smatchet context\n\n");
        systemPrompt.append(contextSection);
    }
    return systemPrompt;
}

/// Resolve the effective chat model for a turn. Pure switch over the provider enum +
/// the per-provider configured model ids; a non-empty `modelOverride` (chat-window Combo)
/// always wins. Mirrors the model-selection block in `RunRequest`. `inline` for the same
/// link-without-the-controller-TU reason as `ComposeSystemPrompt`.
inline std::string ResolveChatModel(AiProvider provider, const std::string& modelOverride,
                                    const std::string& modelOpenAi, const std::string& modelAnthropic,
                                    const std::string& modelOllama, const std::string& modelDeepSeek) {
    if (!modelOverride.empty()) {
        return modelOverride;
    }
    switch (provider) {
    case AiProvider::Anthropic:
        return modelAnthropic;
    case AiProvider::OllamaNative:
    case AiProvider::OllamaOpenAiCompat:
        return modelOllama;
    case AiProvider::DeepSeek:
        return modelDeepSeek;
    case AiProvider::OpenAi:
    default:
        return modelOpenAi;
    }
}

/// Decide whether the worker-thread agents.md cache is still valid for a turn.
/// Pure + `inline` (no I/O, no AppController coupling) so the test rig links it.
/// Cache is reusable only when already populated AND every `LoadLayered` input is
/// unchanged — both paths AND the auto-discover flag (omitting the flag served a
/// stale blob on auto-discovery toggle — the CodeRabbit finding fixed here).
inline bool AgentsMdCacheStillValid(bool cacheValid, const std::string& cachedGlobalPath,
                                    const std::string& cachedProjectPath, bool cachedAutoDiscover,
                                    const std::string& cfgGlobalPath, const std::string& cfgProjectPath,
                                    bool cfgAutoDiscover) {
    return cacheValid && cachedGlobalPath == cfgGlobalPath && cachedProjectPath == cfgProjectPath &&
           cachedAutoDiscover == cfgAutoDiscover;
}

} // namespace pure
} // namespace ai
} // namespace smatchet

/// Worker-thread-backed coordinator for the Smatchet Assistant side panel.
/// **Threading invariants** (UX pillar 2 + pillar 3):
///   * `Submit` / `Cancel` / `CurrentState` are UI-thread-only. They push work onto the
///     internal queue + signal the worker; no blocking I/O ever runs on the UI thread.
///   * The worker thread (`WorkerLoop`) is the *only* thread that calls
///     `IAiClient::SendStreaming`. cpr / libcurl never reach the UI frame stack.
///   * `IAiClient` delta + error callbacks fire on the worker thread. They MUST hand off
///     to the UI thread via `AppController::mainThreadDispatcher.PostToMainThread(...)`
///     — never touch `UiDrawSession` state directly from a worker callback.
///   * `~AiAssistantController` raises the shutdown atom, flips the in-flight AND every
///     pending turn's cancel atom, signals the queue cv, and joins the worker. Members are destroyed after the
///     join returns, so any worker reference is stable for the entire active lifetime.
///   * The owning `AppController` must hold this controller in a `std::unique_ptr` and
///     destroy it (via `aiAssistant_.reset()`) at the *top* of `~AppController` before
///     the main-thread dispatcher's `BeginShutdown()` fires. Once `BeginShutdown()`
///     runs, `PostToMainThread` no-ops, so any in-flight worker callbacks will
///     drop their UI-side deltas — that's harmless because the user is exiting.
/// **State machine**: `Idle` → (`Submit`) → `InFlight` → (`SendStreaming` returns)
/// → `Idle` | `Cancelled` | `Errored`. `Cancel` only flips the atom — the worker
/// observes it on the next `WriteCallback` poll and the state transitions when
/// the (now-aborting) `SendStreaming` call returns.
class AiAssistantController {
  public:
    /// Snapshot of the controller's run state, as observed from the UI thread. The worker
    /// thread updates the atomic when transitioning; the UI thread reads via `CurrentState()`.
    enum class State { Idle, InFlight, Cancelled, Errored };

    AiAssistantController(MainThreadDispatcher& dispatcher, IAiAssistantUiState& uiState);
    ~AiAssistantController();

    AiAssistantController(const AiAssistantController&) = delete;
    AiAssistantController& operator=(const AiAssistantController&) = delete;

    /// UI-thread only. Enqueues a turn for the worker; returns true on success
    /// (the turn was added to the queue), false when the controller rejected
    /// the request and the caller should NOT flip `assistantInFlight = true`.
    /// Rejection currently fires when (a) no live `IAiClient` is wired (the
    /// AI provider enum maps to a build-stripped client or the factory
    /// returned null), or (b) the controller is shutting down. Both cases
    /// emit a `LOG_WARN` on the worker side. The UI side surfaces the
    /// drop via `g_ui.assistantLastError` so the user sees a recoverable
    /// error strip instead of a permanently-stuck Send button.
    /// The caller is expected to have already updated
    /// `UiDrawSession::assistantTurnGen` to a new value and stashed it into
    /// the matching session field — the controller treats `turnGen` as
    /// opaque and forwards it back through every dispatcher callback so
    /// the UI side can drop stale deltas.
    /// `modelOverride` / `effortOverride` are per-turn overrides selected by the chat-
    /// window header Combos. Empty strings mean "use the saved Preferences value for
    /// the active provider". `effortOverride` accepts the same enum as
    /// `TrackerConfig::AiReasoningEffort` ("auto" | "low" | "medium" | "high").
    bool Submit(uint64_t turnGen, std::string prompt, std::vector<AiContextBlock> context,
                std::string modelOverride = std::string(), std::string effortOverride = std::string());

    /// UI-thread only. Sets the in-flight turn's cancel atom to `true` — cpr's
    /// `WriteCallback` polls it every chunk and returns `false`, which aborts the HTTP
    /// session and causes `SendStreaming` to invoke `onError` with `WasCancelled = true`.
    /// ALSO flips every still-QUEUED turn's own token (not just the in-flight one).
    /// This is necessary, not just thorough: a turn Submit() just enqueued may not have
    /// been popped by WorkerLoop yet, so it has no representation in `currentCancel_` —
    /// without also flipping `pending_` tokens here, a Cancel() that lands in that window
    /// would silently fail to stop it. The one caller that can have more than one turn
    /// queued at once is the ungated `AppController::PromptAi` (Lua/automation) path — a
    /// panel Cancel() click while a Lua-submitted turn is queued behind the visible one
    /// will also cancel that queued turn, not just the visible one. Accepted: there is no
    /// per-turn Cancel UI, and `PromptAi` turns are already dropped by the UI's stale-turn
    /// gate regardless (CPP_CODE_AUDIT.md #33) — cancelling one changes what silently
    /// discards it, not whether it was ever visible to the user.
    void Cancel();

    /// UI-thread only. Returns the current state. Reads an atomic — cheap.
    State CurrentState() const { return state_.load(std::memory_order_acquire); }

    /// Provider name from the cached client; empty if no provider is wired yet.
    /// UI-thread safe — returns a snapshot copy under `providerStateMutex_` because the
    /// worker thread rewrites `cachedProviderName_` whenever the user picks a different
    /// provider in Preferences between turns.
    std::string GetActiveProviderName() const;

    // --- Stable signature stubs for Phase E Lua glue. Always present so
    // AppController_LuaBindings.cpp can call them unconditionally regardless
    // of SMATCHET_WITH_AI — the controller-side body short-circuits when
    // there's no live client. Phase C will replace these no-ops with real
    // context-accumulation logic.

    /// Append a Lua-supplied context block to the next turn's payload.
    /// Thread-safe — guarded by `luaContextMutex_`. May be called from the
    /// background Lua automation worker via the `__smatchet_app_ui` slot, in
    /// addition to the UI-thread Lua glue.
    void AddAiContext(const AiContextBlock& block);
    /// Reset the per-turn Lua context. Thread-safe.
    void ClearAiContext();
    /// Snapshot of the Lua-side context. Thread-safe (returns a copy under lock).
    std::vector<AiContextBlock> GetAiContext() const;

    /// Invalidate the worker-side agents.md cache. Call from the UI thread
    /// after the user edits AgentsMdGlobalPath / ProjectAgentsMdPath in
    /// Preferences so the next worker turn re-reads from disk instead of
    /// serving a stale cached blob. Cheap — flips an atomic.
    void InvalidateAgentsMdCache();

  private:
    struct Request {
        std::string Prompt;
        std::vector<AiContextBlock> Context;
        uint64_t TurnGen;
        /// Empty = use Preferences-saved value at RunRequest time. See
        /// Submit() doc-comment for semantics.
        std::string ModelOverride;
        std::string EffortOverride;
        /// This request's own cancel token, created at Submit() time. Each queued
        /// turn owns its token independently of whichever turn is currently
        /// in-flight — see `currentCancel_` (CPP_CODE_AUDIT.md #4: a later Submit
        /// used to rebind the single shared `currentCancel_` while an earlier turn
        /// was still streaming, so Cancel()/the destructor flipped the wrong atom).
        AiCancelToken Cancel;
    };

    void WorkerLoop();
    void RunRequest(const Request& req, const AiCancelToken& cancel);

    // --- RunRequest phase helpers (worker-thread only) ---------------------
    // RunRequest is a thin sequence over these phases: refresh provider →
    // build the chat request (model/effort + system prompt + history) →
    // stream + dispatch deltas/errors to the UI. Each runs on the worker
    // thread; none reaches UI state directly (callbacks hop the dispatcher).

    /// Worker-thread provider refresh. Rebuilds `client_` when the configured
    /// provider enum changed (or the prior client was null) and always rebuilds
    /// `clientConfig_` so a fresh key/URL takes effect next turn. Returns true
    /// when a live client is available for the turn, false to abort early.
    /// `cfg` is the single per-turn config snapshot taken at the top of
    /// `RunRequest` (threaded through all three phase helpers so the worker
    /// reads config once per turn instead of three times).
    bool RefreshProviderForTurn(const TrackerConfig& cfg);

    /// Resolve the per-turn model + reasoning effort into `chatReq`, then run the
    /// F2 model-change auto-clear (posts a UI-side history clear when the
    /// "provider|model" signature changed since the previous turn). `cfg` is the
    /// shared per-turn snapshot (see `RefreshProviderForTurn`).
    void ResolveModelAndEffort(const TrackerConfig& cfg, const Request& req, AiChatRequest& chatReq);

    /// Resolve deferred context blocks (the worker-side audit-trail fetch) into a
    /// fully-materialised block list. Worker thread — performs the SQLite +
    /// filesystem read that the UI deliberately deferred.
    std::vector<AiContextBlock> ResolveDeferredContext(const Request& req);

    /// Assemble `chatReq.SystemPrompt` (cached agents.md blob + resolved context
    /// section via the pure composer) and seed `chatReq.History` with the user
    /// message. `cfg` is the shared per-turn snapshot (see `RefreshProviderForTurn`).
    void BuildChatPayload(const TrackerConfig& cfg, const Request& req, AiChatRequest& chatReq);

    /// Stream the request and dispatch deltas/errors back to the UI thread.
    /// Owns the onDelta/onError callbacks + the SendStreaming try/catch + the
    /// terminal Idle state transition.
    void StreamAndDispatch(const AiChatRequest& chatReq, const AiCancelToken& cancel, uint64_t turnGen);

    // --- StreamAndDispatch callback factories (worker-thread only) ----------
    // Each returns the worker-thread streaming callback for a turn. The bodies
    // hop the dispatcher to the UI thread before touching `g_ui`; `turnGen` is
    // captured by value so stale callbacks (after a newer Submit or Cancel) drop
    // via the `assistantTurnGen` comparison on the UI side.

    /// Build the per-delta callback: appends streamed chunks to the UI stream
    /// buffer (4 MiB hard cap) and, on the final delta, commits the assistant
    /// message to history + the persist queue.
    IAiClient::DeltaCallback MakeOnDelta(uint64_t turnGen);

    /// Build the error/cancel callback: records the terminal state, then on the
    /// UI thread either retains partial cancelled text or surfaces the API error.
    IAiClient::ErrorCallback MakeOnError(uint64_t turnGen);

    /// Run `SendStreaming` inside the worker-thread try/catch, funnelling any
    /// thrown client exception through `onError` so UI state always recovers.
    void RunStreaming(const AiChatRequest& chatReq, const AiCancelToken& cancel,
                      const IAiClient::DeltaCallback& onDelta, const IAiClient::ErrorCallback& onError);

    MainThreadDispatcher& dispatcher_;
    IAiAssistantUiState& uiState_;

    // Provider state. Constructed at controller init and rebuilt by the worker thread
    // at the top of every `RunRequest` when `ProviderFromConfig(cfg)` returns a value
    // different from `cachedProvider_`, OR unconditionally when only the URL / key
    // changed. The mutex guards reads from the UI thread (`GetActiveProviderName`,
    // `Submit`'s log line) against writes from the worker (`RunRequest`'s swap).
    mutable std::mutex providerStateMutex_;
    std::unique_ptr<IAiClient> client_;
    AiProvider cachedProvider_ = AiProvider::OpenAi;
    std::string cachedProviderName_;
    AiClientConfig clientConfig_;

    std::thread worker_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Request> pending_;
    bool shuttingDown_ = false;

    // The cancel atom for whichever turn is CURRENTLY in flight (or about to run).
    // Set by WorkerLoop, under queueMutex_, to the popped Request's own `Cancel`
    // token at the moment it starts that turn — NOT by Submit(), which would let a
    // later-queued turn's Submit silently steal this field out from under an
    // earlier turn still streaming (CPP_CODE_AUDIT.md #4). Cancel()/the destructor
    // flip this token (the in-flight turn) AND every still-pending request's own
    // token (see Cancel()). shared_ptr is captured by the worker callback so the
    // pointer outlives the controller's mutex acquisition; the cheap
    // shared_ptr<atomic<bool>>->load() runs on every libcurl WriteCallback chunk
    // (per the AGENTS.md UX-pillar-2 cancel-atom-poll cadence contract).
    AiCancelToken currentCancel_;

    std::atomic<State> state_{State::Idle};

    // Lua-side context stash. Mutated from both the UI thread (Lua glue running
    // on the main state) and the background automation worker (Lua glue running
    // via `__smatchet_app_ui` on `bgState`). Wrapped in a mutex — the cost of a
    // single lock per `ai.add_context` is negligible vs. the data-race UB risk.
    mutable std::mutex luaContextMutex_;
    std::vector<AiContextBlock> luaContext_;

    // Worker-thread cache of the merged agents.md blob + the paths it was
    // built from. Invalidated when `agentsMdCacheValid_` flips to false from
    // the UI thread via `InvalidateAgentsMdCache()`. Avoids re-reading the
    // (potentially 64 KB × 2 layers) blob on every turn.
    mutable std::mutex agentsMdMutex_;
    std::string agentsMdCachedBody_;
    std::string agentsMdCachedGlobalPath_;
    std::string agentsMdCachedProjectPath_;
    // Mirrors cfg.AgentsMdAutoDiscoverProject for the turn the cache was built on.
    // Part of the cache key — toggling auto-discovery with paths unchanged must
    // invalidate the cache (LoadLayered's output depends on this flag).
    bool agentsMdCachedAutoDiscover_{false};
    std::atomic<bool> agentsMdCacheValid_{false};

    // F2 — model-change auto-clear. The worker thread caches the previous
    // turn's "provider|model" signature; on the next turn it compares against
    // the freshly resolved signature and posts a UI-side clear of the chat
    // history when the signature changes. Empty on first turn so the very
    // first send never triggers a clear. Touched only from the worker.
    std::string lastModelSignature_;
};

#endif // SMATCHET_WITH_AI

#endif
