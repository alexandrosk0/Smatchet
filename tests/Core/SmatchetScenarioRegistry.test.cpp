// Slice 5 of docs/plans/shipped/autonomous-debugging-no-creds.md (V5.1).
//
// Snapshot test pinning the set of scenario names registered by
// RegisterAllScenarios(). Catches accidental adds/removes/renames during the
// extraction refactor and going forward. Because the underlying storage is a
// std::unordered_map (ScenarioRunner::factories_), order is not part of the
// contract — we compare as a set.
//
// The factories themselves are lambdas that refer to extern Make*Scenario()
// symbols defined in each scenario's TU. They are NEVER invoked by this test
// (we only call ScenarioRunner::ListNames), so the test target does NOT need
// to link the scenario implementations.

#include <doctest/doctest.h>

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

// The full, gating-aware expected set. Must match the byte-for-byte name list
// in Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp.
std::set<std::string> ExpectedNames() {
    std::set<std::string> expected;
    expected.insert("priority-grid-scroll");
    expected.insert("lua-recorder-fuzz");
    expected.insert("ui-test");
    expected.insert("dock-gap-sentinel");
    expected.insert("command-palette-fuzzy");
    expected.insert("code-syntax-coloring");
    expected.insert("theme-switch-roundtrip");
    // User Info window bucket-C goldens — 2x2 matrix (desktop/narrow x unified/separate).
    expected.insert("user-info-desktop-unified");
    expected.insert("user-info-desktop-separate");
    expected.insert("user-info-narrow-unified");
    expected.insert("user-info-narrow-separate");
#if defined(SMATCHET_WITH_AI)
    expected.insert("ai-chat-history-render");
#endif
    expected.insert("idle");
    expected.insert("command-contract-sweep");
    expected.insert("cell-edit-burst");
    expected.insert("attachment-preview-open");
    expected.insert("preferences-slider-drag");
    expected.insert("long-text-open-large-adf");
    // Slice 8 of autonomous-debugging-no-creds — 5 missing-bug-path scenarios.
    expected.insert("annotate-open-entry-tab");
    expected.insert("description-tooltip-markdown-render");
    // Multi-grid-tabs Slice 5b — multi-grid concurrency perf scenarios.
    expected.insert("side-by-side-2-grid");
    expected.insert("concurrent-sync");
    // Parametrised N-visible-pane render sweep (default 8).
    expected.insert("side-by-side-grids");
    // ACTIVE-load mixed-backend variant of side-by-side-grids.
    expected.insert("interactive-grid-stress");
    // Issue #1133 mobile CI smoke gate — always registered (no ifdef).
    expected.insert("mobile-texture-guard");
#if defined(SMATCHET_WITH_AI)
    expected.insert("ai-assistant-streaming-happy-path");
    expected.insert("ai-assistant-streaming-transport-down-within-5s");
    // Real-client S2/S4/S5 streaming scenario.
    expected.insert("ai-assistant-send-s2-s4-s5");
#endif
#if defined(SMATCHET_WITH_WHISPER)
    expected.insert("whisper-dictation-roundtrip");
    expected.insert("whisper-ai-assistant-autosend");
#endif
    return expected;
}

} // namespace

TEST_CASE("RegisterAllScenarios populates the expected scenario set") {
    smatchet::cmd::ScenarioRunner runner;
    smatchet::cmd::RegisterAllScenarios(runner);

    const std::vector<std::string> namesVec = runner.ListNames();
    const std::set<std::string> got(namesVec.begin(), namesVec.end());
    const std::set<std::string> expected = ExpectedNames();

    // Diff both directions so the failure message names the missing / extra
    // entries directly (doctest prints CHECK(got == expected) without diff).
    for (const std::string& name : expected) {
        INFO("expected name missing from registry: " << name);
        CHECK(got.count(name) == 1);
    }
    for (const std::string& name : got) {
        INFO("unexpected name registered: " << name);
        CHECK(expected.count(name) == 1);
    }
    CHECK(got.size() == expected.size());
}

TEST_CASE("RegisterAllScenarios is callable on a fresh runner without crash") {
    // Smoke — RegisterAllScenarios is documented as one-shot per AppController
    // init. Re-calling overwrites entries in the unordered_map but must not
    // throw or assert.
    smatchet::cmd::ScenarioRunner runner;
    smatchet::cmd::RegisterAllScenarios(runner);
    smatchet::cmd::RegisterAllScenarios(runner); // second call: tolerated.
    CHECK(runner.ListNames().size() >= 6);       // lower bound — gated entries vary.
}
