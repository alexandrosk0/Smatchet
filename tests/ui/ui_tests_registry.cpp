// Aggregates per-feature ImGui Test Engine test-registration entry points.
// Called once from UiTestScenario::OnStart() after the engine context has
// been created. New per-feature test files declare their `RegisterX(engine)`
// here and prepend a call below.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_engine.h"

extern "C" void SmatchetRegisterViewsColumnsReorderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterJiraDeterministicBackendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterLinearDeterministicBackendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterCallstackTooltipHoverTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterHelpMarkerKeyboardFocusTests(ImGuiTestEngine* engine);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
extern "C" void SmatchetRegisterMcpLuaFreshStateRaceTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAutomationReloadHooksRaceTests(ImGuiTestEngine* engine);
#endif
extern "C" void SmatchetRegisterSyncStallVisibleCueTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAnnotateBeforeClCueTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterToolbarAppendCacheCueTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAttachmentThumbnailLoadingCueTests(ImGuiTestEngine* engine);
// nightly-monkey-tester Layer 2 — random-input UI monkey (self-gated on SMATCHET_UI_MONKEY=1).
extern "C" void SmatchetRegisterUiMonkeyTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterDescriptionTooltipMarkdownRenderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterSpawnWarmupDeterministicGateTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAnnotatePrefsPersistFlowTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterDataDependentWindowsSmokeTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterNotificationCenterTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterFuncSizeWindowRenderSmokeTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterFuncSizePreferencesTabsTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterFuncSizeMainUiSmokeTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterFuncSizeGridRenderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterGridPaneWindowsTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterGridWheelRouteOwnershipTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterResetLayoutDockingTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterDurationInlineEditCommitTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterMobileViewsConfirmModalTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterMobileViewQuickSwitcherTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterViewsFieldSelectionTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterKeybindingsEditorRebindTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterCommandPaletteInlineTypingTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterCommandPaletteDeferredDispatchTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterOmnibarSearchApplyTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterOfflineConflictModalPanesTests(ImGuiTestEngine* engine);
#if defined(SMATCHET_WITH_AI)
extern "C" void SmatchetRegisterAiAssistantPanelDockSwapTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiChatClearConfirmTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantEnterSendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantInputPasteOverflowTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiPrefsAutosaveFlowTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesDockingTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesEnterSendTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesValidationBannerTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesSaveDiscardTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesTestConnectionTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantPreferencesVerifyOnSaveTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterAiAssistantModelChangeStripTests(ImGuiTestEngine* engine);
#endif
#if defined(SMATCHET_WITH_AI) && defined(SMATCHET_WITH_WHISPER)
extern "C" void SmatchetRegisterWhisperAiAssistantAutosendTests(ImGuiTestEngine* engine);
#endif

extern "C" void SmatchetRegisterAllUiTests(ImGuiTestEngine* engine) {
    SmatchetRegisterViewsColumnsReorderTests(engine);
    SmatchetRegisterJiraDeterministicBackendTests(engine);
    SmatchetRegisterLinearDeterministicBackendTests(engine);
    SmatchetRegisterCallstackTooltipHoverTests(engine);
    SmatchetRegisterHelpMarkerKeyboardFocusTests(engine);
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    SmatchetRegisterMcpLuaFreshStateRaceTests(engine);
    SmatchetRegisterAutomationReloadHooksRaceTests(engine);
#endif
    SmatchetRegisterSyncStallVisibleCueTests(engine);
    SmatchetRegisterAnnotateBeforeClCueTests(engine);
    SmatchetRegisterToolbarAppendCacheCueTests(engine);
    SmatchetRegisterAttachmentThumbnailLoadingCueTests(engine);
    SmatchetRegisterUiMonkeyTests(engine); // self-gated on SMATCHET_UI_MONKEY=1
    SmatchetRegisterDescriptionTooltipMarkdownRenderTests(engine);
    SmatchetRegisterSpawnWarmupDeterministicGateTests(engine);
    SmatchetRegisterAnnotatePrefsPersistFlowTests(engine);
    SmatchetRegisterDataDependentWindowsSmokeTests(engine);
    SmatchetRegisterNotificationCenterTests(engine);
    SmatchetRegisterFuncSizeWindowRenderSmokeTests(engine);
    SmatchetRegisterFuncSizePreferencesTabsTests(engine);
    SmatchetRegisterFuncSizeMainUiSmokeTests(engine);
    SmatchetRegisterFuncSizeGridRenderTests(engine);
    SmatchetRegisterGridPaneWindowsTests(engine);
    SmatchetRegisterGridWheelRouteOwnershipTests(engine);
    SmatchetRegisterResetLayoutDockingTests(engine);
    SmatchetRegisterDurationInlineEditCommitTests(engine);
    SmatchetRegisterMobileViewsConfirmModalTests(engine);
    SmatchetRegisterMobileViewQuickSwitcherTests(engine);
    SmatchetRegisterViewsFieldSelectionTests(engine);
    SmatchetRegisterKeybindingsEditorRebindTests(engine);
    SmatchetRegisterCommandPaletteInlineTypingTests(engine);
    SmatchetRegisterCommandPaletteDeferredDispatchTests(engine);
    SmatchetRegisterOmnibarSearchApplyTests(engine);
    SmatchetRegisterOfflineConflictModalPanesTests(engine);
#if defined(SMATCHET_WITH_AI)
    SmatchetRegisterAiAssistantPanelDockSwapTests(engine);
    SmatchetRegisterAiChatClearConfirmTests(engine);
    SmatchetRegisterAiAssistantEnterSendTests(engine);
    SmatchetRegisterAiAssistantInputPasteOverflowTests(engine);
    SmatchetRegisterAiPrefsAutosaveFlowTests(engine);
    SmatchetRegisterAiAssistantPreferencesDockingTests(engine);
    SmatchetRegisterAiAssistantPreferencesEnterSendTests(engine);
    SmatchetRegisterAiAssistantPreferencesValidationBannerTests(engine);
    SmatchetRegisterAiAssistantPreferencesSaveDiscardTests(engine);
    SmatchetRegisterAiAssistantPreferencesTestConnectionTests(engine);
    SmatchetRegisterAiAssistantPreferencesVerifyOnSaveTests(engine);
    SmatchetRegisterAiAssistantModelChangeStripTests(engine);
#endif
#if defined(SMATCHET_WITH_AI) && defined(SMATCHET_WITH_WHISPER)
    SmatchetRegisterWhisperAiAssistantAutosendTests(engine);
#endif
}

#endif // SMATCHET_BUILD_UI_TESTS
