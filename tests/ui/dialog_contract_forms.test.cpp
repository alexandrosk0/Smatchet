// dialog_contract_forms.test.cpp — bucket-E coverage for slice 3 of input-ux-w2-dialog-contract: the form
// and info dialogs on the shared dialog scope (Ui/SmatchetDialogScope.h) and focus return after a dialog or
// the command palette closes (audit #24, #89, #91, #120, #121).
//
// What it locks in:
//   - Caret return: a text field that had the caret when a modal opened (here opened from code, as the
//     update check does while the user types) has it back once the modal closes, with its text intact and
//     the caret where it was, even after the modal's own field took ImGui's one text-edit state (the next
//     key adds to the text instead of replacing it). A field that did not have the caret is not given it.
//     This answers the plan's open question: the ActivateItemByID request from ServiceDialogFocusReturn is
//     not raced by ImGui's own restore.
//   - The same for the command palette, closed with Esc. A command that ran leaves the caret alone, and so
//     does closing the palette after the user clicked into another field.
//   - About: the keyboard starts on Close, Esc closes.
//   - Update Available: the keyboard starts on Later, so Enter installs and skips nothing; Esc is Later.
//   - Customize Toolbar: Add command puts the caret in the command filter; with an edit made Esc only
//     leaves the field and never discards the edit (Cancel does); untouched, Esc closes and Ctrl+Enter saves.
//
// The caret-return harness draws its own host window with a field and its own modal through BeginDialog,
// so it does not depend on which app window holds a text field; ServiceDialogFocusReturn runs from the
// app's SmatchetUI::Draw every frame, as in production.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandPaletteUi.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h" // SmatchetActiveUiTestAppController
#include "SmatchetUiSession.h"
#include "Ui/SmatchetDialogScope.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <nlohmann/json.hpp> // Command.h is json_fwd; this TU constructs nlohmann::json::object()

#include <cstring>
#include <string>
#include <utility>

extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 120) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

bool AppBooted(ImGuiTestContext* ctx) {
    if (SmatchetActiveUiTestAppController() == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr - app not booted");
        return false;
    }
    return true;
}

// The text field holding the caret, 0 for none.
ImGuiID ActiveTextField() {
    const ImGuiContext& g = *GImGui;
    return (g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId) ? g.ActiveId : 0;
}

// ---------------------------------------------------------------------------------------------------------
// Caret return harness: a host window with a field and a button, a modal opened from code, a real palette.
// ---------------------------------------------------------------------------------------------------------

const char* kHostWindow = "CaretReturnHost";
const char* kHostWindowRef = "//CaretReturnHost";
const char* kModalTitle = "Caret return modal";
const char* kPaletteWindow = "##cmdpalette";
const char* kProbeCmd = "buckete.dialog.zqxcaretreturn_probe";
const char* kProbeQuery = "zqxcaretreturn";

struct CaretHarness {
    bool armed = false;
    bool openModal = false; // latched by the test, consumed on the next GuiFunc frame
    char text[64] = {};
    char otherText[64] = {}; // a second field: the user moved on to it while the palette was open
    char modalText[64] = {}; // the modal's own field, which takes ImGui's one text-edit state
    smatchet::ui::DialogScopeState modal;
    smatchet::cmd::CommandPaletteUi palette;
    AppController* app = nullptr;
    int dispatches = 0;
};

CaretHarness g_caret;

void DrawCaretHarness(ImGuiTestContext* /*ctx*/) {
    if (!g_caret.armed) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(40.0f, 60.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 120.0f), ImGuiCond_Always);
    ImGui::Begin(kHostWindow, nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::InputText("##field", g_caret.text, sizeof(g_caret.text));
    ImGui::InputText("##otherfield", g_caret.otherText, sizeof(g_caret.otherText));
    ImGui::Button("Other");
    if (g_caret.openModal) {
        g_caret.openModal = false;
        ImGui::OpenPopup(kModalTitle);
    }
    if (smatchet::ui::BeginDialog(g_caret.modal, kModalTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (smatchet::ui::DialogReadKeys(g_caret.modal, true).Esc == smatchet::dialogkeys::EscapeAction::Cancel) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextUnformatted("A modal over the field");
        ImGui::InputText("##modalfield", g_caret.modalText, sizeof(g_caret.modalText));
        smatchet::ui::DialogFocusHere(g_caret.modal);
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
        }
        smatchet::ui::EndDialog(g_caret.modal);
    }
    ImGui::End();
    if (g_caret.app != nullptr) {
        g_caret.palette.Draw(*g_caret.app);
    }
}

bool ArmCaret(ImGuiTestContext* ctx) {
    if (!AppBooted(ctx)) {
        return false;
    }
    AppController* app = SmatchetActiveUiTestAppController();
    if (!app->Commands().HasExact(kProbeCmd)) {
        smatchet::cmd::Command c;
        c.Name = kProbeCmd;
        c.Category = "buckete";
        c.Summary = "Synthetic caret-return probe";
        c.Handler = [](const nlohmann::json&, const smatchet::cmd::CommandContext&) {
            ++g_caret.dispatches;
            return smatchet::cmd::CommandResult::Success(nlohmann::json::object());
        };
        app->Commands().Register(std::move(c));
    }
    g_caret.app = app;
    g_caret.dispatches = 0;
    g_caret.openModal = false;
    std::memset(g_caret.text, 0, sizeof(g_caret.text));
    std::memset(g_caret.otherText, 0, sizeof(g_caret.otherText));
    std::memset(g_caret.modalText, 0, sizeof(g_caret.modalText));
    g_caret.armed = true;
    g_ui.appUpdateModalOpen = false; // an update prompt from the boot would sit over the harness
    ctx->Yield(2);
    return true;
}

void DisarmCaret(ImGuiTestContext* ctx) {
    g_caret.palette.Close();
    ctx->Yield();
    g_caret.armed = false;
    g_caret.app = nullptr;
    ctx->Yield(2);
}

// The caret in the host field with "abc" typed; returns the field's id, 0 when the field did not take the
// caret (the caller checks it: the test-engine checks return void).
ImGuiID TypeInHostField(ImGuiTestContext* ctx) {
    ctx->SetRef(kHostWindowRef);
    ctx->ItemInput("##field");
    ctx->KeyChars("abc");
    ctx->Yield(2);
    const ImGuiID field = ctx->GetID("##field");
    return ActiveTextField() == field ? field : 0;
}

void CheckModalGivesCaretBack(ImGuiTestContext* ctx) {
    const ImGuiID field = TypeInHostField(ctx);
    IM_CHECK(field != 0);

    g_caret.openModal = true; // opened from code, as the update check does mid-typing
    IM_CHECK(YieldUntil(ctx, [] { return WindowIsLive(kModalTitle); }, 30));
    ctx->Yield(3);
    IM_CHECK_NE(ActiveTextField(), field); // the modal took the keyboard

    // Typing in the modal's own field takes ImGui's one text-edit state away from the host field.
    ctx->SetRef(kModalTitle);
    ctx->ItemInput("##modalfield");
    ctx->KeyChars("zz");
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30));
    IM_CHECK(YieldUntil(ctx, [&] { return ActiveTextField() == field; }, 10));
    ctx->Yield(2);
    // The caret is back at the end, with nothing selected: the next key adds to the text, not over it.
    ctx->KeyChars("d");
    ctx->Yield(2);
    IM_CHECK_STR_EQ(g_caret.text, "abcd");

    // Control: with the keyboard on a button rather than in the field, closing gives the field nothing.
    ctx->SetRef(kHostWindowRef);
    ctx->ItemClick("Other");
    ctx->Yield(2);
    IM_CHECK_EQ(ActiveTextField(), static_cast<ImGuiID>(0));
    g_caret.openModal = true;
    IM_CHECK(YieldUntil(ctx, [] { return WindowIsLive(kModalTitle); }, 30));
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30));
    ctx->Yield(6);
    IM_CHECK_EQ(ActiveTextField(), static_cast<ImGuiID>(0));
}

void CheckPaletteGivesCaretBack(ImGuiTestContext* ctx) {
    const ImGuiID field = TypeInHostField(ctx);
    IM_CHECK(field != 0);

    g_caret.palette.Open();
    ctx->Yield(3);
    IM_CHECK(g_caret.palette.IsOpen());
    IM_CHECK_NE(ActiveTextField(), field); // the palette's own filter has the caret
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !g_caret.palette.IsOpen(); }, 10));
    IM_CHECK(YieldUntil(ctx, [&] { return ActiveTextField() == field; }, 10));
    ctx->Yield(2);
    ctx->KeyChars("d"); // the palette's filter had the text-edit state: the text must not be selected
    ctx->Yield(2);
    IM_CHECK_STR_EQ(g_caret.text, "abcd");

    // The user clicked into another field while the palette stayed open: closing it leaves them there.
    g_caret.palette.Open();
    ctx->Yield(3);
    IM_CHECK(g_caret.palette.IsOpen());
    ctx->SetRef(kHostWindowRef);
    ctx->ItemInput("##otherfield");
    ctx->KeyChars("q");
    ctx->Yield(2);
    const ImGuiID other = ctx->GetID("##otherfield");
    IM_CHECK_EQ(ActiveTextField(), other);
    g_caret.palette.Close(); // as the Ctrl+Shift+P toggle does
    ctx->Yield(6);
    IM_CHECK_EQ(ActiveTextField(), other);
    IM_CHECK_STR_EQ(g_caret.text, "abcd");
    IM_CHECK_STR_EQ(g_caret.otherText, "q");

    // A command that ran leaves the caret where the command put it: nothing is handed back.
    g_caret.palette.Open();
    ctx->Yield(2);
    ctx->SetRef(kPaletteWindow);
    ctx->ItemInput("##cmdq");
    ctx->KeyChars(kProbeQuery);
    ctx->Yield(2);
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(YieldUntil(ctx, [] { return !g_caret.palette.IsOpen(); }, 10));
    ctx->Yield(6); // the palette may run the command a frame after it closes
    IM_CHECK_EQ(g_caret.dispatches, 1);
    IM_CHECK_NE(ActiveTextField(), field);
}

// ---------------------------------------------------------------------------------------------------------
// The app's own dialogs.
// ---------------------------------------------------------------------------------------------------------

const char* kAboutTitle = "About Smatchet";
const char* kUpdateTitle = "Update Available###AppUpdateAvailable";
const char* kToolbarTitle = "Customize Toolbar###SmatchetToolbarEditor";

void CheckAboutKeys(ImGuiTestContext* ctx) {
    g_ui.appUpdateModalOpen = false;
    g_ui.showAbout = true;
    g_ui.aboutOpenLatch = true;
    IM_CHECK(YieldUntil(ctx, [] { return WindowIsLive(kAboutTitle); }, 60));
    ctx->Yield(3);
    ctx->SetRef(kAboutTitle);
    IM_CHECK_EQ(GImGui->NavId, ctx->GetID("Close"));
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kAboutTitle); }, 30));
    IM_CHECK(!g_ui.showAbout);
}

void OpenUpdatePrompt(ImGuiTestContext* ctx) {
    g_ui.appUpdateInfo = AppUpdateInfo();
    g_ui.appUpdateInfo.Ok = true;
    g_ui.appUpdateInfo.UpdateAvailable = true;
    g_ui.appUpdateInfo.CurrentVersion = "1.0.0";
    g_ui.appUpdateInfo.LatestVersion = "9.9.9-buckete";
    g_ui.appUpdateActionStatus.clear();
    g_ui.installerDownloadInFlight = false;
    g_ui.appUpdateModalOpen = true;
    IM_CHECK(YieldUntil(ctx, [] { return WindowIsLive(kUpdateTitle); }, 60));
    ctx->Yield(3);
}

void CheckUpdatePromptKeys(ImGuiTestContext* ctx) {
    const std::string skipBefore = g_ui.cfg.UpdateSkipVersion;

    // Enter straight away is Later: nothing downloads and the version is not skipped.
    OpenUpdatePrompt(ctx);
    ctx->SetRef(kUpdateTitle);
    IM_CHECK_EQ(GImGui->NavId, ctx->GetID("Later"));
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kUpdateTitle); }, 30));
    IM_CHECK(!g_ui.appUpdateModalOpen);
    IM_CHECK(!g_ui.installerDownloadInFlight);
    IM_CHECK_STR_EQ(g_ui.cfg.UpdateSkipVersion.c_str(), skipBefore.c_str());

    // Esc is Later too.
    OpenUpdatePrompt(ctx);
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kUpdateTitle); }, 30));
    IM_CHECK(!g_ui.appUpdateModalOpen);
    IM_CHECK(!g_ui.installerDownloadInFlight);
    IM_CHECK_STR_EQ(g_ui.cfg.UpdateSkipVersion.c_str(), skipBefore.c_str());
}

// The desktop layout, which has the toolbar, is up. False is a skip, logged.
bool DesktopToolbarShown(ImGuiTestContext* ctx) {
    if (!YieldUntil(ctx, [] { return WindowIsLive("##MainMenuBar") && WindowIsLive("##SmatchetToolbar"); }, 120)) {
        ctx->LogInfo("SKIP: no toolbar in this layout (Zen or mobile mode)");
        return false;
    }
    return true;
}

bool OpenToolbarEditor(ImGuiTestContext* ctx) {
    g_ui.appUpdateModalOpen = false;
    // The toolbar's own context menu: the View menu does not stay open across the engine's menu navigation.
    ctx->MouseMove("//##SmatchetToolbar");
    ctx->MouseClick(ImGuiMouseButton_Right);
    ctx->ItemClick("//$FOCUSED/Customize Toolbar...");
    const bool open = YieldUntil(ctx, [] { return WindowIsLive(kToolbarTitle); }, 60);
    ctx->Yield(3);
    return open;
}

// A field of the toolbar editor has the caret.
bool CaretInToolbarEditor() {
    const ImGuiWindow* editor = ImGui::FindWindowByName(kToolbarTitle);
    return editor != nullptr && ActiveTextField() != 0 && GImGui->ActiveIdWindow != nullptr &&
           GImGui->ActiveIdWindow->RootWindow == editor;
}

void CheckToolbarEditorKeys(ImGuiTestContext* ctx) {
    if (!DesktopToolbarShown(ctx)) {
        return;
    }
    const std::size_t buttonsBefore = g_ui.cfg.Toolbar.Buttons.size();
    if (!OpenToolbarEditor(ctx)) {
        IM_CHECK(WindowIsLive(kToolbarTitle)); // fails with the window title in the message
        return;
    }
    // On open the keyboard is in the editor, and no field has the caret yet (the button list has it).
    const ImGuiWindow* editor = ImGui::FindWindowByName(kToolbarTitle);
    IM_CHECK(GImGui->NavWindow != nullptr && GImGui->NavWindow->RootWindow == editor);
    IM_CHECK(!CaretInToolbarEditor());

    // Add command puts the caret in the command filter (#120).
    ctx->SetRef(kToolbarTitle);
    ctx->ItemClick("**/Add command");
    IM_CHECK(YieldUntil(ctx, [] { return CaretInToolbarEditor(); }, 10));
    const ImGuiInputTextState& state = GImGui->InputTextState;
    IM_CHECK(state.ID != 0);

    // An edit was made, so Esc only leaves the field, and a second Esc does not throw the edit away; Cancel
    // does, and nothing is saved.
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK(WindowIsLive(kToolbarTitle));
    IM_CHECK(!CaretInToolbarEditor());
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(4);
    IM_CHECK(WindowIsLive(kToolbarTitle));
    ctx->SetRef(kToolbarTitle);
    ctx->ItemClick("**/Cancel");
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kToolbarTitle); }, 30));
    IM_CHECK_EQ(g_ui.cfg.Toolbar.Buttons.size(), buttonsBefore);

    // Untouched, Esc closes at once.
    if (!OpenToolbarEditor(ctx)) {
        IM_CHECK(WindowIsLive(kToolbarTitle)); // fails with the window title in the message
        return;
    }
    ctx->KeyPress(ImGuiKey_Escape);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kToolbarTitle); }, 30));

    // Untouched, Ctrl+Enter is Save: the editor closes (saving the same list back).
    if (!OpenToolbarEditor(ctx)) {
        IM_CHECK(WindowIsLive(kToolbarTitle)); // fails with the window title in the message
        return;
    }
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
    IM_CHECK(YieldUntil(ctx, [] { return !WindowIsLive(kToolbarTitle); }, 30));
    IM_CHECK_EQ(g_ui.cfg.Toolbar.Buttons.size(), buttonsBefore);
}

} // namespace

extern "C" void SmatchetRegisterDialogContractFormTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DialogContract", "CaretReturnsToFieldAfterModal");
    t->GuiFunc = DrawCaretHarness;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (ArmCaret(ctx)) {
            CheckModalGivesCaretBack(ctx);
            DisarmCaret(ctx);
        }
    };

    t = IM_REGISTER_TEST(engine, "DialogContract", "CaretReturnsToFieldAfterPaletteEsc");
    t->GuiFunc = DrawCaretHarness;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (ArmCaret(ctx)) {
            CheckPaletteGivesCaretBack(ctx);
            DisarmCaret(ctx);
        }
    };

    t = IM_REGISTER_TEST(engine, "DialogContract", "AboutStartsOnCloseAndEscCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (AppBooted(ctx)) {
            CheckAboutKeys(ctx);
        }
    };

    t = IM_REGISTER_TEST(engine, "DialogContract", "UpdatePromptStartsOnLater");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (AppBooted(ctx)) {
            CheckUpdatePromptKeys(ctx);
        }
    };

    t = IM_REGISTER_TEST(engine, "DialogContract", "ToolbarEditorKeys");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (AppBooted(ctx)) {
            CheckToolbarEditorKeys(ctx);
        }
    };
}

#endif // SMATCHET_BUILD_UI_TESTS
