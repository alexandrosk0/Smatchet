// PrCommentWatcher_OpenPrScan.test.cpp — coderabbit-react phase-4 watcher
// tests for `WatchMode::OpenPrScan`. Lives in a separate TU from the
// OriginTracking-mode tests so the two modes' failure footprints stay
// independent (per the phase-4 plan).
//
// Covers:
//   - Basic iteration: dispatcher invoked once per `agent_open_pr_watch` row.
//   - Classifier verdicts: Skip / RejectShortCircuit / Dispatch, each
//     advances the cursor + behaves per the verdict.
//   - No-classifier fallback: every comment falls through to Dispatch.
//   - Smatchet-own marker: comments starting with the handoff marker are
//     skipped regardless of classifier registration.
//   - Budget exhaustion: rows already at the budget post the marker comment
//     and do not dispatch.
//   - Cursor idempotency: second tick with the same comment list skips the
//     already-processed comment.

#include "PrCommentWatcher.h"

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgentProposal.h"
#include "AgentProposalStore.h"
#include "AgenticHandoffController.h"
#include "CodingHarnessTypes.h"
#include "HarnessRunState.h"
#include "ICodingHarnessRunner.h"
#include "PrCommentClassifier.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using ::smatchet::agentic::AgenticHandoffController;
using ::smatchet::agentic::ClassificationResult;
using ::smatchet::agentic::ClassifierVerdict;
using ::smatchet::agentic::PostedComment;
using ::smatchet::agentic::PrCommentClassifier;
using ::smatchet::agentic::PrCommentWatcher;
using ::smatchet::agentic::WatchMode;

namespace {

// Minimal IRunner — unused in OpenPrScan tests (this mode never touches the
// AgenticHandoffController's runner-spawn paths) but the controller ctor
// requires a non-null pointer.
class NoopRunner : public ::CodingHarness::IRunner {
  public:
    std::string Name() const override { return "noop-openprscan-runner"; }
    bool Probe(std::string&) override { return true; }
    bool Spawn(const ::CodingHarness::Seed&, const std::string&,
               ::CodingHarness::IRunner::DeltaCallback,
               ::CodingHarness::IRunner::StateChangeCallback, std::shared_ptr<std::atomic<bool>>,
               ::CodingHarness::RunResult& outResult, std::string& outError) override {
        outResult.ok = false;
        outResult.errorMessage = "noop";
        outError = outResult.errorMessage;
        return false;
    }
    bool Resume(const ::CodingHarness::ClarificationResponse&, std::shared_ptr<std::atomic<bool>>,
                std::string& outError) override {
        outError = "noop";
        return false;
    }
};

// Pre-populate the `agent_open_pr_watch` table with one row.
void SeedOpenPrRow(AgentProposalStore& store, const std::string& prUrl, int prNumber) {
    AgentProposalStore::OpenPrWatchRow row;
    row.prUrl = prUrl;
    row.prNumber = prNumber;
    row.headRefName = "feature/branch";
    row.headSha = "abc123";
    row.lastSeenCommentIdStr = "";
    row.iterationCount = 0;
    row.lastPolledAtSec = 0;
    std::string err;
    REQUIRE(store.SetOpenPrWatch(row, err));
}

// Scripted seam types — one fetcher returns a per-prKey comment list, one
// poster captures (prKey, body) pairs, one dispatcher captures
// (prUrl, body, target) tuples.
class ScriptedOpenPrFetcher {
  public:
    bool operator()(const std::string& prKey, std::vector<PostedComment>& outComments, std::string& outError) {
        (void)outError;
        ++callCount;
        seenKeys.push_back(prKey);
        // Look up the canned list for this prKey; otherwise return empty.
        auto it = byPrKey.find(prKey);
        if (it != byPrKey.end()) {
            outComments = it->second;
        } else {
            outComments.clear();
        }
        return true;
    }
    int callCount = 0;
    std::vector<std::string> seenKeys;
    std::map<std::string, std::vector<PostedComment>> byPrKey;
};

class ScriptedOpenPrPoster {
  public:
    bool operator()(const std::string& prKey, const std::string& body, std::string& outError) {
        (void)outError;
        posts.push_back({prKey, body});
        return true;
    }
    struct Post {
        std::string prKey;
        std::string body;
    };
    std::vector<Post> posts;
};

class ScriptedOpenPrDispatcher {
  public:
    bool operator()(const std::string& prUrl, const std::string& body, const std::string& target,
                    std::string& outError) {
        (void)outError;
        dispatched.push_back({prUrl, body, target});
        return true;
    }
    struct D {
        std::string prUrl;
        std::string body;
        std::string target;
    };
    std::vector<D> dispatched;
};

// FakeClassifier — pulls a scripted verdict from a per-call queue. Falls
// through to a default `Dispatch` with target = "coderabbit-triage" once the
// queue is drained.
class FakeClassifier : public PrCommentClassifier {
  public:
    ClassificationResult Classify(const PostedComment& c) const override {
        ++calls;
        seen.push_back(c.body);
        if (queue.empty()) {
            ClassificationResult fallback;
            fallback.verdict = ClassifierVerdict::Dispatch;
            fallback.dispatchTargetAgent = "coderabbit-triage";
            return fallback;
        }
        auto r = queue.front();
        queue.erase(queue.begin());
        return r;
    }
    mutable int calls = 0;
    mutable std::vector<std::string> seen;
    mutable std::vector<ClassificationResult> queue;
};

PostedComment MakeComment(const std::string& id, const std::string& body, std::int64_t ts = 1700000000) {
    PostedComment c;
    c.id = id;
    c.body = body;
    c.author = "coderabbitai[bot]";
    c.createdAtSec = ts;
    return c;
}

} // namespace

TEST_SUITE("PrCommentWatcher::OpenPrScan") {

TEST_CASE("basic iteration: dispatcher called once per row with the expected comment body") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/1", 1);
    SeedOpenPrRow(store, "https://github.com/team/repo/pull/2", 2);

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#1"] = {MakeComment("100", "feedback on PR 1")};
    fetcher.byPrKey["team/repo#2"] = {MakeComment("200", "feedback on PR 2")};

    ScriptedOpenPrPoster poster;
    ScriptedOpenPrDispatcher dispatcher;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    CHECK(watcher.GetMode() == WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });

    const int n = watcher.Tick();
    CHECK(n == 2);
    REQUIRE(dispatcher.dispatched.size() == 2);
    // Iteration order is `pr_url ASC` per ListOpenPrWatch.
    CHECK(dispatcher.dispatched[0].prUrl == "https://github.com/team/repo/pull/1");
    CHECK(dispatcher.dispatched[0].body == "feedback on PR 1");
    CHECK(dispatcher.dispatched[1].prUrl == "https://github.com/team/repo/pull/2");
    CHECK(dispatcher.dispatched[1].body == "feedback on PR 2");
    CHECK(poster.posts.empty());

    // Cursors advanced + iteration counts bumped.
    AgentProposalStore::OpenPrWatchRow r;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/1", r, err));
    CHECK(r.lastSeenCommentIdStr == "100");
    CHECK(r.iterationCount == 1);
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/2", r, err));
    CHECK(r.lastSeenCommentIdStr == "200");
    CHECK(r.iterationCount == 1);
}

TEST_CASE("classifier registration: Skip / Reject / Dispatch") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/10", 10);
    SeedOpenPrRow(store, "https://github.com/team/repo/pull/20", 20);
    SeedOpenPrRow(store, "https://github.com/team/repo/pull/30", 30);

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#10"] = {MakeComment("10", "skip me", 1700000010)};
    fetcher.byPrKey["team/repo#20"] = {MakeComment("20", "reject me", 1700000020)};
    fetcher.byPrKey["team/repo#30"] = {MakeComment("30", "dispatch me", 1700000030)};

    auto classifier = std::unique_ptr<FakeClassifier>(new FakeClassifier());
    FakeClassifier* classifierPtr = classifier.get();
    ClassificationResult skipRes;
    skipRes.verdict = ClassifierVerdict::Skip;
    skipRes.skipReason = "test-skip";
    classifierPtr->queue.push_back(skipRes);
    ClassificationResult rejectRes;
    rejectRes.verdict = ClassifierVerdict::RejectShortCircuit;
    rejectRes.rejectReplyBody = "smatchet rejects via test fixture";
    rejectRes.rejectRuleCitation = "override #fake";
    classifierPtr->queue.push_back(rejectRes);
    ClassificationResult dispatchRes;
    dispatchRes.verdict = ClassifierVerdict::Dispatch;
    dispatchRes.dispatchTargetAgent = "coderabbit-triage";
    classifierPtr->queue.push_back(dispatchRes);

    ScriptedOpenPrPoster poster;
    ScriptedOpenPrDispatcher dispatcher;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::move(classifier));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });

    const int n = watcher.Tick();
    CHECK(n == 1); // Only one Dispatch verdict
    CHECK(classifierPtr->calls == 3);

    // Skip row: no dispatch, no poster, cursor advanced.
    REQUIRE(dispatcher.dispatched.size() == 1);
    CHECK(dispatcher.dispatched[0].prUrl == "https://github.com/team/repo/pull/30");
    CHECK(dispatcher.dispatched[0].target == "coderabbit-triage");

    // Reject row: poster called with the reject body.
    REQUIRE(poster.posts.size() == 1);
    CHECK(poster.posts[0].body == "smatchet rejects via test fixture");
    CHECK(poster.posts[0].prKey == "team/repo#20");

    // All three cursors advanced.
    AgentProposalStore::OpenPrWatchRow r;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/10", r, err));
    CHECK(r.lastSeenCommentIdStr == "10");
    CHECK(r.iterationCount == 0); // Skip does not bump
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/20", r, err));
    CHECK(r.lastSeenCommentIdStr == "20");
    CHECK(r.iterationCount == 0); // Reject does not bump
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/30", r, err));
    CHECK(r.lastSeenCommentIdStr == "30");
    CHECK(r.iterationCount == 1); // Dispatch bumps
}

TEST_CASE("no-classifier fallback: every comment falls through to dispatcher") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/100", 100);

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#100"] = {MakeComment("ABC", "first comment")};
    ScriptedOpenPrDispatcher dispatcher;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    // No classifier registered.
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });

    const int n = watcher.Tick();
    CHECK(n == 1);
    REQUIRE(dispatcher.dispatched.size() == 1);
    CHECK(dispatcher.dispatched[0].body == "first comment");
    // Empty target when no classifier — phase-7 widens this.
    CHECK(dispatcher.dispatched[0].target.empty());
}

TEST_CASE("Smatchet-own marker is skipped regardless of classifier") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/55", 55);

    ScriptedOpenPrFetcher fetcher;
    PostedComment botComment;
    botComment.id = "bot1";
    botComment.body = std::string(AgenticHandoffController::HandoffBotMarker()) +
                      "\n\nbudget exhausted notice (smatchet posted)";
    botComment.author = "smatchet[bot]";
    botComment.createdAtSec = 1700000999;
    fetcher.byPrKey["team/repo#55"] = {botComment};

    ScriptedOpenPrDispatcher dispatcher;
    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });
    // Even with a classifier that would return Dispatch, the bot-filter
    // pre-empt rejects the marker before the classifier sees it.
    auto classifier = std::unique_ptr<FakeClassifier>(new FakeClassifier());
    FakeClassifier* classifierPtr = classifier.get();
    watcher.SetClassifier(std::move(classifier));

    const int n = watcher.Tick();
    CHECK(n == 0);
    CHECK(classifierPtr->calls == 0); // classifier never invoked on bot marker
    CHECK(dispatcher.dispatched.empty());
    // Cursor still advanced past the bot comment.
    AgentProposalStore::OpenPrWatchRow r;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/55", r, err));
    CHECK(r.lastSeenCommentIdStr == "bot1");
}

TEST_CASE("budget exhaustion: marker posted, no further increment, no dispatch") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    // Pre-populate at budget = 3.
    AgentProposalStore::OpenPrWatchRow seed;
    seed.prUrl = "https://github.com/team/repo/pull/99";
    seed.prNumber = 99;
    seed.iterationCount = 3;
    std::string err;
    REQUIRE(store.SetOpenPrWatch(seed, err));

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#99"] = {MakeComment("777", "would-trigger-dispatch")};
    ScriptedOpenPrPoster poster;
    ScriptedOpenPrDispatcher dispatcher;

    PrCommentWatcher watcher(&controller, /*budget*/ 3, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });

    const int n = watcher.Tick();
    CHECK(n == 0);
    REQUIRE(poster.posts.size() == 1);
    CHECK(poster.posts[0].body.find("budget exhausted") != std::string::npos);
    CHECK(dispatcher.dispatched.empty());

    // iteration_count should NOT be bumped past the budget.
    AgentProposalStore::OpenPrWatchRow r;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/99", r, err));
    CHECK(r.iterationCount == 3);
}

TEST_CASE("cursor advance idempotency: second tick with same comment list is a no-op") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/7", 7);

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#7"] = {MakeComment("555", "the only comment")};
    ScriptedOpenPrDispatcher dispatcher;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetOpenPrRespawnDispatcher([&](const std::string& url, const std::string& b, const std::string& t,
                                            std::string& e) { return dispatcher(url, b, t, e); });

    CHECK(watcher.Tick() == 1);
    CHECK(dispatcher.dispatched.size() == 1);
    // Second tick: the cursor is now at "555", the only comment in the list
    // has id "555", so no comment is strictly greater — no dispatch.
    CHECK(watcher.Tick() == 0);
    CHECK(dispatcher.dispatched.size() == 1);
}

TEST_CASE("OpenPrScan without store wired is a no-op") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    ScriptedOpenPrFetcher fetcher;
    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    // No store wired — Tick returns 0 + LOG_INFO-latches once.
    CHECK(watcher.Tick() == 0);
    CHECK(fetcher.callCount == 0);
}

TEST_CASE("OpenPrScan default-construct mode is OriginTracking") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);
    PrCommentWatcher watcher(&controller, /*budget*/ 5);
    CHECK(watcher.GetMode() == WatchMode::OriginTracking);
}

TEST_CASE("OpenPrScan: Dispatch verdict without OpenPrRespawnDispatcher latches a warning + still advances cursor") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller(/*dispatcher*/ {}, &runner, &store, nullptr);

    SeedOpenPrRow(store, "https://github.com/team/repo/pull/42", 42);

    ScriptedOpenPrFetcher fetcher;
    fetcher.byPrKey["team/repo#42"] = {MakeComment("9001", "needs work")};

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    // NO openPrDispatcher wired.
    const int n = watcher.Tick();
    CHECK(n == 0);
    // Cursor still advanced so subsequent ticks do not re-fire.
    AgentProposalStore::OpenPrWatchRow r;
    std::string err;
    REQUIRE(store.GetOpenPrWatch("https://github.com/team/repo/pull/42", r, err));
    CHECK(r.lastSeenCommentIdStr == "9001");
    CHECK(r.iterationCount == 0); // No dispatch -> no bump
}

} // TEST_SUITE

#endif // SMATCHET_WITH_AGENTIC
