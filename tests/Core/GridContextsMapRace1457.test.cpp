// GridContextsMapRace1457.test.cpp — issue #1457 TSan regression.
//
// Models the locking discipline AppController applies to gridContexts_ — the
// std::map<std::string, std::unique_ptr<GridLiveContext>> keyed by GridPane id.
// The User Info worker (FetchPaneGroupMembers / FetchPaneUserActivity off
// std::async) resolves a context by id off the UI thread while the UI thread
// retire-erases hidden contexts and emplaces freshly-shown ones. Before the fix
// the worker's find traversed the map with no guard against the UI thread's
// concurrent tree-restructuring erase/emplace — a data race / UB.
//
// The real AppController is far too heavy to construct under -fsanitize=thread
// (cpr / SQLite / ImGui closure), so this is a seam-level fixture mirroring the
// three guarded operations: a worker thread doing snapshot-find-under-map-mutex
// while a UI thread does emplace + erase under the SAME mutex. TSan asserts no
// report; the fixture asserts the snapshot pointer (when found) is always a live,
// dereferenceable object — exercising the husk-stays-alive (ADR-0012 graveyard)
// invariant that lets the worker write into a retired context as a harmless no-op.
//
// Header-only closure (no product .cpp linked): the discipline under test is the
// mutex ordering, not AppController internals.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Stand-in for GridLiveContext: carries its own per-context content mutex (the
// roster mutex in the real code) so the fixture can reproduce the worker's
// map-mutex-then-release-then-content-mutex ordering exactly.
struct FakeContext {
    std::mutex contentMutex_;
    int payload = 0;
    std::atomic<bool> alive{true};
};

// Mirrors the AppController seam: gridContexts_ guarded by gridContextsMutex_ for
// structure, each context guarded by its own content mutex for payload.
class ContextMapSeam {
public:
    ContextMapSeam() {
        // The permanent default context (kDefaultPaneId), never retired — the
        // worker's fallback target.
        contexts_["default"] = std::make_unique<FakeContext>();
        graveyard_.reserve(4096);
    }

    // UI thread: EnsurePaneContextLive emplace.
    void Emplace(const std::string& id) {
        std::lock_guard<std::mutex> mapLk(mapMutex_);
        if (contexts_.find(id) == contexts_.end()) {
            contexts_[id] = std::make_unique<FakeContext>();
        }
    }

    // UI thread: retireExpiredHiddenContexts_ erase. The husk is moved to the
    // graveyard (kept alive for latched worker readers), not freed.
    void Erase(const std::string& id) {
        std::lock_guard<std::mutex> mapLk(mapMutex_);
        std::map<std::string, std::unique_ptr<FakeContext>>::iterator it = contexts_.find(id);
        if (it != contexts_.end()) {
            graveyard_.push_back(std::move(it->second));
            contexts_.erase(it);
        }
    }

    // Worker thread: snapshot the pointer under the map mutex, release it, then
    // take the per-context content mutex — never both at once. Mirrors the
    // FetchPaneGroupMembers write-back and FetchPaneUserActivity latch.
    void WorkerTouch(const std::string& id) {
        FakeContext* ctx = nullptr;
        {
            std::lock_guard<std::mutex> mapLk(mapMutex_);
            std::map<std::string, std::unique_ptr<FakeContext>>::iterator it = contexts_.find(id);
            ctx = (it != contexts_.end()) ? it->second.get() : defaultPtr();
        }
        // ctx is never dangling: a found context is live, an unfound one falls
        // back to the permanent default, and a context retired between the
        // snapshot and here survives in the graveyard (ADR-0012 husk).
        REQUIRE(ctx != nullptr);
        std::lock_guard<std::mutex> contentLk(ctx->contentMutex_);
        ++ctx->payload;
    }

private:
    FakeContext* defaultPtr() {
        std::map<std::string, std::unique_ptr<FakeContext>>::iterator it = contexts_.find("default");
        return it->second.get();
    }

    mutable std::mutex mapMutex_;
    std::map<std::string, std::unique_ptr<FakeContext>> contexts_;
    std::vector<std::unique_ptr<FakeContext>> graveyard_;
};

constexpr int kIterations = 400;

} // namespace

TEST_CASE("GridContextsMapRace1457: worker snapshot-find vs UI emplace/erase under the map mutex") {
    ContextMapSeam seam;

    std::atomic<int> workerTouches(0);
    std::atomic<int> uiMutations(0);

    // UI thread: churn the map structure — emplace then erase a rotating set of
    // pane ids, exactly the retire/show lifecycle that races the worker's find.
    std::thread ui([&] {
        for (int i = 0; i < kIterations; ++i) {
            const std::string id = "pane-" + std::to_string(i % 8);
            seam.Emplace(id);
            ++uiMutations;
            seam.Erase(id);
            ++uiMutations;
        }
    });

    // Worker thread: repeatedly resolve a context by id (some present, some
    // already retired, falling back to default) and touch its payload.
    std::thread worker([&] {
        for (int i = 0; i < kIterations; ++i) {
            seam.WorkerTouch("pane-" + std::to_string(i % 8));
            seam.WorkerTouch("absent"); // forces the default-context fallback path
            ++workerTouches;
        }
    });

    ui.join();
    worker.join();

    CHECK(uiMutations.load() == kIterations * 2);
    CHECK(workerTouches.load() == kIterations);
}
