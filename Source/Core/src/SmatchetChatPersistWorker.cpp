#include "SmatchetChatPersistWorker.h"

#if defined(SMATCHET_WITH_AI)

#include "LocalCacheManager.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace smatchet {
namespace ai {
namespace chat_persist {

namespace {

constexpr std::size_t kQueueSoftCap = 1024;
constexpr std::chrono::milliseconds kStopDrainBudget{250};
// Poll interval while a recreate has the cache withdrawn (null). There is no give-up budget: the
// withdrawn window spans background-task joins with no time bound, so any fixed budget could drop a
// chat write the user expects to survive a restart. The queue soft cap bounds memory meanwhile, and
// Stop interrupts the wait.
constexpr std::chrono::milliseconds kNullCacheRetryInterval{50};

struct WorkerState {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Op> queue;
    bool running = false;
    std::atomic<bool> shouldStop{false};
    // Set by Start before the thread launches, cleared by Stop after it joins, so the worker
    // reads them without the lock. The cache is never held here: `resolveCache` hands out a
    // fresh strong handle per op, so a cache swap cannot leave the worker on a freed LCM.
    CacheResolver resolveCache;
    MainThreadDispatcher* dispatcher = nullptr;
    OnAppendCallback onAppend;
    // Worker-thread only: ops dropped at stop because no cache was live, logged once on exit.
    std::size_t droppedNoCache = 0;

    // State() is a function-local static; this destructor runs during exit()/static
    // teardown. If the worker is still joinable then — e.g. the standalone crash handler's
    // frame-loop SEH path calls std::exit() WITHOUT the app's orderly chat_persist::Stop() —
    // std::thread::~thread() would call std::terminate(), turning an already-caught crash
    // into a hard process kill that bypasses WER. Signal + join here so abnormal exits tear
    // down cleanly. Join (never detach): WorkerLoop dereferences this very singleton, so it
    // must finish before our members are destroyed. No-ops after a normal Stop() (thread
    // already moved out + joined, so not joinable). The drain here still calls the resolver,
    // which captures AppController's `this`: safe because the crash-path std::exit() does not
    // destroy the (non-static) AppController, the same exposure the old raw LCM pointer had.
    ~WorkerState() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            shouldStop.store(true, std::memory_order_release);
        }
        cv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

WorkerState& State() {
    static WorkerState s;
    return s;
}

// Returns false, without touching `op`, when no cache is live (a recreate is between
// withdrawing the old one and publishing the new one); the caller retries.
bool ProcessOne(WorkerState& s, const Op& op) {
    // Latched for this op only: the strong handle keeps the LCM alive across the write even if
    // the owner swaps it meanwhile, and is released before the next op resolves again.
    const std::shared_ptr<LocalCacheManager> lcm = s.resolveCache ? s.resolveCache() : nullptr;
    if (!lcm) {
        return false;
    }
    switch (op.kind) {
    case OpKind::Append: {
        const std::int64_t rowId = lcm->AppendChatMessage(op.message);
        if (rowId > 0 && s.onAppend && s.dispatcher) {
            const std::size_t idx = op.messageIndex;
            const std::int64_t capturedRowId = rowId;
            OnAppendCallback cb = s.onAppend;
            s.dispatcher->PostToMainThread([cb, idx, capturedRowId]() { cb(idx, capturedRowId); });
        }
        break;
    }
    case OpKind::UpdatePin:
        lcm->UpdateChatMessagePin(op.rowId, op.pinned);
        break;
    case OpKind::ClearAll:
        lcm->ClearChatMessages();
        break;
    case OpKind::Trim:
        lcm->TrimChatMessages(op.trimCap);
        break;
    }
    return true;
}

// Runs `op`, retrying while the cache is withdrawn, until a cache is published again or Stop
// is signalled (the op is then dropped and counted in `droppedNoCache`).
void ProcessWithRetry(WorkerState& s, const Op& op) {
    while (!ProcessOne(s, op)) {
        std::unique_lock<std::mutex> lk(s.mtx);
        if (s.shouldStop.load(std::memory_order_acquire)) {
            ++s.droppedNoCache;
            return;
        }
        s.cv.wait_for(lk, kNullCacheRetryInterval, [&]() { return s.shouldStop.load(std::memory_order_acquire); });
    }
}

void WorkerLoop() {
    auto& s = State();
    for (;;) {
        Op op;
        {
            std::unique_lock<std::mutex> lk(s.mtx);
            s.cv.wait(lk, [&]() { return s.shouldStop.load(std::memory_order_acquire) || !s.queue.empty(); });
            if (s.queue.empty() && s.shouldStop.load(std::memory_order_acquire)) {
                if (s.droppedNoCache > 0) {
                    LOG_WARN("chat_persist: dropped %zu op(s) at stop: no local cache was live", s.droppedNoCache);
                    s.droppedNoCache = 0;
                }
                return;
            }
            op = std::move(s.queue.front());
            s.queue.pop_front();
        }
        try {
            ProcessWithRetry(s, op);
        } catch (const std::exception& ex) {
            LOG_WARN("chat_persist: op kind=%d threw %s", static_cast<int>(op.kind), ex.what());
        } catch (...) {
            LOG_WARN("chat_persist: op kind=%d threw unknown exception", static_cast<int>(op.kind));
        }
    }
}

} // namespace

void Start(CacheResolver resolveCache, MainThreadDispatcher& dispatcher, OnAppendCallback onAppend) {
    auto& s = State();
    std::thread launched;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.running) {
            LOG_WARN("chat_persist::Start called while already running — ignoring");
            return;
        }
        s.resolveCache = std::move(resolveCache);
        s.dispatcher = &dispatcher;
        s.onAppend = std::move(onAppend);
        s.shouldStop.store(false, std::memory_order_release);
        s.queue.clear();
        s.running = true;
    }
    // Construct the thread outside the lock so it cannot deadlock against the
    // WorkerLoop's first lock acquisition on slow scheduling.
    launched = std::thread(WorkerLoop);
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.thread = std::move(launched);
    }
    LOG_INFO("chat_persist: worker started");
}

void Stop() {
    auto& s = State();
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.running) {
            return;
        }
        s.shouldStop.store(true, std::memory_order_release);
    }
    s.cv.notify_all();

    // Best-effort drain within budget. Poll-sleep rather than condvar-wait so the
    // worker thread retains exclusive control over the queue mutex during drain.
    const auto deadline = std::chrono::steady_clock::now() + kStopDrainBudget;
    for (;;) {
        bool empty = false;
        {
            std::lock_guard<std::mutex> lk(s.mtx);
            empty = s.queue.empty();
        }
        if (empty || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.queue.empty()) {
            LOG_WARN("chat_persist: drain budget exhausted with %zu ops pending; dropping",
                     static_cast<std::size_t>(s.queue.size()));
            s.queue.clear();
        }
        workerToJoin = std::move(s.thread);
        s.running = false;
    }
    s.cv.notify_all();
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
    // Only after the join: the worker reads these without the lock while it runs. Dropping the
    // resolver here also releases whatever it captured before the owner destructs.
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.resolveCache = nullptr;
        s.dispatcher = nullptr;
        s.onAppend = nullptr;
    }
    LOG_INFO("chat_persist: worker stopped");
}

void Enqueue(Op op) {
    auto& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.running) {
            return;
        }
        if (op.kind == OpKind::Trim) {
            // Collapse: drop every pending Trim so only the latest cap survives.
            // Use the erase-remove idiom — std::remove_if is the C++14-canonical
            // form per cppcheck `useStlAlgorithm` over the raw `it = erase(it)` loop.
            s.queue.erase(
                std::remove_if(s.queue.begin(), s.queue.end(), [](const Op& o) { return o.kind == OpKind::Trim; }),
                s.queue.end());
        }
        if (s.queue.size() >= kQueueSoftCap) {
            // Drop the oldest non-Trim op. `std::find_if` is the cppcheck-canonical
            // shape; the raw for-loop tripped `useStlAlgorithm`.
            auto victim =
                std::find_if(s.queue.begin(), s.queue.end(), [](const Op& o) { return o.kind != OpKind::Trim; });
            if (victim != s.queue.end()) {
                s.queue.erase(victim);
            } else {
                // Every queued op is Trim — collapse them all (the new Trim about
                // to be pushed is the latest cap; older ones are stale anyway).
                s.queue.clear();
            }
            LOG_WARN("chat_persist: queue saturated, dropped oldest op (size=%zu)",
                     static_cast<std::size_t>(s.queue.size()));
        }
        s.queue.push_back(std::move(op));
    }
    // Notify unconditionally — we always reach here with a fresh queue entry; the
    // earlier `notify` flag was dead code (cppcheck `knownConditionTrueFalse`).
    s.cv.notify_one();
}

void EnqueueAppendAndTrim(AiMessage message, std::size_t messageIndex, int maxRowsCfg) {
    Op appendOp;
    appendOp.kind = OpKind::Append;
    appendOp.message = std::move(message);
    appendOp.messageIndex = messageIndex;
    Enqueue(std::move(appendOp));

    Op trimOp;
    trimOp.kind = OpKind::Trim;
    trimOp.trimCap = static_cast<std::size_t>(maxRowsCfg > 0 ? maxRowsCfg : 500);
    Enqueue(std::move(trimOp));
}

} // namespace chat_persist
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_WITH_AI
