// CoderabbitCommentClassifier.h — phase-3 of the `coderabbit-react-loop`
// plan (docs/design/coderabbit-react-loop.md § Phased rollout § Phase 3).
// Concrete `PrCommentClassifier` impl that wires the phase-2 pure helpers
// (CoderabbitCommentClassifierPure.{h,cpp}) to the 18-rule override table
// from `agents/coderabbit-triage.md` § "Override rules — reject CodeRabbit
// suggestions that violate these". First match wins; matched rules emit a
// short-circuit-reject reply citing the rule number + canonical reason.
//
// Non-matching CodeRabbit comments fall through to a `Dispatch` verdict
// targeting `coderabbit-triage` — the spawned-harness mode of the same
// agent runs the full triage prose on bodies that the override table
// cannot reject deterministically.
//
// Phase-4 watcher wiring happens in PrCommentWatcher.cpp; this TU only
// owns the classification policy + reply-body construction. The classifier
// is a pure-virtual subclass of `PrCommentClassifier` so phase-4 tests can
// inject a fake without touching the 18-rule table.
//
// Build-time gating: source-list-conditional on SMATCHET_WITH_AGENTIC.
// AGENTIC=OFF builds never compile this TU.

#ifndef SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_H
#define SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_H

#if defined(SMATCHET_WITH_AGENTIC)

#include "PrCommentClassifier.h"

#include <string>
#include <vector>

namespace smatchet {
namespace agentic {

/// Concrete CodeRabbit comment classifier — applies the 18-rule override
/// table from `agents/coderabbit-triage.md` § Override rules. First match
/// wins; rules are encoded as static descriptor rows + detector functions
/// at file scope inside the .cpp TU.
///
/// Two configurable knobs: the bot-login allow-list (default
/// `{"coderabbitai[bot]"}`) and whether short-circuit-reject is enabled at
/// all (default true; turning it off forces every CodeRabbit comment down
/// the Dispatch path for triage agent review — debugging aid).
class CoderabbitCommentClassifier : public PrCommentClassifier {
  public:
    CoderabbitCommentClassifier();
    ~CoderabbitCommentClassifier() override;

    /// Set the bot-login allow-list. Case-insensitive match; empty list
    /// means no comments are recognised as CodeRabbit. Defaults to
    /// `{"coderabbitai[bot]"}`.
    void SetBotLoginAllowList(std::vector<std::string> allowList);
    const std::vector<std::string>& GetBotLoginAllowList() const;

    /// Enable/disable the short-circuit-reject path. When false, classifier
    /// always returns Dispatch for recognised bot comments (the orchestrator
    /// still routes them to coderabbit-triage). Default true.
    void SetShortCircuitRejectEnabled(bool enabled);
    bool GetShortCircuitRejectEnabled() const;

    /// Classify a posted comment. See PrCommentClassifier.h for verdict
    /// semantics. Algorithm:
    ///   1. If `HasSmatchetHandoffMarker(body)` or `HasRejectReplyMarker(body)`
    ///      → Skip ("smatchet marker").
    ///   2. If `!IsBotLoginAllowed(login, allowList)` → Skip ("non-bot author").
    ///   3. If `!shortCircuitRejectEnabled` → Dispatch to coderabbit-triage.
    ///   4. Walk the 18 override rules in numeric order. First match →
    ///      RejectShortCircuit (build the reply body citing rule N).
    ///   5. No match → Dispatch to coderabbit-triage.
    ClassificationResult Classify(const PostedComment& comment) const override;

    /// Build the reject-reply body shown to the PR thread. Cites the rule
    /// number + the canonical reason from the 18-rule table. Exposed for
    /// tests + for the watcher's audit-trail message. Marker prefix is
    /// `<!-- smatchet-coderabbit-react -->` (NOT the handoff marker — this
    /// is a different posting class; the watcher's bot-filter should treat
    /// both markers as "Smatchet authored").
    static std::string BuildRejectReplyBody(int ruleNumber, const std::string& ruleShortReason);

    /// Marker text used by BuildRejectReplyBody. Exposed for tests +
    /// future filters.
    static const char* RejectReplyMarker();

    /// Total number of override rules encoded in the .cpp descriptor
    /// table. Exposed so the markdown-table guard test can assert the
    /// number matches `agents/coderabbit-triage.md` without dragging the
    /// kOverrideRules array into the public header.
    static std::size_t OverrideRuleCount();

    /// Look up a rule's canonical short reason by rule number (1..18).
    /// Returns an empty string when `ruleNumber` is out of range. Exposed
    /// for tests that compare against the markdown-table column verbatim.
    static std::string LookupOverrideShortReason(int ruleNumber);

  private:
    std::vector<std::string> botLoginAllowList_;
    bool shortCircuitRejectEnabled_ = true;
};

} // namespace agentic
} // namespace smatchet

#endif // SMATCHET_WITH_AGENTIC
#endif // SMATCHET_CODERABBIT_COMMENT_CLASSIFIER_H
