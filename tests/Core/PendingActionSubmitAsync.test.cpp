// PendingActionSubmitAsync doctest — the worker/post-back idiom behind every queued tracker write from the
// UI (comments modal, worklog dialog, Annotate). The outcome must always reach the UI thread, a throwing
// submit included, so a caller's in-flight latch can never stay set (Quality Pillar 6); a failed launch
// throws to the caller instead, which then releases the latch itself.

#include "Ui/PendingActionSubmitAsync.h"

#include <doctest/doctest.h>

#include <functional>
#include <stdexcept>
#include <vector>

namespace {

// Runs each task inline and queues posts until Drain(); a throwing task is contained like the app's
// background-task firewall does.
class InlineThreading : public IAppThreading {
  public:
    bool FailLaunch = false;
    bool FailPost = false;
    int TasksThatThrew = 0;

    bool IsOnUiThread() const override { return true; }
    void PostToMainThread(std::function<void()> fn) override {
        if (FailPost) {
            throw std::runtime_error("post failed");
        }
        posted_.push_back(std::move(fn));
    }
    void LaunchBackgroundTask(std::function<void()> task) override {
        if (FailLaunch) {
            throw std::runtime_error("no thread");
        }
        try {
            task();
        } catch (const std::exception&) {
            ++TasksThatThrew;
        }
    }
    void Drain() {
        std::vector<std::function<void()>> posted;
        posted.swap(posted_);
        for (const std::function<void()>& fn : posted) {
            fn();
        }
    }

  private:
    std::vector<std::function<void()>> posted_;
};

PendingActionSubmitResult QueuedResult() {
    PendingActionSubmitResult r;
    r.K = PendingActionSubmitResult::Kind::Queued;
    r.QueueId = 7;
    return r;
}

} // namespace

TEST_CASE("SubmitPendingActionAsync posts the submit's result to the UI thread once") {
    InlineThreading threading;
    std::vector<PendingActionSubmitResult> applied;
    smatchet::ui::SubmitPendingActionAsync(
        threading, []() { return QueuedResult(); },
        [&applied](const PendingActionSubmitResult& r) { applied.push_back(r); });
    CHECK(applied.empty()); // applied on the UI thread, not inside the worker
    threading.Drain();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].K == PendingActionSubmitResult::Kind::Queued);
    CHECK(applied[0].QueueId == 7);
}

TEST_CASE("SubmitPendingActionAsync reports a throwing submit as Failed so the latch clears") {
    InlineThreading threading;
    std::vector<PendingActionSubmitResult> applied;
    smatchet::ui::SubmitPendingActionAsync(
        threading, []() -> PendingActionSubmitResult { throw std::runtime_error("submit threw"); },
        [&applied](const PendingActionSubmitResult& r) { applied.push_back(r); });
    CHECK(threading.TasksThatThrew == 1);
    threading.Drain();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].K == PendingActionSubmitResult::Kind::Failed);
}

TEST_CASE("SubmitPendingActionAsync throws to the caller when the task cannot launch") {
    InlineThreading threading;
    threading.FailLaunch = true;
    int applied = 0;
    CHECK_THROWS_AS(
        smatchet::ui::SubmitPendingActionAsync(
            threading, []() { return QueuedResult(); }, [&applied](const PendingActionSubmitResult&) { ++applied; }),
        std::runtime_error);
    threading.Drain();
    CHECK(applied == 0);
}

TEST_CASE("SubmitPendingActionAsync survives a post-back that throws (no terminate from the exit guard)") {
    InlineThreading threading;
    threading.FailPost = true;
    int applied = 0;
    smatchet::ui::SubmitPendingActionAsync(
        threading, []() { return QueuedResult(); }, [&applied](const PendingActionSubmitResult&) { ++applied; });
    CHECK(threading.TasksThatThrew == 1); // the first post's throw leaves the task; the exit guard's retry is caught
    threading.FailPost = false;
    threading.Drain();
    CHECK(applied == 0);
}
