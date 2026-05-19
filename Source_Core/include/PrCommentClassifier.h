// PrCommentClassifier.h — phase-2 of the `coderabbit-react-loop` plan
// (docs/design/coderabbit-react-loop.md § Phase 2). Defines the abstract
// `PrCommentClassifier` interface that `PrCommentWatcher` invokes (phase 4)
// before its existing default dispatch path to `pr-iterator`. Concrete
// classifiers (`CoderabbitCommentClassifier` — phase 3) decide whether a
// posted PR comment should:
//   - `Dispatch` — spawn a coding harness, routing to the named delegate
//     written into SEED.md's `## First delegate:` line. The watcher hands
//     this off to `AgenticHandoffController` which owns the spawn.
//   - `RejectShortCircuit` — pre-built markdown reply body that the watcher
//     posts via `GitHubClient::CommentAdd`. No spawn cost; the audit row
//     records the rule citation that fired.
//   - `Skip` — ignore the comment (bot's own marker, empty body, comment
//     author not on the allow-list). The watcher advances its cursor and
//     proceeds without further work.
//
// Pure-virtual on purpose so phase-4 watcher tests can inject a deterministic
// fake classifier (see AGENTS.md § "Pure-helper TU-split recipe" — the
// classifier interface itself is the seam, the helpers live in
// CoderabbitCommentClassifierPure.{h,cpp}).
//
// Build-time gating: header body is `#if defined(SMATCHET_WITH_AGENTIC)`-
// gated — `PostedComment` lives behind the gate in `AgenticHandoffController.h`.

#ifndef SMATCHET_PR_COMMENT_CLASSIFIER_H
#define SMATCHET_PR_COMMENT_CLASSIFIER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "AgenticHandoffController.h" // for PostedComment

#include <string>

namespace smatchet {
namespace agentic {

enum class ClassifierVerdict {
    /// Spawn a coding harness; route to the named delegate.
    Dispatch,
    /// Reject without spawning; the watcher posts the pre-built reply body
    /// via GitHubClient::CommentAdd. Audit trail captures the rule citation.
    RejectShortCircuit,
    /// Ignore — bot's own marker, malformed body, comment author not on the
    /// allow-list, etc. The watcher advances its cursor without further work.
    Skip,
};

struct ClassificationResult {
    ClassifierVerdict verdict = ClassifierVerdict::Skip;
    /// For Dispatch: the agent name (e.g. "coderabbit-triage") written into
    /// SEED.md's `## First delegate:` line OR the SEED.json.dispatch_source
    /// discriminator. Empty for non-Dispatch verdicts.
    std::string dispatchTargetAgent;
    /// For RejectShortCircuit: the markdown body to post as the PR reply.
    /// Empty for non-Reject verdicts.
    std::string rejectReplyBody;
    /// For RejectShortCircuit: short rule citation written into the audit
    /// row (e.g. "override #1 — C++14 hard"). Empty for non-Reject verdicts.
    std::string rejectRuleCitation;
    /// For Skip: short reason for the audit row (e.g. "bot's own marker",
    /// "empty body"). Empty when the watcher's caller does not audit Skip.
    std::string skipReason;
};

/// Abstract classifier — concrete impls (CoderabbitCommentClassifier in
/// phase 3) inject into PrCommentWatcher via a registration API added in
/// phase 4. The interface is pure-virtual so the unit test for phase 4 can
/// inject a deterministic fake.
class PrCommentClassifier {
  public:
    virtual ~PrCommentClassifier() = default;
    virtual ClassificationResult Classify(const PostedComment& comment) const = 0;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_PR_COMMENT_CLASSIFIER_H
