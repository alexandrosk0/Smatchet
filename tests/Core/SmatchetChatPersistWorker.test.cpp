// chat_persist worker cache resolution. The worker used to hold a raw LocalCacheManager& for the
// process lifetime, so a cache recreate (AppController swaps `Cache` with atomic_store and frees the
// old manager) left it writing through a dangling reference. It now resolves the cache per op through
// a CacheResolver. These cases drive the real worker against in-memory LCMs behind a test-owned
// atomic shared_ptr, the same shape AppController hands it:
//   - a swap routes the next write to the new cache and the old one is released, not kept alive;
//   - a null cache (recreate mid-flight) is retried, and the op lands once a cache is published;
//   - Stop with no cache drops the op promptly instead of waiting for one.

#if defined(SMATCHET_WITH_AI)

#include "AiTypes.h"
#include "LocalCacheManager.h"
#include "MainThreadDispatcher.h"
#include "SmatchetChatPersistWorker.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace chat_persist = smatchet::ai::chat_persist;

namespace {

std::vector<std::string> LoadContents(LocalCacheManager& lcm) {
    std::vector<AiMessage> msgs;
    std::vector<std::int64_t> ids;
    lcm.LoadChatMessages(100, msgs, ids);
    std::vector<std::string> out(msgs.size());
    std::transform(msgs.begin(), msgs.end(), out.begin(), [](const AiMessage& m) { return m.Content; });
    return out;
}

// Polls until `lcm` holds `want` chat rows or ~5 s pass; returns the final row count.
std::size_t WaitForRows(LocalCacheManager& lcm, std::size_t want) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const std::size_t n = LoadContents(lcm).size();
        if (n >= want || std::chrono::steady_clock::now() >= deadline) {
            return n;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void EnqueueAppend(const std::string& content, std::int64_t createdAt) {
    chat_persist::Op op;
    op.kind = chat_persist::OpKind::Append;
    op.message.Role = "user";
    op.message.Content = content;
    op.message.CreatedAtUnixMs = createdAt;
    chat_persist::Enqueue(std::move(op));
}

// Test-owned stand-in for AppController::Cache: swapped with atomic_store, read with atomic_load.
struct CacheSlot {
    std::shared_ptr<LocalCacheManager> current;
    chat_persist::CacheResolver Resolver() {
        return [this]() { return std::atomic_load(&current); };
    }
    void Publish(std::shared_ptr<LocalCacheManager> next) { std::atomic_store(&current, std::move(next)); }
};

// Stops the process-global worker even when a REQUIRE unwinds the case.
struct WorkerGuard {
    WorkerGuard() = default;
    WorkerGuard(const WorkerGuard&) = delete;
    WorkerGuard& operator=(const WorkerGuard&) = delete;
    ~WorkerGuard() { chat_persist::Stop(); }
};

} // namespace

TEST_CASE("chat_persist: a swapped cache receives the next write and the old manager is released") {
    MainThreadDispatcher dispatcher;
    CacheSlot slot;
    auto first = std::make_shared<LocalCacheManager>(":memory:");
    slot.Publish(first);

    WorkerGuard guard;
    chat_persist::Start(slot.Resolver(), dispatcher, chat_persist::OnAppendCallback());

    EnqueueAppend("before swap", 1000);
    REQUIRE(WaitForRows(*first, 1) == 1);

    // The recreate shape: publish a fresh manager, drop the owner's reference to the old one.
    auto second = std::make_shared<LocalCacheManager>(":memory:");
    const std::weak_ptr<LocalCacheManager> firstWeak = first;
    slot.Publish(second);
    first.reset();

    EnqueueAppend("after swap", 2000);
    REQUIRE(WaitForRows(*second, 1) == 1);
    CHECK(LoadContents(*second) == std::vector<std::string>{"after swap"});

    // The worker latches the cache per op only, so nothing keeps the swapped-out manager alive.
    CHECK(firstWeak.expired());
}

TEST_CASE("chat_persist: an op enqueued while no cache is published lands once one is") {
    MainThreadDispatcher dispatcher;
    CacheSlot slot; // starts null: a recreate has withdrawn the cache

    WorkerGuard guard;
    chat_persist::Start(slot.Resolver(), dispatcher, chat_persist::OnAppendCallback());

    EnqueueAppend("during recreate", 3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // several retry intervals with no cache

    auto fresh = std::make_shared<LocalCacheManager>(":memory:");
    slot.Publish(fresh);
    REQUIRE(WaitForRows(*fresh, 1) == 1);
    CHECK(LoadContents(*fresh) == std::vector<std::string>{"during recreate"});
}

TEST_CASE("chat_persist: Stop with no cache drops the pending op without waiting for one") {
    MainThreadDispatcher dispatcher;
    CacheSlot slot;

    chat_persist::Start(slot.Resolver(), dispatcher, chat_persist::OnAppendCallback());
    EnqueueAppend("never persisted", 4000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let the worker enter its retry wait

    const auto t0 = std::chrono::steady_clock::now();
    chat_persist::Stop();
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    // The 250 ms drain budget plus slack: the retry wait is interrupted, not waited out.
    CHECK(elapsed < std::chrono::seconds(2));

    // Stop is final: a cache published afterwards never receives the dropped op.
    auto late = std::make_shared<LocalCacheManager>(":memory:");
    slot.Publish(late);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(LoadContents(*late).empty());
}

#endif // SMATCHET_WITH_AI
