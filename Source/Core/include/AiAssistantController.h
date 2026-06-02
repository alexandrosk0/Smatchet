#ifndef SMATCHET_AI_ASSISTANT_CONTROLLER_H
#define SMATCHET_AI_ASSISTANT_CONTROLLER_H

#if !defined(SMATCHET_WITH_AI)
// AiAssistantController is the standalone-only worker for the Smatchet Assistant side
// panel. The DX12 (Unreal) target compiles Source/Core/ TUs with SMATCHET_WITH_AI
// undefined; this header collapses to nothing so dependent TUs (AppController.cpp)
// can `#include` it unconditionally without a parallel macro gate at every call site.
#else

#include "AiTypes.h"
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

namespace smatchet {
namespace ai {
namespace pure {

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
        contextSection.append(block.Name);
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
///   * `~AiAssistantController` raises the shutdown atom, flips the current cancel atom,
///     signals the queue cv, and joins the worker. Members are destroyed after the
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

    explicit AiAssistantController(AppController& app);
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

    /// UI-thread only. Sets the in-flight turn's cancel atom to `true`. cpr's
    /// `WriteCallback` polls the atom every chunk and returns `false`, which aborts
    /// the HTTP session and causes `SendStreaming` to invoke `onError` with
    /// `WasCancelled = true`.
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
    bool RefreshProviderForTurn();

    /// Resolve the per-turn model + reasoning effort into `chatReq`, then run the
    /// F2 model-change auto-clear (posts a UI-side history clear when the
    /// "provider|model" signature changed since the previous turn).
    void ResolveModelAndEffort(const Request& req, AiChatRequest& chatReq);

    /// Resolve deferred context blocks (the worker-side audit-trail fetch) into a
    /// fully-materialised block list. Worker thread — performs the SQLite +
    /// filesystem read that the UI deliberately deferred.
    std::vector<AiContextBlock> ResolveDeferredContext(const Request& req);

    /// Assemble `chatReq.SystemPrompt` (cached agents.md blob + resolved context
    /// section via the pure composer) and seed `chatReq.History` with the user
    /// message.
    void BuildChatPayload(const Request& req, AiChatRequest& chatReq);

    /// Stream the request and dispatch deltas/errors back to the UI thread.
    /// Owns the onDelta/onError callbacks + the SendStreaming try/catch + the
    /// terminal Idle state transition.
    void StreamAndDispatch(const AiChatRequest& chatReq, const AiCancelToken& cancel, uint64_t turnGen);

    AppController& app_;

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

    // The cancel atom for the currently-flighting turn. Lives until the next
    // Submit replaces it. shared_ptr is captured by the worker callback so the
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
