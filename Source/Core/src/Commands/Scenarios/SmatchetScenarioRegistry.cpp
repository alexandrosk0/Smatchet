// Slice 5 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure refactor.
// See Source/Core/include/Commands/Scenarios/SmatchetScenarioRegistry.h for
// the behaviour contract.
// Each entry mirrors the pre-refactor `scenarioRunner_->RegisterFactory(...)`
// call from AppController::Initialize byte-for-byte: same name, same factory,
// same ifdef gating. The pre-refactor block in AppController.cpp declared each
// `extern std::unique_ptr<smatchet::cmd::IScenario> MakeXScenario();` inside
// the lambda body — at the lambda's enclosing namespace, which was AppCtl's
// translation-unit global scope (AppController::Initialize is a member of a
// global-namespace class). Each scenario .cpp defines its `MakeXScenario()`
// at global scope. So the externs MUST be declared at global scope here too
// (not inside `namespace smatchet::cmd`) — otherwise lookup mangles them as
// `smatchet::cmd::MakeXScenario` and the linker can't resolve them.

#include "Commands/Scenarios/SmatchetScenarioRegistry.h"

#include <memory>

#include "Commands/Scenarios/IScenario.h"

// Forward declarations at GLOBAL scope — these must match the (unqualified)
// definitions in each scenario's .cpp file (`std::unique_ptr<smatchet::cmd::
// IScenario> MakeXScenario()` at file scope, no enclosing namespace).
extern std::unique_ptr<smatchet::cmd::IScenario> MakePriorityGridScrollScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeLuaRecorderFuzzScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeUiTestScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeDockGapSentinelScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeCommandPaletteFuzzyScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeCodeSyntaxColoringScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeThemeSwitchRoundtripScenario();
#if defined(SMATCHET_WITH_AI)
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAiChatHistoryRenderScenario();
#endif
extern std::unique_ptr<smatchet::cmd::IScenario> MakeIdleScenario();
// Gap map Tier 3 — registry-wide command error-envelope contract sweep (mutation-free:
// every probe is rejected by the Dispatch guards before any handler runs).
extern std::unique_ptr<smatchet::cmd::IScenario> MakeCommandContractSweepScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeCellEditBurstScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAttachmentPreviewOpenScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakePreferencesSliderDragScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeLongTextOpenLargeAdfScenario();
// Slice 8 of autonomous-debugging-no-creds — 5 missing-bug-path scenarios.
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAnnotateOpenEntryTabScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeDescriptionTooltipMarkdownRenderScenario();
// Multi-grid-tabs Slice 5b — two multi-grid concurrency / registration perf scenarios.
extern std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySide2GridScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeConcurrentSyncScenario();
// Parametrised N-visible-pane sweep (default 8) — the operator-driven "how many
// grids before the budget blows" probe the multi-grid plan § Performance left open.
extern std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySideNGridScenario();
// ACTIVE-load variant of the N-pane sweep: mixed backends (4 Jira / 2 GitHub /
// 2 Plane) under per-frame scroll + pane-focus + view switching. Operator probe
// for the "does 8 panes hold budget while a user USES it" question.
extern std::unique_ptr<smatchet::cmd::IScenario> MakeInteractiveGridStressScenario();
// Issue #1133 mobile CI smoke gate — drives the dynamic-texture guard's
// [P0 #1122] repoint path end-to-end. Always registered (the fault-injector /
// guard symbols are no-ops when SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION is OFF).
extern std::unique_ptr<smatchet::cmd::IScenario> MakeMobileTextureGuardScenario();
#if defined(SMATCHET_WITH_AI)
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingHappyPathScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingTransportDownScenario();
// S2/S4/S5 streaming via a REAL OpenAiClient over an in-process httplib
// loopback server (vs the stub-driven happy-path / transport-down scenarios above).
extern std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantSendScenario();
#endif
#if defined(SMATCHET_WITH_WHISPER)
extern std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperDictationScenario();
extern std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperAiAssistantAutosendScenario();
#endif

namespace smatchet {
namespace cmd {

void RegisterAllScenarios(ScenarioRunner& runner) {
    runner.RegisterFactory("priority-grid-scroll", []() { return ::MakePriorityGridScrollScenario(); });
    runner.RegisterFactory("lua-recorder-fuzz", []() { return ::MakeLuaRecorderFuzzScenario(); });
    runner.RegisterFactory("ui-test", []() { return ::MakeUiTestScenario(); });
    // Phase 7 bucket-C screenshot-diff scenarios (test-suite-expansion-
    // completion plan). Each scenario drives the UI to a known steady state,
    // then triggers debug.window.screenshot so the bash driver can diff the
    // captured PPM against tests/golden/<name>.ppm.
    runner.RegisterFactory("dock-gap-sentinel", []() { return ::MakeDockGapSentinelScenario(); });
    runner.RegisterFactory("command-palette-fuzzy", []() { return ::MakeCommandPaletteFuzzyScenario(); });
    // code-syntax-coloring — bucket-C golden for the multi-language code-block
    // colouring feature, see code-syntax-coloring-and-tooltips.
    // Closes the deferred tooling.md P3 (2026-05-15) "no golden-image screenshot
    // diff for syntax highlighting" gap.
    runner.RegisterFactory("code-syntax-coloring", []() { return ::MakeCodeSyntaxColoringScenario(); });
    // theme-switch-roundtrip — bucket-C guard for the user-reported residual-
    // colour bug on SmatchetDark <-> NortonCommander <-> SmatchetDark. See
    // Source/Core/include/Commands/Scenarios/ThemeSwitchRoundtripScenario.h.
    runner.RegisterFactory("theme-switch-roundtrip", []() { return ::MakeThemeSwitchRoundtripScenario(); });
#if defined(SMATCHET_WITH_AI)
    // Phase 6.7 of ai-chat-claude-desktop-parity. Seeds N mock messages into
    // g_ui.assistantHistory, runs N frames so the new DrawHistoryArea /
    // DrawPinStripIfAny / renderTurn perf scopes accumulate measurable timings.
    runner.RegisterFactory("ai-chat-history-render", []() { return ::MakeAiChatHistoryRenderScenario(); });
#endif
    // perf-tooling-bundle scenarios — 5 perf scenarios surfaced by the
    // perf-detective audit on develop@31e1893. Each verifies a previously-
    // shipped pillar-1 / pillar-2 fix doesn't regress, or establishes the
    // baseline floor against which other scenarios are compared.
    runner.RegisterFactory("idle", []() { return ::MakeIdleScenario(); });
    runner.RegisterFactory("command-contract-sweep", []() { return ::MakeCommandContractSweepScenario(); });
    runner.RegisterFactory("cell-edit-burst", []() { return ::MakeCellEditBurstScenario(); });
    runner.RegisterFactory("attachment-preview-open", []() { return ::MakeAttachmentPreviewOpenScenario(); });
    runner.RegisterFactory("preferences-slider-drag", []() { return ::MakePreferencesSliderDragScenario(); });
    runner.RegisterFactory("long-text-open-large-adf", []() { return ::MakeLongTextOpenLargeAdfScenario(); });

    // Slice 8 of autonomous-debugging-no-creds — closes the missing-bug-path
    // scenario backlog gaps (tooling.md P2 line 178; tooling.md P2 line 56;
    // test.md P2 AI streaming S2/S4/S5; defensive cover for be2b1d9 wrapWidth
    // tooltip regression).
    runner.RegisterFactory("annotate-open-entry-tab", []() { return ::MakeAnnotateOpenEntryTabScenario(); });
    runner.RegisterFactory("description-tooltip-markdown-render",
                           []() { return ::MakeDescriptionTooltipMarkdownRenderScenario(); });
    // Multi-grid-tabs Slice 5b — perf coverage of the multi-grid concurrency the
    // plan flagged: side-by-side-2-grid (two panes drawing each frame) +
    // concurrent-sync (two live contexts under TickAllContexts' shared deadline).
    // Both are declared in scripts/dev/perf-pr-fast-set.json so the perf gate
    // covers multi-grid on every Core PR.
    runner.RegisterFactory("side-by-side-2-grid", []() { return ::MakeSideBySide2GridScenario(); });
    runner.RegisterFactory("concurrent-sync", []() { return ::MakeConcurrentSyncScenario(); });
    // side-by-side-grids — parametrised N-visible-pane render sweep (default 8,
    // override with --panes=N). Not in the PR-fast gate set: it is an operator
    // probe (variable N → no fixed baseline), not a regression guard.
    runner.RegisterFactory("side-by-side-grids", []() { return ::MakeSideBySideNGridScenario(); });
    runner.RegisterFactory("interactive-grid-stress", []() { return ::MakeInteractiveGridStressScenario(); });
    // Issue #1133 mobile CI smoke gate — no ifdef: the TU compiles on every
    // preset and the guard/injector calls are no-ops when the fault-injection
    // macro is OFF (recoveryLogSeen stays false, which is expected off-preset).
    runner.RegisterFactory("mobile-texture-guard", []() { return ::MakeMobileTextureGuardScenario(); });
#if defined(SMATCHET_WITH_AI)
    runner.RegisterFactory("ai-assistant-streaming-happy-path",
                           []() { return ::MakeAiAssistantStreamingHappyPathScenario(); });
    runner.RegisterFactory("ai-assistant-streaming-transport-down-within-5s",
                           []() { return ::MakeAiAssistantStreamingTransportDownScenario(); });
    // Real-client S2 (happy) / S4 (401 bad-key) / S5 (transport-down)
    // streaming over an in-process httplib loopback server. See
    // AiAssistantSendScenario.cpp + scripts/dev/test-ai-assistant.sh.
    runner.RegisterFactory("ai-assistant-send-s2-s4-s5", []() { return ::MakeAiAssistantSendScenario(); });
#endif

#if defined(SMATCHET_WITH_WHISPER)
    // Phase G — end-to-end whisper-dictation regression gate. The scenario
    // TU is source-list-conditional (only added to CORE_SOURCES when the
    // CMake option is ON), so the factory call must be ifdef-wrapped too:
    // the OFF build has no symbol for MakeWhisperDictationScenario.
    runner.RegisterFactory("whisper-dictation-roundtrip", []() { return ::MakeWhisperDictationScenario(); });
    // Regression gate (fix at bf9ce67f) — focuses the AI Assistant chat
    // input via router registration + non-zero ItemId so the
    // ReloadUserBufAndMoveToEnd half of the fix is exercised end-to-end.
    // Source-list conditional (CMakeLists.txt) + ifdef-wrapped here for the
    // same reason as the dictation-roundtrip factory.
    runner.RegisterFactory("whisper-ai-assistant-autosend",
                           []() { return ::MakeWhisperAiAssistantAutosendScenario(); });
#endif
}

} // namespace cmd
} // namespace smatchet
