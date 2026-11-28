// offline_conflict_modal_panes.test.cpp — bucket-E (ImGui Test Engine) coverage
// for the per-kind offline-conflict resolution modal (DrawOfflineConflictModal,
// SmatchetOfflineQueueUi.cpp, ADR-0016).
//
// The modal dispatches on the parsed `kind` of conflict_context_json into one of
// four panes. This test covers the two scalar-vs-rich-text alternatives the legacy
// `text` pane does NOT exercise:
//   - kind:"scalar"     -> DrawConflictPaneScalar    (Use Mine / Use Theirs / "Save value")
//   - kind:"unverified" -> DrawConflictPaneUnverified ("Force Mine" / Discard my edit)
//
// BEHAVIORAL assertion (no screenshot / golden baseline): the modal is opened by
// seeding the three g_ui trigger fields the production right-click / double-click
// handlers set (showConflictResolveModal + conflictContextJson + conflictResolveDbId),
// then we assert the popup window goes live AND the per-kind branch's signature
// button is reachable. "Save value" exists ONLY in the scalar pane; "Force Mine"
// exists ONLY in the unverified pane — so each button's presence proves the
// correct per-kind branch was reached and rendered without an in-frame ImGui
// assertion (the engine traps any Begin/End or ID imbalance).
//
// FIXTURE + LOCAL-CACHE GATED (mirrors data_dependent_windows_smoke.test.cpp case 8):
// DrawOfflineConflictModal is a file-local static reachable only through
// DrawUnifiedOfflineQueuesPanel, which (a) is drawn inline by the docked
// "Smatchet - Active Project" host pane — live only once tickets load (fixture) —
// and (b) early-returns when the offline queue is empty (total==0). So we sync the
// deterministic Jira fixture to make the host pane live, then enqueue one real
// pending field edit (QueueFieldEditOffline — a pure local-SQLite write, needs the
// SMATCHET_UITEST_WITH_LOCAL_CACHE=1 cache) so total>0 forces the panel body — and
// thus DrawOfflineConflictModal — to run every frame. When either gate is absent the
// test SKIPS with a log rather than asserting on an unreachable modal.
//
// KEYBOARD cases (same host, same gates): the caret starts in the resolved field, Enter
// (value) / Ctrl+Enter (text, once the markers are gone) save, Esc never throws typed
// text away, and Enter on a Conflict row opens the resolver. The save cases open the
// resolver on an id that is not in the queue: the save is then turned down, which shows
// the key reached the save path without sending anything to the tracker.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "FakeNetworkSwitch.h" // hold the fixture tracker down so a resolved edit is not replayed mid-case
#include "SmatchetUiSession.h"
#include "Tracker/TrackerFieldValueParseExplain.h"
#include "Ui/SmatchetGridUiSupport.h"         // SmatchetOfflineConflictUnsavedHintShownForTest
#include "Ui/SmatchetInlineError.h"           // InlineErrorKindForTests — the parser's reason is on screen
#include "Ui/SmatchetOfflineQueueActionsUi.h" // SmatchetOfflineConflictDraft*ForTest
#include "Ui/SmatchetSubmitKeys.h"            // KeyHintDrawnForTest
#include "Ui/SmatchetToast.h"                 // the "Edit discarded" Undo toast (#116)
#include "UiTestWriteScope.h" // BucketE::UiTestWriteScope — disable fresh-install read-only so the write lands

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdint>
#include <cstdlib> // getenv — fixture / local-cache env gates
#include <cstring>
#include <memory>
#include <string>

// g_ui — the shared bag of UI-thread visibility flags / modal trigger state.
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once a top-level window with `title` exists and is Active this frame.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// True if the named env var is set (cross-platform getenv; MSVC C4996 quieted).
bool EnvSet(const char* name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const bool set = std::getenv(name) != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

// Open the conflict modal for a given kind by reproducing the production trigger
// state (DrawOfflineRowStateCell double-click / context-menu "Resolve merge
// conflict..."), then wait for the popup window to go live. The panel's
// DrawOfflineConflictModal calls OpenPopup("ResolveMergeConflict") whenever
// showConflictResolveModal is true, so this drives the same path the user does.
bool OpenConflictModal(ImGuiTestContext* ctx, const char* contextJson, std::int64_t dbId) {
    g_ui.conflictResolveBuf.clear();
    g_ui.conflictResolveDbId = dbId;
    g_ui.conflictContextJson = contextJson;
    g_ui.showConflictResolveModal = true;
    return YieldUntil(ctx, [] { return WindowIsLive("ResolveMergeConflict"); });
}

// Cancel out of the live modal without invoking ResolveFieldEditConflict (which
// would need a backend replay). Every pane submits a "Cancel" Button that calls
// ImGui::CloseCurrentPopup(); click it through the popup scope, then wait for the
// window to drop. Falls back to clearing the trigger state if the click misses.
void CancelConflictModal(ImGuiTestContext* ctx) {
    ctx->SetRef("ResolveMergeConflict");
    if (ctx->ItemExists("Cancel")) {
        ctx->ItemClick("Cancel");
    }
    g_ui.showConflictResolveModal = false;
    g_ui.conflictResolveDbId = 0;
    g_ui.conflictContextJson.clear();
    g_ui.conflictResolveBuf.clear();
    YieldUntil(ctx, [] { return !WindowIsLive("ResolveMergeConflict"); });
}

const char* const kHostTitle = "Smatchet - Active Project";
const char* const kModalTitle = "ResolveMergeConflict";

// Makes the host pane live. Returns the app, or nullptr (logged) when a gate is missing
// or the pane never came up.
AppController* EnterConflictHost(ImGuiTestContext* ctx) {
    if (!EnvSet("SMATCHET_TEST_JIRA_BACKEND_FIXTURE")) {
        ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — the docked Active Project "
                     "host pane never goes live, so DrawUnifiedOfflineQueuesPanel (and the conflict "
                     "modal it owns) is unreachable. Same gate as data_dependent case 8.");
        return nullptr;
    }
    if (!EnvSet("SMATCHET_UITEST_WITH_LOCAL_CACHE")) {
        ctx->LogInfo("SKIP: SMATCHET_UITEST_WITH_LOCAL_CACHE not set — no live local cache, so "
                     "QueueFieldEditOffline cannot persist a pending edit and the panel stays on its "
                     "total==0 early-return path (no modal call site).");
        return nullptr;
    }

    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return nullptr;
    }

    // Make the host pane live: sync the fixture so tickets load + the docked
    // "Smatchet - Active Project" window activates (mirrors data_dependent case 8).
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsSyncPendingOrActive(); });
    IM_CHECK_NO_RET(syncDone);
    IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

    ctx->SetRef(kHostTitle);
    const bool hostLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kHostTitle);
    });
    IM_CHECK_NO_RET(hostLive);
    return hostLive ? app : nullptr;
}

// Queues one real pending field edit so total>0 forces the panel body (and the modal
// call site) to render. A pure local-SQLite write — no backend round-trip. The caller
// holds a BucketE::UiTestWriteScope. Returns the edit id, or 0 (checked and logged).
std::int64_t QueueConflictHostEdit(ImGuiTestContext* ctx, AppController* app) {
    std::string queueErr;
    const std::int64_t editId =
        app->QueueFieldEditOffline("SMAT-1", "summary", "{\"summary\":\"offline conflict modal pane test\"}", queueErr,
                                   std::string(), std::string("mine-original"), true);
    if (editId <= 0) {
        ctx->LogError("QueueFieldEditOffline returned %lld (err='%s') with read-only disabled + live "
                      "cache — the modal call site is unreachable; failing rather than faking green.",
                      static_cast<long long>(editId), queueErr.c_str());
    }
    IM_CHECK_NO_RET(editId > 0);
    IM_CHECK_NO_RET(!app->GetPendingFieldEdits().empty());
    if (editId <= 0) {
        return 0;
    }

    // Keep the host pane the active dock tab while the modal drives.
    const bool stillLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kHostTitle);
    });
    IM_CHECK_NO_RET(stillLive);
    return editId;
}

// Drops the synthetic queue row so it never leaks into a later test, closing the
// resolver first when a case left it open.
void LeaveConflictHost(ImGuiTestContext* ctx, AppController* app, std::int64_t editId) {
    if (WindowIsLive(kModalTitle)) {
        CancelConflictModal(ctx);
    }
    // That Cancel keeps typed text like the user's does: nothing kept outlives the case.
    SmatchetOfflineConflictDraftsClearForTest();
    app->DeletePendingFieldEdits({editId});
    g_ui.requestActiveProjectFocus = false;
    ctx->Yield();
}

void RunPerKindModalTest(ImGuiTestContext* ctx) {
    AppController* app = EnterConflictHost(ctx);
    if (app == nullptr) {
        return;
    }

    // The harness boots a fresh profile in the isolated SMATCHET_USER_DATA dir,
    // and ConfigManager defaults fresh installs to read-only (no config file →
    // ReadOnlyMode=true), which would reject the queue write below. The RAII
    // scope flips read-only=false for the duration of the test and restores it on
    // teardown. We hard-flip (not skip) because with both env gates satisfied the
    // queue write MUST land — a silent skip here would hide a real regression in
    // the offline path. See tests/ui/_helpers/UiTestWriteScope.h.
    BucketE::UiTestWriteScope writeScope;
    const std::int64_t editId = QueueConflictHostEdit(ctx, app);
    if (editId <= 0) {
        // ~UiTestWriteScope restores the prior ReadOnlyMode on return.
        return;
    }

    // ---- Pane 1: kind:"scalar" -> DrawConflictPaneScalar -------------------
    // Signature widget: "Save value" Button (exists in NO other pane: text has
    // "Save resolved", unverified has none, unknown has none).
    const bool scalarOpened = OpenConflictModal(ctx, "{\"kind\":\"scalar\",\"mine\":\"5\",\"theirs\":\"8\"}", editId);
    IM_CHECK_NO_RET(scalarOpened);
    if (scalarOpened) {
        ctx->SetRef("ResolveMergeConflict");
        const bool scalarBtn = ctx->ItemExists("Save value");
        // Negative cross-check: the unverified-only button must be absent in the
        // scalar pane, proving the dispatch is exclusive (not a both-rendered bug).
        const bool forceMineAbsent = !ctx->ItemExists("Force Mine");
        IM_CHECK_NO_RET(scalarBtn);
        IM_CHECK_NO_RET(forceMineAbsent);
    }
    CancelConflictModal(ctx);

    // ---- Pane 2: kind:"unverified" -> DrawConflictPaneUnverified -----------
    // Signature widget: "Force Mine" Button (exists in NO other pane).
    const bool unverifiedOpened =
        OpenConflictModal(ctx, "{\"kind\":\"unverified\",\"mine\":\"mine-original\"}", editId);
    IM_CHECK_NO_RET(unverifiedOpened);
    if (unverifiedOpened) {
        ctx->SetRef("ResolveMergeConflict");
        const bool forceMineBtn = ctx->ItemExists("Force Mine");
        // Negative cross-check: the scalar-only button must be absent here.
        const bool saveValueAbsent = !ctx->ItemExists("Save value");
        IM_CHECK_NO_RET(forceMineBtn);
        IM_CHECK_NO_RET(saveValueAbsent);
    }
    CancelConflictModal(ctx);

    // (~UiTestWriteScope restores the prior ReadOnlyMode at end of scope.)
    LeaveConflictHost(ctx, app, editId);
}

// ---- Keyboard cases ----------------------------------------------------------

const char* const kScalarJson = "{\"kind\":\"scalar\",\"mine\":\"5\",\"theirs\":\"8\"}";
const char* const kScalarEstimateJson =
    "{\"kind\":\"scalar\",\"mine\":\"3600\",\"theirs\":\"7200\",\"fieldId\":\"timeoriginalestimate\"}";
const char* const kTextJson = "{\"kind\":\"text\",\"mine\":\"a\",\"theirs\":\"b\"}";
const char* const kScalarField = "##SCResolved";
const char* const kTextField = "##CRResolved";
// Added to a real queue id to get one that is not in the queue: a save on it is turned down.
const std::int64_t kNotQueuedOffset = 1000000;

struct KeyboardCase {
    AppController* App = nullptr;
    std::int64_t EditId = 0;
};
typedef void (*KeyboardCaseBody)(ImGuiTestContext*, const KeyboardCase&);

// Host set-up and tear-down around one keyboard case.
void RunKeyboardCase(ImGuiTestContext* ctx, KeyboardCaseBody body) {
    KeyboardCase c;
    c.App = EnterConflictHost(ctx);
    if (c.App == nullptr) {
        return;
    }
    BucketE::UiTestWriteScope writeScope;
    c.EditId = QueueConflictHostEdit(ctx, c.App);
    if (c.EditId <= 0) {
        return;
    }
    // A case that cancels around typed text leaves that text kept for its queue id, and the next
    // case can be given the same id: each case starts with nothing kept.
    SmatchetOfflineConflictDraftsClearForTest();
    body(ctx, c);
    LeaveConflictHost(ctx, c.App, c.EditId);
}

std::string ResolvedText() {
    return g_ui.conflictResolveBuf.empty() ? std::string() : std::string(g_ui.conflictResolveBuf.data());
}

// Opens the resolver and waits for the caret to land in `field`. False (checked) when it does not.
bool OpenWithCaretIn(ImGuiTestContext* ctx, const char* contextJson, std::int64_t dbId, const char* field) {
    const bool opened = OpenConflictModal(ctx, contextJson, dbId);
    IM_CHECK_NO_RET(opened);
    if (!opened) {
        return false;
    }
    ctx->SetRef(kModalTitle);
    const ImGuiID fieldId = ctx->GetID(field);
    const bool caret = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caret);
    // A field takes its own shortcuts (Ctrl+A among them) from the second frame after the caret
    // enters: ImGui routes a shortcut to its owner one frame after the owner asks for it.
    ctx->Yield(2);
    return caret;
}

void CaseScalarCaretInFieldOnOpen(ImGuiTestContext* ctx, const KeyboardCase& c) {
    OpenWithCaretIn(ctx, kScalarJson, c.EditId, kScalarField);
}

void CaseTextCaretInFieldOnOpen(ImGuiTestContext* ctx, const KeyboardCase& c) {
    OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField);
}

// Enter in the value field runs the save. The id is not in the queue, so the save is
// turned down: the message shows, the resolver stays, and the caret returns to the field
// with the typed value still in it.
void CaseScalarEnterSaves(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kScalarJson, c.EditId + kNotQueuedOffset, kScalarField)) {
        return;
    }
    ctx->KeyCharsReplace("6");
    IM_CHECK_NO_RET(g_ui.conflictResolveError.empty());
    ctx->KeyPress(ImGuiKey_Enter);
    const bool saveRan = YieldUntil(ctx, [] { return !g_ui.conflictResolveError.empty(); }, 30);
    IM_CHECK_NO_RET(saveRan);
    IM_CHECK_NO_RET(WindowIsLive(kModalTitle));
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), "6");
    const ImGuiID fieldId = ctx->GetID(kScalarField);
    const bool caretBack = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caretBack);
}

// The queue service writes the field id into the conflict context, so an estimate field reads the typed value
// as a duration: text that does not read shows the parser's reason, text that reads clears it, and a field with
// no known kind has nothing to validate.
void CaseScalarEstimateFieldValidatesAsTyped(ImGuiTestContext* ctx, const KeyboardCase& c) {
    smatchet::ui::ResetInlineErrorForTests();
    if (!OpenWithCaretIn(ctx, kScalarEstimateJson, c.EditId + kNotQueuedOffset, kScalarField)) {
        return;
    }
    ctx->KeyCharsReplace("xyz");
    ctx->Yield(3);
    IM_CHECK_NO_RET(smatchet::ui::InlineErrorKindForTests() == "error");
    IM_CHECK_NO_RET(!smatchet::ui::InlineErrorTextForTests().empty());

    ctx->KeyCharsReplace("1h 30m");
    ctx->Yield(3);
    IM_CHECK_NO_RET(smatchet::ui::InlineErrorKindForTests() != "error");

    CancelConflictModal(ctx);
    smatchet::ui::ResetInlineErrorForTests();
    if (!OpenWithCaretIn(ctx, kScalarJson, c.EditId + kNotQueuedOffset, kScalarField)) {
        return;
    }
    ctx->KeyCharsReplace("xyz");
    ctx->Yield(3);
    IM_CHECK_NO_RET(smatchet::ui::InlineErrorKindForTests() != "error");
}

// A Jira sprint is a custom field whose id does not say "sprint": the resolver takes the kind the queue service
// recorded in the context, and falls back to the field id for a context written without one (#2513).
const char* const kScalarSprintJson = "{\"kind\":\"scalar\",\"mine\":\"42\",\"theirs\":\"Sprint 43\","
                                      "\"fieldId\":\"customfield_10020\",\"fieldKind\":\"sprint\"}";
const char* const kScalarSprintByIdJson =
    "{\"kind\":\"scalar\",\"mine\":\"42\",\"theirs\":\"Sprint 43\",\"fieldId\":\"customfield_sprint\"}";

// Types a value into the scalar resolver on an id that is not queued, saves with Enter, and returns the
// "turned down" text. Empty (checked) when the resolver did not open or the save did not run.
std::string TurnedDownScalarSave(ImGuiTestContext* ctx, const char* contextJson, std::int64_t dbId) {
    if (!OpenWithCaretIn(ctx, contextJson, dbId, kScalarField)) {
        return std::string();
    }
    ctx->KeyCharsReplace("Sprint 99");
    ctx->KeyPress(ImGuiKey_Enter);
    const bool saveRan = YieldUntil(ctx, [] { return !g_ui.conflictResolveError.empty(); }, 30);
    IM_CHECK_NO_RET(saveRan);
    return g_ui.conflictResolveError;
}

void CaseScalarSprintKindFromContext(ImGuiTestContext* ctx, const KeyboardCase& c) {
    TrackerParseExplain::ParseMessage sprintMsg;
    sprintMsg.Id = TrackerParseExplain::ParseMessageId::ConflictValueRejectedSprint;
    const std::string sprintText = TrackerParseExplain::DescribeParseMessage(sprintMsg);
    TrackerParseExplain::ParseMessage genericMsg;
    genericMsg.Id = TrackerParseExplain::ParseMessageId::ConflictValueRejectedGeneric;
    const std::string genericText = TrackerParseExplain::DescribeParseMessage(genericMsg);

    const std::string byKind = TurnedDownScalarSave(ctx, kScalarSprintJson, c.EditId + kNotQueuedOffset);
    IM_CHECK_NO_RET(byKind.find(sprintText) == 0);
    IM_CHECK_NO_RET(byKind.find(genericText) == std::string::npos);

    CancelConflictModal(ctx);
    SmatchetOfflineConflictDraftsClearForTest();
    g_ui.conflictResolveError.clear();
    const std::string byId = TurnedDownScalarSave(ctx, kScalarSprintByIdJson, c.EditId + kNotQueuedOffset);
    IM_CHECK_NO_RET(byId.find(sprintText) == 0);
}

// Ctrl+Enter does nothing while a conflict marker is left: the Save button is disabled then.
void CaseTextCtrlEnterBlockedByMarkers(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId + kNotQueuedOffset, kTextField)) {
        return;
    }
    const std::string before = ResolvedText();
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
    ctx->Yield(5);
    IM_CHECK_NO_RET(WindowIsLive(kModalTitle));
    IM_CHECK_NO_RET(g_ui.conflictResolveError.empty());
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), before.c_str());
    // Turned down: the caret is back in the text, where the markers are fixed.
    const ImGuiID fieldId = ctx->GetID(kTextField);
    const bool caretBack = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caretBack);
}

// With the markers gone, Ctrl+Enter runs the save and types no newline into the text.
void CaseTextCtrlEnterSavesWhenClean(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId + kNotQueuedOffset, kTextField)) {
        return;
    }
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
    ctx->KeyChars("merged");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
    const bool saveRan = YieldUntil(ctx, [] { return !g_ui.conflictResolveError.empty(); }, 30);
    IM_CHECK_NO_RET(saveRan);
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), "merged");
    const ImGuiID fieldId = ctx->GetID(kTextField);
    const bool caretBack = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caretBack);
}

// True when the key that is still held pressed the button. A button pressed from the keyboard holds the
// active id for one frame only, and KeyDown returns a frame after it, so the frame before counts too.
bool HeldKeyPressedButton(ImGuiTestContext* ctx, ImGuiID buttonId) {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    bool pressed = g.ActiveId == buttonId || g.ActiveIdPreviousFrame == buttonId;
    for (int i = 0; i < 3 && !pressed; ++i) {
        ctx->Yield();
        pressed = g.ActiveId == buttonId || g.ActiveIdPreviousFrame == buttonId;
    }
    return pressed;
}

// Ctrl+Enter saves the typed value even with the keyboard cursor on Use Theirs. ImGui presses the button
// under the cursor on Enter whatever the modifiers; the plain Enter at the end shows the check sees a press.
void CaseScalarCtrlEnterDoesNotPressFocusedButton(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kScalarJson, c.EditId + kNotQueuedOffset, kScalarField)) {
        return;
    }
    ctx->KeyCharsReplace("6");
    ctx->KeyPress(ImGuiKey_Escape); // leaves the field and keeps the 6
    ctx->Yield(2);
    IM_CHECK_NO_RET(ImGui::GetActiveID() == 0);
    const ImGuiID theirsId = ctx->GetID("Use Theirs");
    ctx->NavMoveTo("Use Theirs");
    IM_CHECK_NO_RET(ImGui::GetFocusID() == theirsId);

    ctx->KeyDown(ImGuiMod_Ctrl | ImGuiKey_Enter);
    const bool ctrlEnterPressedTheirs = HeldKeyPressedButton(ctx, theirsId);
    ctx->KeyUp(ImGuiMod_Ctrl | ImGuiKey_Enter);
    IM_CHECK_NO_RET(!ctrlEnterPressedTheirs);
    const bool saveRan = YieldUntil(ctx, [] { return !g_ui.conflictResolveError.empty(); }, 30);
    IM_CHECK_NO_RET(saveRan);
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), "6");

    // The refused save put the caret back in the field; leave it again and press the button with Enter.
    const ImGuiID fieldId = ctx->GetID(kScalarField);
    const bool caretBack = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caretBack);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(2);
    IM_CHECK_NO_RET(ImGui::GetActiveID() == 0);
    ctx->NavMoveTo("Use Theirs");
    ctx->KeyDown(ImGuiKey_Enter);
    const bool enterPressedTheirs = HeldKeyPressedButton(ctx, theirsId);
    ctx->KeyUp(ImGuiKey_Enter);
    IM_CHECK_NO_RET(enterPressedTheirs);
}

// The first Esc only leaves the text box and keeps what was typed; the second keeps the
// resolver open with the text and says how to discard.
void CaseTextEscKeepsTypedText(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField)) {
        return;
    }
    ctx->KeyChars("X");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);
    IM_CHECK_NO_RET(WindowIsLive(kModalTitle));
    IM_CHECK_NO_RET(ImGui::GetActiveID() == 0);
    IM_CHECK_NO_RET(ResolvedText().find('X') != std::string::npos);
    IM_CHECK_NO_RET(!SmatchetOfflineConflictUnsavedHintShownForTest());

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);
    IM_CHECK_NO_RET(WindowIsLive(kModalTitle));
    IM_CHECK_NO_RET(ResolvedText().find('X') != std::string::npos);
    IM_CHECK_NO_RET(SmatchetOfflineConflictUnsavedHintShownForTest());
}

// The Mine pane is a read-only field that can hold the active item. With a resolution typed, a click into it
// and then Esc is not "leaving the resolved field" (that field is not the active one): the resolver stays
// open with the text and says how to discard, as it does for an Esc pressed out of any field. The dialog
// scope is told which field counts as being typed in; "any field" would leave the Mine pane and show no hint.
void CaseTextEscAfterClickIntoReadOnlyPane(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField)) {
        return;
    }
    ctx->KeyChars("X");
    ctx->ItemClick("##CRMine");
    const ImGuiID mineId = ctx->GetID("##CRMine");
    const bool mineActive = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == mineId; }, 30);
    IM_CHECK_NO_RET(mineActive); // otherwise this case proves nothing about a read-only pane holding the item
    IM_CHECK_NO_RET(!SmatchetOfflineConflictUnsavedHintShownForTest());

    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);
    IM_CHECK_NO_RET(WindowIsLive(kModalTitle));
    IM_CHECK_NO_RET(ResolvedText().find('X') != std::string::npos);
    IM_CHECK_NO_RET(SmatchetOfflineConflictUnsavedHintShownForTest());
}

// The "not saved" hint after an Esc makes the same promise as the Cancel message: a cancelled resolution is
// kept in the draft store, also if Smatchet closes (#2510).
void CaseTextEscHintAgreesWithCancelMessage(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField)) {
        return;
    }
    ctx->KeyChars("X");
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_Escape);
    ctx->Yield(3);
    IM_CHECK_NO_RET(SmatchetOfflineConflictUnsavedHintShownForTest());
    const std::string hint = SmatchetOfflineConflictUnsavedHintTextForTest();
    IM_CHECK_NO_RET(hint.find("until Smatchet closes") == std::string::npos);
    IM_CHECK_NO_RET(hint.find("also if Smatchet closes") != std::string::npos);

    g_ui.offlineQueuePanelStatus.clear();
    ctx->SetRef(kModalTitle);
    ctx->ItemClick("Cancel");
    IM_CHECK_NO_RET(YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30));
    IM_CHECK_NO_RET(g_ui.offlineQueuePanelStatus.find("also if Smatchet closes") != std::string::npos);
}

// Types into the text resolver and cancels with the button. Returns what the field held, or an empty
// string (checked) when the resolver did not open or did not close.
std::string TypeAndCancelTextResolver(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField)) {
        return std::string();
    }
    ctx->KeyChars("X");
    ctx->Yield(2);
    const std::string typed = ResolvedText();
    ctx->SetRef(kModalTitle);
    ctx->ItemClick("Cancel");
    const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(closed);
    return closed ? typed : std::string();
}

// Cancel does not lose a resolution that was typed: opening the same conflict again brings it back,
// and "Start over" puts the field back to what the resolver opens with and forgets it.
void CaseTextCancelKeepsWhatWasTyped(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const std::string typed = TypeAndCancelTextResolver(ctx, c);
    IM_CHECK_NO_RET(typed.find('X') != std::string::npos);
    if (typed.empty()) {
        return;
    }
    if (!OpenWithCaretIn(ctx, kTextJson, c.EditId, kTextField)) {
        return;
    }
    IM_CHECK_NO_RET(SmatchetOfflineConflictDraftRestoredForTest());
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), typed.c_str());

    ctx->SetRef(kModalTitle);
    ctx->ItemClick("Start over");
    ctx->Yield(3);
    IM_CHECK_NO_RET(!SmatchetOfflineConflictDraftRestoredForTest());
    IM_CHECK_NO_RET(ResolvedText().find('X') == std::string::npos);
    IM_CHECK_NO_RET(ResolvedText().find("<<<<<<< mine") == 0);

    // Started over and cancelled: nothing is kept for the next open.
    ctx->SetRef(kModalTitle);
    ctx->ItemClick("Cancel");
    IM_CHECK_NO_RET(YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30));
    IM_CHECK_NO_RET(OpenConflictModal(ctx, kTextJson, c.EditId));
    ctx->Yield(2);
    IM_CHECK_NO_RET(!SmatchetOfflineConflictDraftRestoredForTest());
    IM_CHECK_NO_RET(ResolvedText().find('X') == std::string::npos);
}

// The kept text belongs to the conflict it was typed against. When the same queue id opens with a
// different server side, the resolver starts from that conflict, not from the old text.
void CaseKeptTextNotPutIntoChangedConflict(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const std::string typed = TypeAndCancelTextResolver(ctx, c);
    // Without the typed X nothing is kept, and the checks below would pass with the guard broken.
    IM_CHECK_NO_RET(typed.find('X') != std::string::npos);
    if (typed.find('X') == std::string::npos) {
        return;
    }
    const bool opened = OpenConflictModal(ctx, "{\"kind\":\"text\",\"mine\":\"a\",\"theirs\":\"c\"}", c.EditId);
    IM_CHECK_NO_RET(opened);
    ctx->Yield(2);
    IM_CHECK_NO_RET(!SmatchetOfflineConflictDraftRestoredForTest());
    IM_CHECK_NO_RET(ResolvedText().find('X') == std::string::npos);
    IM_CHECK_NO_RET(ResolvedText().find("\nc\n") != std::string::npos);
}

// Nothing typed: one Esc closes, although the caret starts in the field.
void CaseUntouchedEscCloses(ImGuiTestContext* ctx, const KeyboardCase& c) {
    if (!OpenWithCaretIn(ctx, kScalarJson, c.EditId, kScalarField)) {
        return;
    }
    ctx->KeyPress(ImGuiKey_Escape);
    const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(closed);
    IM_CHECK_NO_RET(g_ui.conflictResolveBuf.empty());
}

// A pane with no field: Esc closes like Cancel, and the edit stays queued.
void CaseUnverifiedEscCloses(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const bool opened = OpenConflictModal(ctx, "{\"kind\":\"unverified\",\"mine\":\"mine-original\"}", c.EditId);
    IM_CHECK_NO_RET(opened);
    if (!opened) {
        return;
    }
    ctx->Yield(3);
    ctx->KeyPress(ImGuiKey_Escape);
    const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(closed);
    IM_CHECK_NO_RET(!c.App->GetPendingFieldEdits().empty());
}

// Puts the queued edit into the conflict state and waits for its row. Returns the ref of
// the row's State cell, or nullptr (checked). A row whose site has no open grid shows
// "Held" in place of "Conflict"; it opens the same way.
const char* MarkConflictAndFindRow(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const bool marked = c.App->MarkFieldEditConflictForTest(c.EditId, kScalarJson);
    IM_CHECK_NO_RET(marked);
    const bool shown = YieldUntil(ctx, [&] {
        const std::shared_ptr<const OfflineQueueSnapshot> snap = c.App->GetOfflineQueueSnapshot();
        for (const PendingFieldEditRecord& row : snap->PendingEdits) {
            if (row.Id == c.EditId && row.HasMergeConflict) {
                return true;
            }
        }
        return false;
    });
    IM_CHECK_NO_RET(shown);
    if (!marked || !shown) {
        return nullptr;
    }
    ctx->Yield(2);
    ctx->SetRef(kHostTitle);
    const char* rowRef = ctx->ItemExists("**/Conflict") ? "**/Conflict" : "**/Held";
    const bool rowFound = ctx->ItemExists(rowRef);
    IM_CHECK_NO_RET(rowFound);
    return rowFound ? rowRef : nullptr;
}

// Enter on the row opens the resolver with the caret in the value field; closing it
// with Esc puts the keyboard cursor back on the row.
void CaseRowEnterOpensResolver(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const char* rowRef = MarkConflictAndFindRow(ctx, c);
    if (rowRef == nullptr) {
        return;
    }
    const ImGuiID rowId = ctx->ItemInfo(rowRef).ID;
    ctx->NavMoveTo(rowRef);
    ctx->KeyPress(ImGuiKey_Enter);
    const bool opened = YieldUntil(ctx, [] { return WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(opened);
    if (!opened) {
        return;
    }
    IM_CHECK_NO_RET(g_ui.conflictResolveDbId == c.EditId);
    ctx->SetRef(kModalTitle);
    const ImGuiID fieldId = ctx->GetID(kScalarField);
    const bool caret = YieldUntil(ctx, [&] { return ImGui::GetActiveID() == fieldId; }, 30);
    IM_CHECK_NO_RET(caret);
    IM_CHECK_STR_EQ_NO_RET(ResolvedText().c_str(), "5");

    ctx->KeyPress(ImGuiKey_Escape);
    const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(closed);
    ctx->Yield(2);
    IM_CHECK_NO_RET(ImGui::GetFocusID() == rowId);
}

// A single click and Space only place the cursor on the row, as before.
void CaseRowClickAndSpaceDoNotOpen(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const char* rowRef = MarkConflictAndFindRow(ctx, c);
    if (rowRef == nullptr) {
        return;
    }
    ctx->ItemClick(rowRef);
    ctx->Yield(5);
    IM_CHECK_NO_RET(!WindowIsLive(kModalTitle));

    ctx->NavMoveTo(rowRef);
    ctx->KeyPress(ImGuiKey_Space);
    ctx->Yield(5);
    IM_CHECK_NO_RET(!WindowIsLive(kModalTitle));
}

// The queued payload of the case's edit, or empty when the row is gone.
std::string QueuedPayload(const KeyboardCase& c) {
    for (const PendingFieldEditRecord& row : c.App->GetPendingFieldEdits()) {
        if (row.Id == c.EditId) {
            return row.FieldsPayloadJson;
        }
    }
    return std::string();
}

// Alt+M / Alt+T resolve to mine / theirs, as Use Mine / Use Theirs do, and each button prints its key
// (INPUT_UX_AUDIT.md #111). The fixture tracker is held down, so the re-queued edit stays queued and
// its payload shows which side the key chose.
void CasePickKeysResolveToEachSide(ImGuiTestContext* ctx, const KeyboardCase& c) {
    smatchet_tests::ScopedFakeNetworkReset networkReset;
    smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
    const struct {
        ImGuiKeyChord Chord;
        const char* Chosen; // the side the key picks (kScalarJson: mine 5, theirs 8)
        const char* Other;
    } picks[] = {{ImGuiMod_Alt | ImGuiKey_M, "\"5\"", "\"8\""}, {ImGuiMod_Alt | ImGuiKey_T, "\"8\"", "\"5\""}};
    for (const auto& pick : picks) {
        if (!OpenWithCaretIn(ctx, kScalarJson, c.EditId, kScalarField)) {
            return;
        }
        IM_CHECK_STR_EQ_NO_RET(smatchet::ui::KeyHintDrawnForTest("Use Mine").c_str(), "Alt+M");
        IM_CHECK_STR_EQ_NO_RET(smatchet::ui::KeyHintDrawnForTest("Use Theirs").c_str(), "Alt+T");
        ctx->KeyPress(pick.Chord);
        const bool resolved = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 60);
        if (!resolved) {
            ctx->LogError("the pick key did not resolve the conflict");
        }
        IM_CHECK_NO_RET(resolved);
        if (!resolved) {
            return;
        }
        const std::string payload = QueuedPayload(c);
        IM_CHECK_NO_RET(!payload.empty());
        IM_CHECK_NO_RET(payload.find(pick.Chosen) != std::string::npos);
        IM_CHECK_NO_RET(payload.find(pick.Other) == std::string::npos);
    }
}

// Opens the resolver on the case's conflict row as the `unverified` kind and clicks "Discard my edit".
// Returns true once the resolver closed.
bool DiscardFromUnverifiedPane(ImGuiTestContext* ctx, const KeyboardCase& c) {
    const bool opened = OpenConflictModal(ctx, "{\"kind\":\"unverified\",\"mine\":\"mine-original\"}", c.EditId);
    IM_CHECK_NO_RET(opened);
    if (!opened) {
        return false;
    }
    ctx->SetRef(kModalTitle);
    ctx->ItemClick("Discard my edit");
    const bool closed = YieldUntil(ctx, [] { return !WindowIsLive(kModalTitle); }, 30);
    IM_CHECK_NO_RET(closed);
    return closed;
}

// "Discard my edit" is undoable (INPUT_UX_AUDIT.md #116): the row is held, hidden from the panel and still
// queued while the toast is live. Undo brings it back; a toast that settles (DismissAllLive commits) deletes
// it. The fixture tracker is held down, though a conflict row is never replayed anyway.
void CaseDiscardMyEditUndo(ImGuiTestContext* ctx, const KeyboardCase& c) {
    smatchet_tests::ScopedFakeNetworkReset networkReset;
    smatchet_tests::GlobalFakeNetwork().Set(smatchet_tests::FakeNetworkMode::TransportDown);
    SmatchetToastManager& toasts = SmatchetToastManager::Instance();
    toasts.DismissAllLive();
    const char* rowRef = MarkConflictAndFindRow(ctx, c);
    if (rowRef == nullptr) {
        return;
    }
    // Pinned by ID: the wildcard ref would also match another conflict row in the panel.
    const ImGuiID rowId = ctx->ItemInfo(rowRef).ID;
    IM_CHECK_NO_RET(rowId != 0);
    const std::string title = "Edit discarded";
    const std::string body = "Undo puts it back in the offline queue.";

    // Discard, then Undo: the row was held and hidden but never left the queue, and it comes back.
    if (!DiscardFromUnverifiedPane(ctx, c)) {
        return;
    }
    IM_CHECK_NO_RET(SmatchetOfflineConflictDiscardHeldForTest(c.EditId));
    IM_CHECK_NO_RET(toasts.LiveUndoableCount() == 1);
    IM_CHECK_NO_RET(toasts.HasLive(title, body));
    IM_CHECK_NO_RET(!QueuedPayload(c).empty());
    ctx->SetRef(kHostTitle);
    const bool hidden = YieldUntil(ctx, [&] { return !ctx->ItemExists(rowId); }, 30);
    IM_CHECK_NO_RET(hidden);
    IM_CHECK_NO_RET(toasts.UndoNewest());
    IM_CHECK_NO_RET(!SmatchetOfflineConflictDiscardHeldForTest(c.EditId));
    IM_CHECK_NO_RET(toasts.LiveUndoableCount() == 0);
    const bool back = YieldUntil(ctx, [&] { return ctx->ItemExists(rowId); }, 30);
    IM_CHECK_NO_RET(back);
    IM_CHECK_NO_RET(!QueuedPayload(c).empty());

    // Discard again and let the toast settle: the row is deleted, and it stays hidden until the published
    // queue snapshot no longer has it (no flash of the deleted row), then the hold is forgotten.
    if (!DiscardFromUnverifiedPane(ctx, c)) {
        return;
    }
    IM_CHECK_NO_RET(SmatchetOfflineConflictDiscardHeldForTest(c.EditId));
    toasts.DismissAllLive();
    IM_CHECK_NO_RET(QueuedPayload(c).empty());
    ctx->SetRef(kHostTitle);
    bool flashed = false;
    const bool forgotten = YieldUntil(
        ctx,
        [&] {
            flashed = flashed || ctx->ItemExists(rowId);
            return !SmatchetOfflineConflictDiscardHeldForTest(c.EditId);
        },
        60);
    IM_CHECK_NO_RET(forgotten);
    IM_CHECK_NO_RET(!flashed);
    IM_CHECK_NO_RET(!ctx->ItemExists(rowId));
    IM_CHECK_NO_RET(SmatchetOfflineConflictDiscardHeldCountForTest() == 0);
}

void RegisterKeyboardCase(ImGuiTestEngine* engine, const char* name, KeyboardCaseBody body) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineConflictModalPanes", name);
    t->UserData = reinterpret_cast<void*>(body);
    t->TestFunc = [](ImGuiTestContext* ctx) {
        RunKeyboardCase(ctx, reinterpret_cast<KeyboardCaseBody>(ctx->Test->UserData));
    };
}

} // namespace

extern "C" void SmatchetRegisterOfflineConflictModalPanesTests(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "OfflineConflictModalPanes", "ConflictModal_ScalarAndUnverifiedPerKindPanes");
    t->TestFunc = [](ImGuiTestContext* ctx) { RunPerKindModalTest(ctx); };

    RegisterKeyboardCase(engine, "Scalar_CaretInFieldOnOpen", CaseScalarCaretInFieldOnOpen);
    RegisterKeyboardCase(engine, "Text_CaretInFieldOnOpen", CaseTextCaretInFieldOnOpen);
    RegisterKeyboardCase(engine, "Scalar_EnterSaves", CaseScalarEnterSaves);
    RegisterKeyboardCase(engine, "Scalar_EstimateFieldValidatesAsTyped", CaseScalarEstimateFieldValidatesAsTyped);
    RegisterKeyboardCase(engine, "Scalar_SprintKindFromContext", CaseScalarSprintKindFromContext);
    RegisterKeyboardCase(engine, "Scalar_CtrlEnterDoesNotPressFocusedButton",
                         CaseScalarCtrlEnterDoesNotPressFocusedButton);
    RegisterKeyboardCase(engine, "Text_CtrlEnterBlockedByMarkers", CaseTextCtrlEnterBlockedByMarkers);
    RegisterKeyboardCase(engine, "Text_CtrlEnterSavesWhenClean", CaseTextCtrlEnterSavesWhenClean);
    RegisterKeyboardCase(engine, "Text_EscKeepsTypedText", CaseTextEscKeepsTypedText);
    RegisterKeyboardCase(engine, "Text_EscHintAgreesWithCancelMessage", CaseTextEscHintAgreesWithCancelMessage);
    RegisterKeyboardCase(engine, "Text_EscAfterClickIntoReadOnlyPane", CaseTextEscAfterClickIntoReadOnlyPane);
    RegisterKeyboardCase(engine, "Text_CancelKeepsWhatWasTyped", CaseTextCancelKeepsWhatWasTyped);
    RegisterKeyboardCase(engine, "Text_KeptTextNotPutIntoChangedConflict", CaseKeptTextNotPutIntoChangedConflict);
    RegisterKeyboardCase(engine, "Untouched_EscCloses", CaseUntouchedEscCloses);
    RegisterKeyboardCase(engine, "Unverified_EscCloses", CaseUnverifiedEscCloses);
    RegisterKeyboardCase(engine, "ConflictRow_EnterOpensResolver", CaseRowEnterOpensResolver);
    RegisterKeyboardCase(engine, "ConflictRow_ClickAndSpaceDoNotOpen", CaseRowClickAndSpaceDoNotOpen);
    RegisterKeyboardCase(engine, "Scalar_AltMAndAltTPickASide", CasePickKeysResolveToEachSide);
    RegisterKeyboardCase(engine, "Unverified_DiscardMyEditUndo", CaseDiscardMyEditUndo);
}

#endif // SMATCHET_BUILD_UI_TESTS
