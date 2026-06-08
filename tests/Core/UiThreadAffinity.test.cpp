// UiThreadAffinity — low-layer UI-thread registry behind the close-gate-gaps Slice-1a
// Pillar-2 gate. Pins the contract the blocking wrappers (ConfigManager / P4RunCommand)
// rely on: after SetUiThread() registers a thread, IsOnUiThread()/WarnIfOnUiThread() are
// true on THAT thread and false on any other.
//
// Note on global state: the registry is process-global publish-once (mirroring
// AppController's uiThreadId_), so this test asserts the post-registration contract rather
// than the pre-registration fail-open (which is order-dependent across the doctest binary).
// SetUiThread() is idempotent for the same thread, so re-registering the doctest main
// thread here is safe regardless of whether an earlier test already registered it.

#include "UiThreadAffinity.h"

#include <doctest/doctest.h>

#include <thread>

TEST_CASE("UiThreadAffinity: registered thread is UI; other threads are not") {
    UiThreadAffinity::SetUiThread(); // register the doctest main thread as the UI thread

    CHECK(UiThreadAffinity::IsOnUiThread());
    // On the UI thread, the blocking-I/O guard warns and reports it did.
    CHECK(UiThreadAffinity::WarnIfOnUiThread("test:on-ui"));

    bool workerSawUi = true;
    bool workerWarned = true;
    std::thread worker([&]() {
        workerSawUi = UiThreadAffinity::IsOnUiThread();
        workerWarned = UiThreadAffinity::WarnIfOnUiThread("test:off-ui");
    });
    worker.join();

    // A worker thread is never the UI thread, so the guard is silent there.
    CHECK_FALSE(workerSawUi);
    CHECK_FALSE(workerWarned);
}

TEST_CASE("UiThreadAffinity: WarnIfOnUiThread tolerates a null context") {
    UiThreadAffinity::SetUiThread();
    // Must not crash on a null context string (defensive — wrappers pass string literals,
    // but the contract is null-safe).
    CHECK(UiThreadAffinity::WarnIfOnUiThread(nullptr));
}
