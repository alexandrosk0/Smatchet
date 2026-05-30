// Aggregates per-feature ImGui Test Engine test-registration entry points.
// Called once from UiTestScenario::OnStart() after the engine context has
// been created. New per-feature test files declare their `RegisterX(engine)`
// here and prepend a call below.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_engine.h"

extern "C" void SmatchetRegisterViewsColumnsReorderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterCallstackTooltipHoverTests(ImGuiTestEngine* engine);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
extern "C" void SmatchetRegisterMcpLuaFreshStateRaceTests(ImGuiTestEngine* engine);
#endif
extern "C" void SmatchetRegisterSyncStallVisibleCueTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterDescriptionTooltipMarkdownRenderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterSpawnWarmupDeterministicGateTests(ImGuiTestEngine* engine);
#if defined(SMATCHET_WITH_AI)
extern "C" void SmatchetRegisterAiAssistantPanelDockSwapTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantEnterSendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiPrefsAutosaveFlowTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesDockingTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesEnterSendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesValidationBannerTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesSaveDiscardTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesTestConnectionTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesVerifyOnSaveTests(ImGuiTestEngine* engine);
#endif
#if defined(SMATCHET_WITH_AI) && defined(SMATCHET_WITH_WHISPER)
extern "C" void SmatchetRegisterWhisperAiAssistantAutosendTests(ImGuiTestEngine* engine);
#endif

extern "C" void SmatchetRegisterAllUiTests(ImGuiTestEngine* engine) {
    SmatchetRegisterViewsColumnsReorderTests(engine);
    SmatchetRegisterCallstackTooltipHoverTests(engine);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    SmatchetRegisterMcpLuaFreshStateRaceTests(engine);
#endif
    SmatchetRegisterSyncStallVisibleCueTests(engine);
    SmatchetRegisterDescriptionTooltipMarkdownRenderTests(engine);
    SmatchetRegisterSpawnWarmupDeterministicGateTests(engine);
#if defined(SMATCHET_WITH_AI)
    SmatchetRegisterAiAssistantPanelDockSwapTests(engine);
    SmatchetRegisterAiAssistantEnterSendTests(engine);
    SmatchetRegisterAiPrefsAutosaveFlowTests(engine);
    SmatchetRegisterAiAssistantPreferencesDockingTests(engine);
    SmatchetRegisterAiAssistantPreferencesEnterSendTests(engine);
    SmatchetRegisterAiAssistantPreferencesValidationBannerTests(engine);
    SmatchetRegisterAiAssistantPreferencesSaveDiscardTests(engine);
    SmatchetRegisterAiAssistantPreferencesTestConnectionTests(engine);
    SmatchetRegisterAiAssistantPreferencesVerifyOnSaveTests(engine);
#endif
#if defined(SMATCHET_WITH_AI) && defined(SMATCHET_WITH_WHISPER)
    SmatchetRegisterWhisperAiAssistantAutosendTests(engine);
#endif
}

#endif // SMATCHET_BUILD_UI_TESTS
