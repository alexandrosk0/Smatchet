#ifndef SMATCHET_AI_CHAT_PERSIST_WORKER_H
#define SMATCHET_AI_CHAT_PERSIST_WORKER_H

#if defined(SMATCHET_WITH_AI)

#include "AiTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>

class LocalCacheManager;
class MainThreadDispatcher;

/// Single-thread coalescing persistence worker for the AI chat history.
///
/// Replaces the per-write detached-thread pattern (`ScheduleConfigSaveDetached` on
/// `SmatchetAiAssistantUi.cpp:80`) for the chat-message write path. Detached threads
/// can outlive `LocalCacheManager` on shutdown (LCM is owned by `AppController`, which
/// destructs before process exit) — that is a Pillar 3 use-after-free path. Here a
/// single background thread is explicitly joined before LCM destructs, so chat writes
/// in flight at shutdown either drain within a 250 ms budget or are dropped with a
/// `LOG_WARN`.
///
/// Public surface is process-global (singleton WorkerState behind the namespace) —
/// the chat panel is a singleton in the app and the lifecycle is owned by
/// `AppController`, so a single instance is sufficient and avoids threading a
/// worker pointer through every call site.
namespace smatchet {
namespace ai {
namespace chat_persist {

enum class OpKind { Append, UpdatePin, ClearAll, Trim };

struct Op {
    OpKind kind = OpKind::Append;
    /// Append-only: the message body to persist. Worker reads `CreatedAtUnixMs`,
    /// `Role`, `Content`, and `Pinned` only — provider wire shape is irrelevant.
    AiMessage message;
    /// Append-only: index into `UiDrawSession::assistantHistory` at the moment of
    /// enqueue. After persistence the worker fires `OnAppendCallback(messageIndex,
    /// rowId)` on the UI thread so the UI can backfill the parallel row-id vector.
    std::size_t messageIndex = 0;
    /// UpdatePin-only: SQLite row id to flip.
    std::int64_t rowId = -1;
    /// UpdatePin-only: new pinned state.
    bool pinned = false;
    /// Trim-only: cap (most-recent N non-pinned rows kept; pinned rows exempt).
    std::size_t trimCap = 0;
};

/// UI-thread callback fired after a successful Append. `messageIndex` mirrors the
/// `Op::messageIndex` shipped at enqueue time; `rowId` is the SQLite-assigned id.
using OnAppendCallback = std::function<void(std::size_t messageIndex, std::int64_t rowId)>;

/// Starts the worker. Idempotent — a second `Start` while running is logged + ignored.
/// Lifetime contract: caller MUST invoke `Stop()` before `lcm` or `dispatcher` destruct.
/// `onAppend` is invoked on the UI thread via `dispatcher.PostToMainThread`; it may be
/// empty (no UI backfill, used by tests that drive the worker without a UI session).
void Start(LocalCacheManager& lcm, MainThreadDispatcher& dispatcher, OnAppendCallback onAppend);

/// Signals the worker to stop, best-effort drains pending ops within ~250 ms, joins.
/// Idempotent — safe to call when not running. After return, `Enqueue` no-ops.
void Stop();

/// Posts an op to the queue. Backpressure: when the queue exceeds 1024 entries, the
/// oldest non-Trim op is dropped with a `LOG_WARN`. Pending Trim ops collapse — a new
/// Trim removes any prior Trims so only the latest cap is honoured. No-op when the
/// worker is not running (between Stop and the next Start, or before first Start).
void Enqueue(Op op);

} // namespace chat_persist
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_WITH_AI

#endif // SMATCHET_AI_CHAT_PERSIST_WORKER_H
