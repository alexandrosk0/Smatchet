// Aggregates per-feature ImGui Test Engine test-registration entry points.
// Called once from UiTestScenario::OnStart() after the engine context has
// been created. New per-feature test files declare their `RegisterX(engine)`
// here and prepend a call below.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "imgui.h"
#include "imgui_te_engine.h"

extern "C" void SmatchetRegisterViewsColumnsReorderTests(ImGuiTestEngine* engine);
extern "C" void SmatchetRegisterCallstackTooltipHoverTests(ImGuiTestEngine* engine);

extern "C" void SmatchetRegisterAllUiTests(ImGuiTestEngine* engine) {
    SmatchetRegisterViewsColumnsReorderTests(engine);
    SmatchetRegisterCallstackTooltipHoverTests(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
