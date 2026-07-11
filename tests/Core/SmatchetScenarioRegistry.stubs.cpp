// Linker stubs for SmatchetScenarioRegistry.test.cpp.
//
// The registry under test (Source/Core/src/Commands/Scenarios/
// SmatchetScenarioRegistry.cpp) embeds lambdas that reference each scenario's
// MakeXScenario() factory function. The factories themselves live in the
// per-scenario .cpp files which pull in AppController + ImGui + per-scenario
// state — far too much to link into the doctest rig just to satisfy the
// references. The snapshot test never invokes any factory (it only calls
// ScenarioRunner::ListNames), so noreturn-style stubs are sufficient to
// satisfy the linker.
//
// Symbol set MUST stay in lock-step with the extern declarations at the top
// of SmatchetScenarioRegistry.cpp — when a new scenario is added there, add
// a matching stub here (the registry's snapshot test will then assert the
// name appears in ListNames()).

#include <memory>

#include "Commands/Scenarios/IScenario.h"

namespace {
std::unique_ptr<smatchet::cmd::IScenario> NullScenario() { return std::unique_ptr<smatchet::cmd::IScenario>(); }
} // namespace

std::unique_ptr<smatchet::cmd::IScenario> MakePriorityGridScrollScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeLuaRecorderFuzzScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeUiTestScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeDockGapSentinelScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeCommandPaletteFuzzyScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeCodeSyntaxColoringScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeThemeSwitchRoundtripScenario() { return NullScenario(); }
#if defined(SMATCHET_WITH_AI)
std::unique_ptr<smatchet::cmd::IScenario> MakeAiChatHistoryRenderScenario() { return NullScenario(); }
#endif
std::unique_ptr<smatchet::cmd::IScenario> MakeIdleScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeCommandContractSweepScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeCellEditBurstScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeAttachmentPreviewOpenScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakePreferencesSliderDragScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeLongTextOpenLargeAdfScenario() { return NullScenario(); }
// Slice 8 of autonomous-debugging-no-creds — 5 missing-bug-path scenarios.
std::unique_ptr<smatchet::cmd::IScenario> MakeAnnotateOpenEntryTabScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeDescriptionTooltipMarkdownRenderScenario() { return NullScenario(); }
// Multi-grid-tabs Slice 5b — multi-grid concurrency perf scenarios.
std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySide2GridScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeConcurrentSyncScenario() { return NullScenario(); }
// Parametrised N-visible-pane render sweep (default 8).
std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySideNGridScenario() { return NullScenario(); }
// ACTIVE-load mixed-backend variant of side-by-side-grids.
std::unique_ptr<smatchet::cmd::IScenario> MakeInteractiveGridStressScenario() { return NullScenario(); }
// Issue #1133 mobile CI smoke gate — always registered (no ifdef).
std::unique_ptr<smatchet::cmd::IScenario> MakeMobileTextureGuardScenario() { return NullScenario(); }
#if defined(SMATCHET_WITH_AI)
std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingHappyPathScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantStreamingTransportDownScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeAiAssistantSendScenario() { return NullScenario(); }
#endif
#if defined(SMATCHET_WITH_WHISPER)
std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperDictationScenario() { return NullScenario(); }
std::unique_ptr<smatchet::cmd::IScenario> MakeWhisperAiAssistantAutosendScenario() { return NullScenario(); }
#endif
