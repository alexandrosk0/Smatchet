// MainThreadDispatcherConcurrent.test.cpp — MainThreadDispatcher TSan pass (the keystone of the
// whole UI-thread-marshalling discipline).
//
// Every cross-thread write to the UI-owned `g_ui` (UiDrawSession) is race-free ONLY because the
// worker that produced it posts a lambda through MainThreadDispatcher::PostToMainThread and the
// UI thread runs that lambda later inside Drain(). Concretely: AiAssistantController's streaming
// worker wraps every `g_ui.assistantStreamBuf.append(...)` in PostToMainThread
// (AiAssistantController.cpp MakeOnDelta), the merge-watch notify HTTP thread posts its toast the
// same way (SmatchetMergeWatchNotifyServer.cpp), and the sync-status toasts ride the UI thread by
// contract. If the dispatcher itself races, ALL of those hand-offs race. This case instruments
// that primitive directly so ThreadSanitizer proves the contract instead of us reasoning about it.
//
// The race canary mirrors the real g_ui pattern: each posted task mutates NON-atomic shared state
// (a plain counter + a std::string append, exactly like assistantStreamBuf). That is data-race
// free ONLY if Drain() serialises every task onto the single draining ("UI") thread. If a task
// ever ran on a producer thread, or two Drain()s overlapped, TSan would flag the non-atomic
// writes and the value asserts would catch the lost updates. The atomic `ranCount` exists solely
// as the drainer's loop-termination signal — the non-atomic state is what TSan watches.
//
// Modelled on EditMetaCacheConcurrent.test.cpp / LocalCacheConcurrentSeed.test.cpp. ImGui/GL-free
// closure: MainThreadDispatcher.h (header-only) + UiPerfMonitor.cpp (the SMATCHET_UI_PERF_SCOPE in
// Drain()) — both ImGui-free.

#include "MainThreadDispatcher.h"

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace {

// 4 producers x 512 = 2048 total tasks, kept strictly below kMaxQueueSize (4096) so the
// drop-oldest-on-overflow path never fires regardless of producer/drainer interleaving — that
// keeps the count assertions exact (overflow drops are a separate, intentional behaviour and
// would make the totals timing-dependent).
constexpr int kProducers = 4;
constexpr int kTasksPerProducer = 512;
constexpr long kExpected = static_cast<long>(kProducers) * kTasksPerProducer;

} // namespace

TEST_CASE("MainThreadDispatcher: concurrent posters vs single drainer serialise tasks race-free") {
    MainThreadDispatcher dispatcher;

    // NON-atomic canary state — only ever touched inside a drained task. Race-free iff Drain()
    // runs every task on this one thread. This is the g_ui.assistantStreamBuf pattern in miniature.
    long nonAtomicSum = 0;
    std::string nonAtomicBuf;

    // Atomic purely so the drainer knows when every posted task has run (loop termination).
    std::atomic<long> ranCount(0);

    // Drainer = the "UI thread": spin Drain() until all expected tasks have executed. Producers
    // post concurrently below; because the total stays under kMaxQueueSize nothing is dropped, so
    // ranCount is guaranteed to reach kExpected.
    std::thread drainer([&] {
        while (ranCount.load(std::memory_order_acquire) < kExpected) {
            dispatcher.Drain();
            if (dispatcher.LastDrainTaskCount() == 0) {
                std::this_thread::yield(); // nothing ready yet — don't hot-spin the CPU
            }
        }
        dispatcher.Drain(); // final sweep for anything posted after the last loop check
    });

    // A second concurrent reader hammering the const QueueLen() accessor (which takes mutex_) so
    // TSan also instruments snapshot reads racing the posters' writes to queue_.
    std::atomic<bool> stopReader(false);
    std::thread queueReader([&] {
        while (!stopReader.load(std::memory_order_acquire)) {
            (void)dispatcher.QueueLen();
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kTasksPerProducer; ++i) {
                dispatcher.PostToMainThread([&] {
                    // Runs on the drainer thread only. Mutating non-atomic state here is the
                    // whole point — TSan certifies the serialisation.
                    nonAtomicSum += 1;
                    nonAtomicBuf.push_back('x');
                    ranCount.fetch_add(1, std::memory_order_acq_rel);
                });
            }
        });
    }

    for (std::thread& t : producers) {
        t.join();
    }
    drainer.join();
    stopReader.store(true, std::memory_order_release);
    queueReader.join();

    // Every task ran exactly once on the drain thread: no drops (under cap), no lost non-atomic
    // updates (would indicate a race the value check catches even if TSan somehow missed it).
    CHECK(ranCount.load() == kExpected);
    CHECK(nonAtomicSum == kExpected);
    CHECK(nonAtomicBuf.size() == static_cast<std::size_t>(kExpected));
    CHECK(dispatcher.QueueLen() == 0u);
}

TEST_CASE("MainThreadDispatcher: BeginShutdown races concurrent posters without data race") {
    MainThreadDispatcher dispatcher;

    std::atomic<long> drainedAfterShutdown(0);

    // Producers keep posting until told to stop; BeginShutdown() fires concurrently. This
    // exercises PostToMainThread's unlocked shutdown check + locked re-check against
    // BeginShutdown's store on the shuttingDown_ atom (the path that protects late posters from
    // touching an about-to-be-destroyed mutex).
    std::atomic<bool> stop(false);
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                dispatcher.PostToMainThread([&] { drainedAfterShutdown.fetch_add(0, std::memory_order_relaxed); });
            }
        });
    }

    // Let some posts accumulate, then shut down while producers are still racing.
    dispatcher.BeginShutdown();
    stop.store(true, std::memory_order_release);
    for (std::thread& t : producers) {
        t.join();
    }

    // After shutdown every post is a no-op, so any explicit post now must never reach the queue.
    const std::size_t lenBefore = dispatcher.QueueLen();
    dispatcher.PostToMainThread([&] { drainedAfterShutdown.fetch_add(1, std::memory_order_relaxed); });
    CHECK(dispatcher.QueueLen() == lenBefore); // post rejected post-shutdown

    // The final drain still empties whatever landed before BeginShutdown (shutdown ignores the
    // per-frame budget so the last drain runs the whole tail), and the post-shutdown task above
    // never ran.
    dispatcher.Drain();
    CHECK(dispatcher.QueueLen() == 0u);
    // The post-shutdown task is the only one that would have added +1; it was rejected, and every
    // pre-shutdown producer task adds 0, so the counter never moved.
    CHECK(drainedAfterShutdown.load() == 0);
}
