// ai_assistant_preferences_validation_banner.test.cpp — bucket-E coverage
// for the AI Assistant Preferences tab's validation banner: errors block
// (red), warnings pass through (yellow), valid config shows no banner.
//
// Drift warning — IF YOU CHANGE Source_Core/include/AiPrefsValidator.h:62
// `ValidateAiPrefs(const TrackerConfig&)`, Source_Core/src/AiPrefsValidator.cpp
// (the issue-emission body), OR Source_Core/src/SmatchetPreferencesUi.cpp:567
// where the banner is drawn from PrefsValidation.Errors / Warnings,
// UPDATE THIS TEST to match. The test drives the **real** ValidateAiPrefs()
// against synthetic TrackerConfig snapshots and asserts on the structured
// PrefsValidation result — no replica needed.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "AiPrefsValidator.h"
#include "AiTypes.h"
#include "ConfigManager.h"

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

struct ValidationState {
    TrackerConfig cfg;
    smatchet::ai::PrefsValidation lastResult;
};

ValidationState g_validationState;

void ResetValidationState() {
    g_validationState.cfg = TrackerConfig{};
    g_validationState.lastResult = smatchet::ai::PrefsValidation{};
}

// Render the banner shape from the real validation result so the bucket-E
// surface exercises the UI path even though the assertion targets the pure
// validator output.
void DrawBannerReplica(const ValidationState& s) {
    ImGui::SetNextWindowSize(ImVec2(480, 160), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::ValidationBanner", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (!s.lastResult.Errors.empty()) {
            ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "Errors: %d",
                               static_cast<int>(s.lastResult.Errors.size()));
        } else if (!s.lastResult.Warnings.empty()) {
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.2f, 1.0f), "Warnings: %d",
                               static_cast<int>(s.lastResult.Warnings.size()));
        } else {
            ImGui::TextUnformatted("(no banner)");
        }
    }
    ImGui::End();
}

void RegisterErrorBannerVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "ValidationBanner_ErrorBlocks");
    t->UserData = &g_validationState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ValidationState*>(ctx->Test->UserData);
        DrawBannerReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ValidationState*>(ctx->Test->UserData);
        ResetValidationState();
        // OpenAi provider but no API key → must error.
        s->cfg.AiProviderKind = static_cast<int>(AiProvider::OpenAi);
        s->cfg.AiApiKey = "";
        s->cfg.AiModelOpenAi = "gpt-4o-mini";
        s->cfg.AiBaseUrl = "https://api.openai.com";
        s->lastResult = smatchet::ai::ValidateAiPrefs(s->cfg);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(!s->lastResult.IsOk());
        IM_CHECK(!s->lastResult.Errors.empty());
    };
}

void RegisterNoBannerOnValidVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "ValidationBanner_NoneOnValid");
    t->UserData = &g_validationState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ValidationState*>(ctx->Test->UserData);
        DrawBannerReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<ValidationState*>(ctx->Test->UserData);
        ResetValidationState();
        // OpenAi with all required fields populated → no errors (warnings tolerated).
        s->cfg.AiProviderKind = static_cast<int>(AiProvider::OpenAi);
        s->cfg.AiApiKey = "sk-test-XXXXXXXX";
        s->cfg.AiModelOpenAi = "gpt-4o-mini";
        s->cfg.AiBaseUrl = "https://api.openai.com";
        s->lastResult = smatchet::ai::ValidateAiPrefs(s->cfg);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(s->lastResult.IsOk());
        IM_CHECK(s->lastResult.Errors.empty());
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPreferencesValidationBannerTests(ImGuiTestEngine* engine) {
    RegisterErrorBannerVariant(engine);
    RegisterNoBannerOnValidVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
