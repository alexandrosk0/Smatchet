// plan_doc_viewer_external_open.test.cpp — bucket-E coverage for opening an
// arbitrary markdown file in the Plan Docs viewer via
// smatchet::PlanDocViewerOpenExternalFile (the Core entry point behind both the
// OS drag-and-drop callback and the "Open..." dialog).
//
// The OS drop event itself (glfwSetDropCallback) cannot be synthesized by the
// ImGui Test Engine; MarkdownFileDropCallback in StandaloneBoot_detail.cpp is a
// thin extension filter over this entry point, and its registration runs on
// every bucket-E boot. So the API is what is exercised here.
//
// STRUCTURAL probe: under the harness the doc scanner finds no repo docs (the
// repo root is not the CWD at runtime — see data_dependent_windows_smoke.test.cpp
// case 3), so before this feature the picker combo could never appear. After
// PlanDocViewerOpenExternalFile the combined list holds the external file, the
// window must open + focus itself, and "##plan_doc_picker" must render — that
// combo appearing IS the proof the dropped entry populated the picker.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetPlanDocViewerUi.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — proven real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <ghc/filesystem.hpp>

#include <fstream>
#include <string>
#include <system_error>

// g_ui — the shared bag of UI-thread visibility flags (defined in SmatchetUI.cpp).
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
// Returns true if the predicate became true within the budget.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once a top-level window with `title` exists and is Active (Begin()
// returned true this frame, so its children were submitted).
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// Write a small markdown file under the OS temp dir. Returns the generic-form
// absolute path, or empty on failure (the test then SKIPs rather than asserting
// on an environment problem).
std::string WriteTempMarkdownFile() {
    std::error_code ec;
    ghc::filesystem::path dir = ghc::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::string();
    }
    const ghc::filesystem::path p = dir / "smatchet_uitest_external_open.md";
    std::ofstream out(p.generic_string().c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return std::string();
    }
    out << "# External doc\n\nOpened via PlanDocViewerOpenExternalFile.\n";
    out.close();
    return p.generic_string();
}

void RegisterExternalOpenPopulatesPicker(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "PlanDocViewerExternalOpen", "ExternalFile_OpensViewerAndPopulatesPicker");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo(
                "SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted under UiTestScenario");
            return;
        }

        const std::string mdPath = WriteTempMarkdownFile();
        if (mdPath.empty()) {
            ctx->LogInfo("SKIP: could not write a temp .md file for the external-open probe");
            return;
        }

        smatchet::PlanDocViewerOpenExternalFile(g_ui, mdPath);
        // The entry point must open + focus the window itself — that is the
        // whole point of the drop path (no menu interaction involved).
        IM_CHECK_NO_RET(g_ui.showPlanDocViewer);

        ctx->SetRef("Plan Docs");
        const bool visible = YieldUntil(ctx, [&] {
            g_ui.requestPlanDocViewerFocus = true; // re-arm: docked-tab focus can take frames
            return WindowIsLive("Plan Docs");
        });
        IM_CHECK_NO_RET(visible);

        if (visible) {
            // The harness CWD has no docs/ tree, so the scan lands empty; only
            // the external entry can put the picker on screen. Pre-feature this
            // branch rendered the "No plan docs found" empty state instead.
            const bool pickerAppeared = YieldUntil(ctx, [&] { return ctx->ItemExists("##plan_doc_picker"); });
            IM_CHECK_NO_RET(pickerAppeared);
        }

        g_ui.showPlanDocViewer = false;
        ctx->Yield();
        std::error_code ec;
        ghc::filesystem::remove(ghc::filesystem::path(mdPath), ec); // best-effort temp cleanup
    };
}

} // namespace

extern "C" void SmatchetRegisterPlanDocViewerExternalOpenTests(ImGuiTestEngine* engine) {
    RegisterExternalOpenPopulatesPicker(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
