// DraftStore.cpp — the process-wide draft store and its file worker. See DraftStore.h for the contract
// and docs/plans/input-ux-w2-drafts-undo.md for the design.
//
// Threading: `State().mu` guards the map and the flags. The UI thread holds it only for a map update or
// a lookup. The worker holds it to copy the map and clear the dirty flag, then serializes and writes
// with the lock released, so a slow disk never reaches the frame (Pillar 2; Persistence/AGENTS.md —
// all persistence I/O off the UI thread).

#include "DraftStore.h"

#include "Config/ConfigManager.h" // GetUserDataDirectory, AtomicWriteTextFile (temp + fsync + rename, 0600)
#include "Logger.h"

#include <ghc/filesystem.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace smatchet {
namespace drafts {
namespace {

namespace fs = ghc::filesystem;
using Clock = std::chrono::steady_clock;

// After a failed write, wait this long before trying again so a full disk is not retried every pause.
constexpr std::chrono::seconds kRetryAfterFailedWrite(5);

struct StoreState {
    std::mutex mu;
    std::condition_variable cv;
    DraftMap map;
    std::string path;
    std::thread worker;
    bool running = false;
    bool stopRequested = false;
    bool loaded = true; // a store that was never started has nothing to load
    bool dirty = false;
    Clock::time_point lastChange{};
    Clock::time_point firstUnsavedChange{};
    Clock::time_point notBefore{};

    ~StoreState() {
        // Static destruction: Stop() was not called (a test binary, an abnormal exit). Join without the
        // final write; the logger and ConfigManager may already be gone.
        std::thread pending;
        {
            std::lock_guard<std::mutex> lk(mu);
            stopRequested = true;
            dirty = false;
            pending = std::move(worker);
        }
        cv.notify_all();
        if (pending.joinable()) {
            pending.join();
        }
    }
};

StoreState& State() {
    static StoreState s;
    return s;
}

std::int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Caller holds the lock.
void MarkChangedLocked(StoreState& s) {
    const Clock::time_point now = Clock::now();
    if (!s.dirty) {
        s.firstUnsavedChange = now;
    }
    s.dirty = true;
    s.lastChange = now;
}

// Reads the drafts file, at most kMaxDraftFileBytes. False when there is nothing usable to read.
bool ReadDraftFile(const std::string& path, std::string& out) {
    /* PILLAR2_WORKER_ONLY */ // est-latency: 5ms - one small file read, on the draft store worker
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!fs::exists(p, ec) || ec) {
        return false;
    }
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec || size == 0) {
        return false;
    }
    if (size > kMaxDraftFileBytes) {
        LOG_WARN("DraftStore: '%s' is %llu bytes, over the %llu byte cap; not loaded", path.c_str(),
                 static_cast<unsigned long long>(size), static_cast<unsigned long long>(kMaxDraftFileBytes));
        return false;
    }
    fs::ifstream in(p, std::ios::binary);
    if (!in.is_open()) {
        LOG_WARN("DraftStore: could not open '%s' for reading", path.c_str());
        return false;
    }
    out.assign(static_cast<std::size_t>(size), '\0');
    in.read(&out[0], static_cast<std::streamsize>(size));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return !out.empty();
}

void LoadOnWorker(StoreState& s, const std::string& path) {
    std::string text;
    DraftMap loaded(s.map.Limits());
    if (ReadDraftFile(path, text)) {
        std::string err;
        if (!DraftMap::FromJson(text, loaded.Limits(), loaded, err)) {
            LOG_WARN("DraftStore: '%s' could not be read (%s); it is replaced on the next save", path.c_str(),
                     err.c_str());
        }
    }
    std::lock_guard<std::mutex> lk(s.mu);
    s.map.MergeUnder(loaded);
    s.loaded = true;
}

// Writes `snapshot` and records the outcome. Called with the lock released.
void WriteOnWorker(StoreState& s, const std::string& path, const DraftMap& snapshot) {
    /* PILLAR2_WORKER_ONLY */ // est-latency: 5ms - serialize and atomically replace one small file, on the worker
    const bool ok = ConfigManager::AtomicWriteTextFile(path, snapshot.ToJson());
    if (ok) {
        return;
    }
    LOG_WARN("DraftStore: could not save drafts to '%s'; retrying later", path.c_str());
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.dirty) {
        s.dirty = true;
        s.firstUnsavedChange = Clock::now();
        s.lastChange = s.firstUnsavedChange;
    }
    s.notBefore = Clock::now() + kRetryAfterFailedWrite;
}

// The next time the worker should look again while a change is waiting.
Clock::time_point NextWakeLocked(const StoreState& s) {
    const Clock::time_point due = DraftWriteDueAt(s.lastChange, s.firstUnsavedChange);
    return due < s.notBefore ? s.notBefore : due;
}

void WorkerMain(const std::string& path) {
    StoreState& s = State();
    LoadOnWorker(s, path);
    for (;;) {
        DraftMap snapshot;
        bool finalWrite = false;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            for (;;) {
                if (s.stopRequested) {
                    break;
                }
                const Clock::time_point now = Clock::now();
                if (now >= s.notBefore && DraftWriteDue(s.dirty, s.lastChange, s.firstUnsavedChange, now)) {
                    break;
                }
                if (s.dirty) {
                    s.cv.wait_until(lk, NextWakeLocked(s));
                } else {
                    s.cv.wait(lk);
                }
            }
            if (s.stopRequested && !s.dirty) {
                return;
            }
            finalWrite = s.stopRequested;
            snapshot = s.map;
            s.dirty = false;
        }
        WriteOnWorker(s, path, snapshot);
        if (finalWrite) {
            return;
        }
    }
}

void WorkerEntry(const std::string& path) {
    try {
        WorkerMain(path);
    } catch (const std::exception& ex) {
        LOG_ERROR("DraftStore: worker stopped: %s", ex.what());
    } catch (...) {
        LOG_ERROR("DraftStore: worker stopped on an unknown exception");
    }
    StoreState& s = State();
    std::lock_guard<std::mutex> lk(s.mu);
    s.loaded = true;   // a worker that died while loading must not leave restores waiting forever
    s.running = false; // however it ended: a later Start may begin a new worker
}

} // namespace

std::string DefaultPath() { return ConfigManager::GetUserDataDirectory() + "smatchet_drafts.json"; }

void Start(const std::string& path) {
    StoreState& s = State();
    std::thread finished;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.running) {
            LOG_WARN("DraftStore: Start called while already running; ignored");
            return;
        }
        // A worker that ended on its own (it died) is still a joinable thread object: reap it first.
        finished = std::move(s.worker);
    }
    if (finished.joinable()) {
        finished.join();
    }
    if (path.empty()) {
        LOG_WARN("DraftStore: no drafts path; drafts stay in memory");
        return;
    }
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.running || s.worker.joinable()) {
        return; // another Start won while this one reaped the old worker (its worker may even have ended already)
    }
    s.path = path;
    s.stopRequested = false;
    s.loaded = false;
    s.notBefore = Clock::time_point{};
    try {
        s.worker = std::thread(WorkerEntry, path);
    } catch (const std::system_error& ex) {
        s.loaded = true;
        LOG_ERROR("DraftStore: could not start the worker (%s); drafts stay in memory", ex.what());
        return;
    }
    s.running = true;
}

void Stop() {
    StoreState& s = State();
    std::thread worker;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.worker.joinable()) {
            return;
        }
        // `running` is left to the worker: it clears it as its last act, so a Start that arrives while this
        // join is under way is refused instead of resetting stopRequested under the worker.
        s.stopRequested = true;
        worker = std::move(s.worker);
    }
    s.cv.notify_all();
    worker.join();
}

bool IsLoaded() {
    StoreState& s = State();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.loaded;
}

bool Put(const std::string& surface, const std::string& scope, const std::string& text, const std::string& base) {
    StoreState& s = State();
    PutResult result = PutResult::Unchanged;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        const bool had = s.map.Contains(surface, scope);
        result = s.map.Put(surface, scope, text, base, NowUnixMs());
        const bool changed =
            result == PutResult::Stored || ((result == PutResult::Erased || result == PutResult::Rejected) && had);
        if (!changed) {
            return result != PutResult::Rejected;
        }
        MarkChangedLocked(s);
    }
    s.cv.notify_one();
    return result != PutResult::Rejected;
}

bool Get(const std::string& surface, const std::string& scope, DraftEntry& out) {
    StoreState& s = State();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.map.Get(surface, scope, out);
}

void Erase(const std::string& surface, const std::string& scope) {
    StoreState& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.map.Erase(surface, scope)) {
            return;
        }
        MarkChangedLocked(s);
    }
    s.cv.notify_one();
}

void EraseIfText(const std::string& surface, const std::string& scope, const std::string& text) {
    StoreState& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.map.EraseIfText(surface, scope, text)) {
            return;
        }
        MarkChangedLocked(s);
    }
    s.cv.notify_one();
}

void EraseSurface(const std::string& surface) {
    StoreState& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.map.EraseSurface(surface) == 0) {
            return;
        }
        MarkChangedLocked(s);
    }
    s.cv.notify_one();
}

void ResetForTests() {
    StoreState& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.map.Size() == 0) {
            return;
        }
        s.map.Clear();
        MarkChangedLocked(s);
    }
    s.cv.notify_one();
}

} // namespace drafts
} // namespace smatchet
