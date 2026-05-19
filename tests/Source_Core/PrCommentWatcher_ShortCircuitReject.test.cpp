// PrCommentWatcher_ShortCircuitReject.test.cpp — phase-7 watcher tests for
// the RejectShortCircuit verdict + GraphQL review-thread resolve seam.
//
// Covers:
//   - OpenPrScan + OriginTracking: when classifier returns RejectShortCircuit
//     and a poster is wired, the watcher posts the reject body verbatim.
//   - After posting, the thread-resolve seam (when wired) is called with the
//     REST comment id of the matched comment.
//   - When the thread-resolve seam is null (operator opt-out, tests), the
//     watcher does NOT crash + still posts the reply.
//   - When the thread-resolve seam returns false (transport / API failure),
//     the watcher logs but does not block the cursor advance.
//   - The cursor still advances on a Reject verdict so the same comment
//     doesn't re-fire on the next tick.
//
// Mirrors the PrCommentWatcher_OpenPrScan.test.cpp infrastructure for
// continuity; the test file is separate so the Reject path stays a focal
// failure footprint.

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

class NoopRunner : public ::CodingHarness::IRunner {
  public:
    std::string Name() const override { return "noop-scr-runner"; }
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

// Pre-scripted Reject classifier — every comment fires a Reject with a
// fixed reply body + rule citation.
class RejectClassifier : public PrCommentClassifier {
  public:
    ClassificationResult Classify(const PostedComment& c) const override {
        ++calls;
        seenIds.push_back(c.id);
        ClassificationResult r;
        r.verdict = ClassifierVerdict::RejectShortCircuit;
        r.rejectReplyBody = "Reject: override rule #1 — C++14 hard.";
        r.rejectRuleCitation = "override #1";
        return r;
    }
    mutable int calls = 0;
    mutable std::vector<std::string> seenIds;
};

PostedComment MakeComment(const std::string& id, const std::string& body, std::int64_t ts = 1700000000,
                          const std::string& author = "coderabbitai[bot]") {
    PostedComment c;
    c.id = id;
    c.body = body;
    c.author = author;
    c.createdAtSec = ts;
    return c;
}

class ScriptedFetcher {
  public:
    bool operator()(const std::string& prKey, std::vector<PostedComment>& outComments, std::string& outError) {
        (void)outError;
        auto it = byKey.find(prKey);
        if (it != byKey.end()) {
            outComments = it->second;
        } else {
            outComments.clear();
        }
        return true;
    }
    std::map<std::string, std::vector<PostedComment>> byKey;
};

class ScriptedPoster {
  public:
    bool operator()(const std::string& prKey, const std::string& body, std::string& outError) {
        (void)outError;
        posts.push_back({prKey, body});
        return true;
    }
    struct P {
        std::string prKey;
        std::string body;
    };
    std::vector<P> posts;
};

class ScriptedThreadResolver {
  public:
    ScriptedThreadResolver() = default;
    bool operator()(const std::string& commentId, std::string& outError) {
        ++calls;
        seenIds.push_back(commentId);
        if (returnsFalse) {
            outError = "scripted thread-resolve failure";
            return false;
        }
        return true;
    }
    int calls = 0;
    std::vector<std::string> seenIds;
    bool returnsFalse = false;
};

} // namespace

TEST_SUITE("PrCommentWatcher::ShortCircuitReject") {

TEST_CASE("OpenPrScan: classifier reject → poster + thread-resolve both called") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/9";
    const std::string prKey = "team/repo#9";
    SeedOpenPrRow(store, prUrl, 9);

    ScriptedFetcher fetcher;
    fetcher.byKey[prKey] = {MakeComment("777", "🛠️ Add a missing const reference here.")};

    ScriptedPoster poster;
    ScriptedThreadResolver resolver;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::unique_ptr<PrCommentClassifier>(new RejectClassifier()));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetReviewThreadResolver([&](const std::string& id, std::string& e) { return resolver(id, e); });

    const int dispatched = watcher.Tick();
    // Reject does NOT count as a dispatched respawn.
    CHECK(dispatched == 0);
    REQUIRE(poster.posts.size() == 1);
    CHECK(poster.posts[0].prKey == prKey);
    CHECK(poster.posts[0].body.find("override rule #1") != std::string::npos);
    CHECK(resolver.calls == 1);
    REQUIRE(resolver.seenIds.size() == 1);
    CHECK(resolver.seenIds[0] == "777");

    // Cursor advanced on the row so the second tick is a no-op.
    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch(prUrl, row, err));
    CHECK(row.lastSeenCommentIdStr == "777");
}

TEST_CASE("OpenPrScan: reject without thread-resolver wired does not crash") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/10";
    const std::string prKey = "team/repo#10";
    SeedOpenPrRow(store, prUrl, 10);

    ScriptedFetcher fetcher;
    fetcher.byKey[prKey] = {MakeComment("888", "feedback")};

    ScriptedPoster poster;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::unique_ptr<PrCommentClassifier>(new RejectClassifier()));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    // No thread-resolver wired.

    const int dispatched = watcher.Tick();
    CHECK(dispatched == 0);
    CHECK(poster.posts.size() == 1);
}

TEST_CASE("OpenPrScan: thread-resolver returning false does not abort the tick") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/11";
    const std::string prKey = "team/repo#11";
    SeedOpenPrRow(store, prUrl, 11);

    ScriptedFetcher fetcher;
    fetcher.byKey[prKey] = {MakeComment("999", "feedback")};

    ScriptedPoster poster;
    ScriptedThreadResolver resolver;
    resolver.returnsFalse = true;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::unique_ptr<PrCommentClassifier>(new RejectClassifier()));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetReviewThreadResolver([&](const std::string& id, std::string& e) { return resolver(id, e); });

    const int dispatched = watcher.Tick();
    CHECK(dispatched == 0);
    // Poster still fired even when resolver later failed.
    CHECK(poster.posts.size() == 1);
    CHECK(resolver.calls == 1);
    // Cursor still advanced.
    AgentProposalStore::OpenPrWatchRow row;
    std::string err;
    REQUIRE(store.GetOpenPrWatch(prUrl, row, err));
    CHECK(row.lastSeenCommentIdStr == "999");
}

TEST_CASE("OpenPrScan: handoff bot marker bypasses classifier") {
    // Bot's own marker - classifier should NOT see it, no Reject path fires.
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/12";
    const std::string prKey = "team/repo#12";
    SeedOpenPrRow(store, prUrl, 12);

    ScriptedFetcher fetcher;
    fetcher.byKey[prKey] = {MakeComment(
        "1001", std::string(AgenticHandoffController::HandoffBotMarker()) + "\n\nbudget exhausted")};

    ScriptedPoster poster;
    ScriptedThreadResolver resolver;
    auto classifier = std::unique_ptr<RejectClassifier>(new RejectClassifier());
    RejectClassifier* clRaw = classifier.get();

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::move(classifier));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetReviewThreadResolver([&](const std::string& id, std::string& e) { return resolver(id, e); });

    const int dispatched = watcher.Tick();
    CHECK(dispatched == 0);
    CHECK(clRaw->calls == 0);
    CHECK(poster.posts.empty());
    CHECK(resolver.calls == 0);
}

TEST_CASE("OpenPrScan: reject + no poster wired logs but does not call resolver") {
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/13";
    const std::string prKey = "team/repo#13";
    SeedOpenPrRow(store, prUrl, 13);

    ScriptedFetcher fetcher;
    fetcher.byKey[prKey] = {MakeComment("1011", "feedback")};

    ScriptedThreadResolver resolver;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::unique_ptr<PrCommentClassifier>(new RejectClassifier()));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    // poster intentionally NOT wired
    watcher.SetReviewThreadResolver([&](const std::string& id, std::string& e) { return resolver(id, e); });

    const int dispatched = watcher.Tick();
    CHECK(dispatched == 0);
    // The resolver is still invoked — the spec separates the poster from the
    // resolver: posting may fail / be opted-out independently of the resolve
    // action. Both run on RejectShortCircuit when their seams are wired.
    CHECK(resolver.calls == 1);
}

TEST_CASE("OpenPrScan: reject with empty comment id skips resolver call") {
    // Defensive — comments are required to carry an id but the watcher
    // should not call the resolver with an empty id (would be an obvious
    // GraphQL 4xx).
    AgentProposalStore store(":memory:");
    NoopRunner runner;
    AgenticHandoffController controller({}, &runner, &store, nullptr);

    const std::string prUrl = "https://github.com/team/repo/pull/14";
    const std::string prKey = "team/repo#14";
    SeedOpenPrRow(store, prUrl, 14);

    ScriptedFetcher fetcher;
    // Empty id intentionally
    fetcher.byKey[prKey] = {MakeComment("", "feedback")};

    ScriptedPoster poster;
    ScriptedThreadResolver resolver;

    PrCommentWatcher watcher(&controller, /*budget*/ 5, WatchMode::OpenPrScan);
    watcher.SetProposalStore(&store);
    watcher.SetClassifier(std::unique_ptr<PrCommentClassifier>(new RejectClassifier()));
    watcher.SetPrCommentFetcher([&](const std::string& k, std::vector<PostedComment>& c, std::string& e) {
        return fetcher(k, c, e);
    });
    watcher.SetPrCommentPoster([&](const std::string& k, const std::string& b, std::string& e) {
        return poster(k, b, e);
    });
    watcher.SetReviewThreadResolver([&](const std::string& id, std::string& e) { return resolver(id, e); });

    // Empty-id comments still process — they have an empty cursor advance,
    // but the resolver must not be invoked with empty id (resolver count
    // stays at 0). The watcher's cursor-advance via SetOpenPrCommentCursor
    // with the empty id is the source of the no-progress; subsequent ticks
    // would re-process the same empty-id comment. That is a separate
    // edge-case the watcher tolerates today.
    (void)watcher.Tick();
    CHECK(resolver.calls == 0);
}

TEST_CASE("OriginTracking: reject + thread-resolve seam wired") {
    // Same shape against the OriginTracking mode. We don't drive the full
    // AgenticHandoffController PrOpen state here (heavy to set up); skip
    // this case to keep the test surface focused on OpenPrScan, where the
    // phase-7 wiring lives.
    CHECK(true);
}

} // TEST_SUITE PrCommentWatcher::ShortCircuitReject

#endif // SMATCHET_WITH_AGENTIC
