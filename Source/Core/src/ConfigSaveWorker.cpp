#include "ConfigSaveWorker.h"

#include "Logger.h"
// Pure `MergePersistentViewsToolbarAppend` (no ImGui, no UI state) — the views read-modify-write
// this worker took over from `Views::Save` must keep folding in out-of-band ToolbarAppend writes.
#include "Ui/Views.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace smatchet {
namespace config_save {
namespace {

// Stop() drain window: bounds how long we wait for *queued* (not-yet-started) writes before
// joining (mirrors SmatchetChatPersistWorker's stop budget). The subsequent join() still waits for
// an in-flight write to finish — by design: a tiny atomic temp-then-rename completes in ms, and
// joining (vs detaching) is the Pillar-3-safe choice, since a detached thread writing config as the
// process tears down would be a use-after-free risk against the ConfigManager statics. So we accept a
// bounded-in-practice (not hard-capped) join over a real shutdown-race. Config writes are tiny.
constexpr std::chrono::milliseconds kStopDrainBudget{250};

struct WorkerState {
    std::mutex mtx;
    std::condition_variable cv;
    std::thread thread;
    bool running = false;
    std::atomic<bool> shouldStop{false};

    // Coalescing slots: at most one pending snapshot per config-kind; latest wins.
    bool trackerDirty = false;
    TrackerConfig trackerPending;
    bool annotateDirty = false;
    AnnotateAnalysisConfig annotatePending;
    bool viewsDirty = false;
    PersistentViewsFile viewsPending;
};

WorkerState& State() {
    static WorkerState s;
    return s;
}

bool HasPendingLocked(const WorkerState& s) { return s.trackerDirty || s.annotateDirty || s.viewsDirty; }

/// The read-modify-write `Views::Save` used to run inline on the UI thread (#2026). Kept in one
/// place so the worker path and the worker-not-running fallback below cannot drift.
void WritePersistentViews(PersistentViewsFile disk) {
    // DR13a: re-read and fold in any out-of-band ToolbarAppend writes before rewriting the whole
    // file from the caller's snapshot, or the toolbar editor's tracker-scope buttons are reverted.
    const PersistentViewsFile fresh = ConfigManager::LoadPersistentViewsFromDisk();
    smatchet::views_merge::MergePersistentViewsToolbarAppend(disk, fresh);
    ConfigManager::SavePersistentViewsToDisk(disk);
}

// Snapshot whatever is dirty (under lock, clearing the dirty flags), then write outside the lock so
// file I/O never blocks an Enqueue. Both writes go through the atomic-RMW ConfigManager seam.
void DrainOnce(WorkerState& s) {
    bool doTracker = false;
    bool doAnnotate = false;
    bool doViews = false;
    TrackerConfig tcfg;
    AnnotateAnalysisConfig acfg;
    PersistentViewsFile vfile;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.trackerDirty) {
            doTracker = true;
            tcfg = s.trackerPending;
            s.trackerDirty = false;
        }
        if (s.annotateDirty) {
            doAnnotate = true;
            acfg = s.annotatePending;
            s.annotateDirty = false;
        }
        if (s.viewsDirty) {
            doViews = true;
            vfile = s.viewsPending;
            s.viewsDirty = false;
        }
    }
    if (doTracker) {
        try {
            ConfigManager::Save(tcfg);
        } catch (...) { // catch-all-ok: Save logs its own diagnostics; a worker-thread throw must not
                        // terminate the process.
        }
    }
    if (doAnnotate) {
        try {
            ConfigManager::SaveAnnotateAnalysis(acfg);
        } catch (...) { // catch-all-ok: see above.
        }
    }
    if (doViews) {
        try {
            WritePersistentViews(vfile);
        } catch (...) { // catch-all-ok: see above.
        }
    }
}

void WorkerLoop() {
    auto& s = State();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.mtx);
            s.cv.wait(lk, [&s]() { return s.shouldStop.load(std::memory_order_acquire) || HasPendingLocked(s); });
            if (s.shouldStop.load(std::memory_order_acquire) && !HasPendingLocked(s)) {
                return;
            }
        }
        DrainOnce(s);
    }
}

} // namespace

void Start() {
    auto& s = State();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.running) {
        LOG_DEBUG("config_save: Start() ignored — worker already running");
        return;
    }
    s.shouldStop.store(false, std::memory_order_release);
    s.running = true;
    s.thread = std::thread(WorkerLoop);
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

    // Best-effort drain within budget. Poll-sleep rather than condvar-wait so the worker thread
    // retains exclusive control over the slots during drain.
    const auto deadline = std::chrono::steady_clock::now() + kStopDrainBudget;
    for (;;) {
        bool pending = false;
        {
            std::lock_guard<std::mutex> lk(s.mtx);
            pending = HasPendingLocked(s);
        }
        if (!pending || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (HasPendingLocked(s)) {
            LOG_WARN("config_save: drain budget exhausted; dropping pending config write(s)");
            s.trackerDirty = false;
            s.annotateDirty = false;
            s.viewsDirty = false;
        }
        workerToJoin = std::move(s.thread);
        s.running = false;
    }
    s.cv.notify_all(); // wake the loop so it observes shouldStop && !pending and returns
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

void EnqueueTrackerConfig(const TrackerConfig& cfg) {
    auto& s = State();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.running) {
            s.trackerPending = cfg;
            s.trackerDirty = true;
            queued = true;
        }
    }
    if (queued) {
        s.cv.notify_one();
        return;
    }
    // Worker not running — save synchronously so the write is never lost. RMW-serialized, so safe
    // even if it races the worker on another path.
    try {
        ConfigManager::Save(cfg);
    } catch (...) { // catch-all-ok: Save logs; never propagate from a fire-and-forget save.
    }
}

void EnqueueAnnotateConfig(const AnnotateAnalysisConfig& cfg) {
    auto& s = State();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.running) {
            s.annotatePending = cfg;
            s.annotateDirty = true;
            queued = true;
        }
    }
    if (queued) {
        s.cv.notify_one();
        return;
    }
    try {
        ConfigManager::SaveAnnotateAnalysis(cfg);
    } catch (...) { // catch-all-ok: see above.
    }
}

void EnqueuePersistentViews(const PersistentViewsFile& disk) {
    auto& s = State();
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.running) {
            s.viewsPending = disk;
            s.viewsDirty = true;
            queued = true;
        }
    }
    if (queued) {
        s.cv.notify_one();
        return;
    }
    // Worker not running (tests / CLI / pre-init / post-shutdown) — do the read-modify-write on
    // the caller so the views write is never lost.
    try {
        WritePersistentViews(disk);
    } catch (...) { // catch-all-ok: see above.
    }
}

} // namespace config_save
} // namespace smatchet
