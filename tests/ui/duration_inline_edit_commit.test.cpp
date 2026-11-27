// duration_inline_edit_commit.test.cpp — bucket-E (ImGui Test Engine) coverage for the inline
// duration-cell editor's COMMIT / CANCEL contract.
//
// WHAT THIS LOCKS IN — three recently-fixed bugs in TicketFieldEditor.cpp
// RenderTextInlineEdit (the duration path = DrawDurationFieldWithSuggestions):
//   1. No-op edit must NEVER PUT — exiting an EMPTY cell that was already empty, or releasing an
//      existing value unchanged, must queue NO PendingFieldEdit (dirty-check: `valueChanged`).
//   2. Focus-loss must END the edit even when the value is unchanged (the editor must not stick
//      open) — driven by ShouldEndInlineEdit + IsItemDeactivated (not ...AfterEdit).
//   3. Escape cancels with no PUT.
//
// COMPLEMENTS, does NOT duplicate, tests/Core/TicketFieldEditorCommitPolicyPure.test.cpp. That
// suite unit-tests the pure DECISION functions (ShouldCommitInlineFieldEdit / ShouldEndInlineEdit).
// THIS suite drives the REAL ImGui editor through the engine (type / Escape / click-away) and
// asserts the END-TO-END glue produces the right commit-vs-no-commit OUTCOME: that the production
// InputText + IsItemDeactivated + dirty-check wiring actually feeds those pure functions correctly.
//
// HOW IT REACHES THE PRODUCTION CODE — RenderTextInlineEdit is in an anonymous namespace in
// TicketFieldEditor.cpp; SmatchetTest_RenderTextInlineEdit (declared in TicketFieldEditor_detail.h,
// SMATCHET_BUILD_UI_TESTS-only) is a thin external-linkage forwarder onto it. The GuiFunc below
// calls that forwarder every frame the cell is "editing", exactly as RenderTextEditor's editing
// branch does in production — so the duration widget, the popup, the dirty-check, and the
// session-end logic are all the REAL code. The only thing the test owns is the per-frame call site
// (which is a 1:1 mirror of RenderTextEditor.cpp:602-604) and the observation of the caller-owned
// pendingEdits vector (the exact vector the production caller passes and later drains to the PUT
// queue) — so a queued edit here is the same signal a real grid edit produces.
//
// ALSO HERE — the worklog dialog's keyboard contract (cases E-K at the end of this file). The dialog
// is the other user of the duration field and steers that field's popup state, so its cases run in
// the same group.
//
// OBSERVATION SURFACE — pendingEdits (std::vector<PendingFieldEdit>), the caller-owned sink
// RenderTextInlineEdit::QueueEdit appends to. A spurious commit = a non-empty vector; a correct
// no-op = an empty vector. Deterministic, in-process, no network, no golden image.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "TicketFieldEditor_detail.h"  // SmatchetTest_RenderTextInlineEdit forwarder
#include "TicketFieldEditor_Worklog.h" // the worklog dialog + its test snapshot
#include "Ui/SmatchetSubmitKeys.h"     // what the key-hint button and the placeholder drew

#include "AppController.h"                         // IAppThreading for the dialog's worker; IssueCreateResult
#include "CachedTicketTypes.h"                     // CachedTicket
#include "DraftStore.h"                            // the entry the worklog dialog keeps
#include "Commands/Scenarios/UiTestScenario.h"     // SmatchetActiveUiTestAppController
#include "GridPane.h"                              // the pane the harness draws the dialog in
#include "Interfaces/IAppTicketMutations.h"        // the facet the dialog saves through
#include "PendingActionTypes.h"                    // PendingActionSubmitResult
#include "SmatchetUiSession.h"                     // PendingFieldEdit; g_ui.activePaneForDraw
#include "SpreadsheetState.h"                      // SpreadsheetState
#include "Tracker/TrackerFieldValueParseExplain.h" // the dialog and the field explain a bad time with it
#include "TrackerFieldSchema.h"                    // TrackerField
#include "Ui/SmatchetToast.h"                      // the toast a save result goes to when no dialog shows it

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Per-test fixture. TU-local global reached through ImGuiTest::UserData (the engine runs GuiFunc and
// TestFunc on the same UI thread in alternating frames, so no synchronisation is needed — same
// pattern as ai_assistant_enter_send.test.cpp).
struct DurationEditState {
    CachedTicket ticket;
    TrackerField field;
    SpreadsheetState state;
    std::vector<PendingFieldEdit> pendingEdits;

    // The dummy "elsewhere" widget the TestFunc clicks to simulate clicking a DIFFERENT cell:
    // its activation deactivates the duration InputText → IsItemDeactivated() fires, exactly as a
    // click on another grid cell does. Counter only exists so the widget has visible side-effect.
    int elsewhereClicks = 0;
};

DurationEditState g_state;

const char* kIssueId = "SMAT-1";
const char* kFieldId = "timeoriginalestimate"; // IsTimeDurationField(...) == true → duration path

// Reset the fixture and seed the cell. `seedValue` is the value already stored on the ticket (the
// "current" pre-edit value); empty string models an empty estimate cell.
void ResetFixture(const std::string& seedValue) {
    g_state.ticket = CachedTicket{};
    g_state.ticket.id = kIssueId;
    if (!seedValue.empty()) {
        g_state.ticket.fieldValues[kFieldId] = seedValue;
    }

    g_state.field = TrackerField{};
    g_state.field.Id = kFieldId;
    g_state.field.Name = "Original estimate";

    g_state.state = SpreadsheetState{};
    g_state.pendingEdits.clear();
    g_state.elsewhereClicks = 0;
}

// Begin editing the duration cell with `currentValue` as the pre-populated buffer — the same call
// RenderTextEditor makes when the user opens the cell (StartEditingField copies the value into
// EditBuffer and arms EditJustStarted).
void StartEditing(const std::string& currentValue) {
    g_state.state.StartEditingField(kIssueId, kFieldId, currentValue);
}

bool IsEditing() { return g_state.state.IsEditingField(kIssueId, kFieldId); }

// GuiFunc — mirrors RenderTextEditor.cpp's editing branch (lines 602-604): while the cell is in
// the editing state, draw the REAL inline editor. A trailing dummy button gives the TestFunc a
// concrete "other cell" to click for the focus-loss / click-away gesture.
void DrawDurationEditorHarness(ImGuiTestContext* ctx) {
    auto* s = static_cast<DurationEditState*>(ctx->Test->UserData);
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::DurationInlineEdit", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (s->state.IsEditingField(kIssueId, kFieldId)) {
            // THE PRODUCTION CALL — duration widget, popup, dirty-check, session-end logic.
            SmatchetTest_RenderTextInlineEdit(s->ticket, s->field, s->state, s->pendingEdits);
        } else {
            ImGui::TextUnformatted("(not editing)");
        }
        if (ImGui::Button("Elsewhere##other_cell")) {
            ++s->elsewhereClicks;
        }
    }
    ImGui::End();
}

const char* kElsewhereRef = "Elsewhere##other_cell";

// Drive the focus-loss / click-away gesture until the edit session ends, or the frame budget is
// spent. The duration field opens its suggestions popup on the first activation of an EMPTY buffer;
// the first click then lands on the popup (closing it, NOT committing — the production refocus
// branch), so a single click is not always enough. Re-click "Elsewhere" each iteration until the
// editor closes (IsEditing() == false). Returns true if the session ended within the budget.
bool ClickAwayUntilEnded(ImGuiTestContext* ctx, int maxAttempts = 12) {
    for (int i = 0; i < maxAttempts && IsEditing(); ++i) {
        // MouseMove + MouseClick (geometry) rather than ItemClick (by ref): when the suggestions
        // popup is open over an empty cell it occludes the "Elsewhere" button, and a ref-based
        // ItemClick errors the context on the occlusion. A geometric click hits the button rect
        // regardless and, on the popup-open frame, lands on the popup first (closing it) — exactly
        // the production refocus-vs-commit sequence — so we re-attempt until the session ends.
        ctx->MouseMove(kElsewhereRef);
        ctx->MouseClick(0);
        ctx->Yield();
        ctx->Yield();
    }
    return !IsEditing();
}

// ============================================================================
// A. Exit an unchanged EMPTY duration cell → NO commit, session ends.
// ============================================================================
void RegisterExitEmptyUnchangedNoCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DurationInlineEdit", "ExitEmptyUnchanged_NoCommit");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDurationEditorHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*seedValue=*/"");
        StartEditing(/*currentValue=*/"");
        ctx->SetRef("SmatchetTest::DurationInlineEdit");
        ctx->Yield();
        ctx->Yield();

        // Do NOT type anything — the empty cell stays empty (the no-op case). End the edit by
        // clicking another cell.
        const bool ended = ClickAwayUntilEnded(ctx);
        IM_CHECK(ended); // bug 2: focus-loss must END the edit even when unchanged

        // bug 1: an unchanged empty cell must queue NO edit (no spurious empty-valued PUT).
        IM_CHECK_EQ(static_cast<int>(g_state.pendingEdits.size()), 0);
    };
}

// ============================================================================
// B. Open an EMPTY editor, type a value, COMMIT → exactly one edit with the typed value.
// ============================================================================
// The commit-side complement that proves the no-commit assertions (A/C/D) are not vacuous: a
// genuine change DOES queue exactly one edit carrying the typed value. Typing into an empty duration
// cell reliably fills the buffer via the widget's type-to-edit splice (the empty cell opens the
// suggestions popup; the splice routes the printable chars into the buffer and closes it). The
// commit gesture is Enter (EnterReturnsTrue) — the deterministic submit for the duration widget,
// whose focus self-management makes a synthetic pure-focus-loss commit unreliable in the headless
// engine (see the residue NOTE at the end of this file).
void RegisterChangeThenCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DurationInlineEdit", "TypeIntoEmptyThenCommit_CommitsOnce");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDurationEditorHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*seedValue=*/""); // empty cell
        StartEditing(/*currentValue=*/"");
        ctx->SetRef("SmatchetTest::DurationInlineEdit");
        ctx->Yield();
        ctx->Yield();

        // Editor auto-focuses; type straight into it. The empty-cell suggestions popup's
        // type-to-edit splice routes the chars into the buffer.
        ctx->KeyChars("2h");
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_STR_EQ(g_state.state.EditBuffer, "2h"); // the real buffer took the typed value

        // Explicit submit — EnterReturnsTrue inside DrawDurationFieldWithSuggestions.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(!IsEditing());

        // Exactly one commit, carrying the typed value for this issue+field (changed "" -> "2h").
        IM_CHECK_EQ(static_cast<int>(g_state.pendingEdits.size()), 1);
        if (!g_state.pendingEdits.empty()) {
            const PendingFieldEdit& e = g_state.pendingEdits.front();
            IM_CHECK_STR_EQ(e.IssueId.c_str(), kIssueId);
            IM_CHECK_STR_EQ(e.Field.Id.c_str(), kFieldId);
            IM_CHECK_EQ(static_cast<int>(e.Values.size()), 1);
            if (!e.Values.empty()) {
                IM_CHECK_STR_EQ(e.Values.front().c_str(), "2h");
            }
        }
    };
}

// ============================================================================
// C. Escape after focusing an existing value → NO commit, session ends.
// ============================================================================
void RegisterEscapeCancelsNoCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DurationInlineEdit", "EscapeOnExistingValue_NoCommit");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDurationEditorHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*seedValue=*/"1h"); // cell already holds a value
        StartEditing(/*currentValue=*/"1h");
        ctx->SetRef("SmatchetTest::DurationInlineEdit");
        ctx->Yield();
        ctx->Yield();

        // Editor auto-focuses on StartEditing; the existing value "1h" is in the focused input.
        // Escape — ImGui reverts the buffer and the edit must cancel with no PUT.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(!IsEditing());                                        // bug 3: Escape ends the session
        IM_CHECK_EQ(static_cast<int>(g_state.pendingEdits.size()), 0); // bug 3: Escape never PUTs
    };
}

// ============================================================================
// D. Open editor on a cell WITH a value, click another cell without changing it → ENDS, NO commit.
// ============================================================================
// Faithful to production: the buffer seed and the stored value are the SAME value (the grid passes
// `ticket.GetFieldValue(field.Id)` both as the cell's currentValue and as the dirty-check baseline —
// SmatchetActiveProjectGridUi.cpp:1207), and duration values are stored already-formatted ("3h"),
// so there is NO raw-vs-formatted split. The two invariants under test are the real remaining bugs:
//   - bug "stuck editor" (focus-loss must END an UNCHANGED duration edit): the duration widget's
//     deactivation now flows through IsItemDeactivated() (not ...AfterEdit, which fires only after a
//     real edit). An existing value opens with NO auto-popup, so a single click-away deactivates the
//     input and ends the session. NO Escape fallback here — the IM_CHECK(!IsEditing()) below has
//     teeth: against the pre-fix ...AfterEdit it stays open and FAILS.
//   - bug "no-op PUT" (unchanged value must NEVER commit): the dirty check (valueChanged) keeps the
//     session-end from queuing an edit.
void RegisterUnchangedExistingValueNoCommit(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DurationInlineEdit", "UnchangedExistingValue_NoCommit");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDurationEditorHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*seedValue=*/"3h"); // production: buffer seed == stored value == GetFieldValue
        StartEditing(/*currentValue=*/"3h");
        ctx->SetRef("SmatchetTest::DurationInlineEdit");
        ctx->Yield();
        ctx->Yield();

        // Focus-loss ONLY: click another cell, value untouched. No Escape fallback.
        const bool ended = ClickAwayUntilEnded(ctx);
        IM_CHECK(ended); // stuck-editor fix: focus-loss ENDS an unchanged duration edit

        // bug 1 (the load-bearing assertion): an unchanged existing value must NEVER PUT.
        IM_CHECK_EQ(static_cast<int>(g_state.pendingEdits.size()), 0);
    };
}

// NOTE on the two duration gestures NOT covered as bucket-E here:
//   - Pure FOCUS-LOSS commit of a *changed* duration value, and
//   - In-place EDIT (backspace/replace) of an existing duration value,
// both fight the duration widget's per-frame focus self-management (needRepositionAndFocus +
// type-to-edit splice + deselect-on-open caret reset) when driven by synthetic engine input, so
// they cannot be driven deterministically without product changes. The COMMIT path itself is
// proven by case B (type → Enter), and the no-op/no-PUT + session-end invariants (the three fixed
// bugs) are fully covered by A/C/D. The pure DECISION logic for those gestures is already covered
// by tests/Core/TicketFieldEditorCommitPolicyPure.test.cpp.

// ============================================================================
// Worklog dialog keyboard contract (TicketFieldEditor_Worklog.cpp)
// ============================================================================
// The dialog is the other user of the duration field, and it steers that field's private popup state
// to keep the caret in Time spent on open (PrimeTimeSpentFocus). These cases drive the REAL dialog:
// the caret on open, Enter in Time spent, Ctrl+Enter from any field, and Esc. The save goes to a
// recording IAppTicketMutations, so a test counts saves and reads what was sent without a tracker.

// Records every worklog the dialog's worker submits. The dialog reaches nothing else on this facet.
class RecordingWorklogMutations final : public IAppTicketMutations {
  public:
    struct Call {
        std::string IssueId;
        std::string TimeSpent;
        std::string Description;
        /// The pane and tracker site the worklog was sent to.
        std::string TargetPaneId;
        std::string TargetBackendKey;
        /// What the send-or-queue route was told about the tracker.
        TrackerConnectivityState TargetConnectivity = TrackerConnectivityState::Unknown;
    };

    using IAppTicketMutations::QueueCreateOffline;

    const TrackerField* FindFieldById(const std::string&) const override { return nullptr; }
    /// The focused pane, as the controller latches it: it can still be another pane than the one that
    /// draws the dialog (the pane focus follows a click a frame or two late).
    PendingActionTarget LatchPendingActionTarget() const override {
        PendingActionTarget target;
        target.PaneId = m_FocusedPaneId;
        target.BackendKey = m_BackendKey;
        return target;
    }
    /// A named pane, as the controller latches it: its tracker site when the pane is known (SetPaneSite),
    /// otherwise a target with no backend and no backend key, like a pane with no live context.
    PendingActionTarget LatchPendingActionTargetForPane(const std::string& paneId) const override {
        PendingActionTarget target;
        target.PaneId = paneId;
        const auto it = m_PaneSites.find(paneId);
        if (it != m_PaneSites.end()) {
            target.BackendKey = it->second;
        }
        return target;
    }
    PendingActionSubmitResult SubmitFieldEditOrQueue(const PendingActionTarget&, const std::string&,
                                                     const TrackerField&, const std::vector<std::string>&) override {
        return PendingActionSubmitResult();
    }
    PendingActionSubmitResult SubmitOrQueueComment(const PendingActionTarget&, const std::string&,
                                                   const std::string&) override {
        return PendingActionSubmitResult();
    }
    // Runs on the dialog's worker thread.
    PendingActionSubmitResult SubmitOrQueueWorklog(const PendingActionTarget& target, const std::string& issueId,
                                                   const std::string& timeSpent, const std::string&, const std::string&,
                                                   const std::string& workDescription, const std::string&) override {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Calls.push_back(
                Call{issueId, timeSpent, workDescription, target.PaneId, target.BackendKey, target.Connectivity});
        }
        while (m_Hold.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        PendingActionSubmitResult result;
        result.K = PendingActionSubmitResult::Kind::Sent;
        return result;
    }
    std::int64_t QueueCreateOffline(const IssueDraft&) override { return 0; }
    std::future<IssueCreateResult> CreateIssueAsync(const IssueDraft&, smatchet::ui::CancelToken) override {
        return std::future<IssueCreateResult>();
    }
    std::shared_ptr<const std::vector<CachedTicket>> GetActiveTicketsSnapshot() const override { return nullptr; }
    // Bulk-edit pre-flight reads; the single-worklog dialog never calls them.
    TrackerConnectivityState GetLastTrackerConnectivityState() const override { return m_Connectivity.load(); }
    bool IsShuttingDown() const override { return false; }

    /// The focused pane and its tracker site. UI-thread only, like the open request: the test coroutine
    /// and the GuiFunc never run at once. Reset leaves it alone.
    void SetFocusedPane(const std::string& paneId, const std::string& backendKey) {
        m_FocusedPaneId = paneId;
        m_BackendKey = backendKey;
    }
    /// The panes LatchPendingActionTargetForPane knows, each with its tracker site. UI-thread only, like
    /// SetFocusedPane. Reset leaves them alone.
    void SetPaneSite(const std::string& paneId, const std::string& backendKey) { m_PaneSites[paneId] = backendKey; }
    void ClearPaneSites() { m_PaneSites.clear(); }
    /// The last connectivity probe, as the controller reports it. Reset puts it back to reachable.
    void SetConnectivity(TrackerConnectivityState state) { m_Connectivity.store(state); }
    /// While held, a submit stays "in flight" (the worker waits) so a test can act during the save.
    void SetHold(bool hold) { m_Hold.store(hold); }
    std::vector<Call> Calls() const {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Calls;
    }
    int CallCount() const { return static_cast<int>(Calls().size()); }
    void Reset() {
        m_Hold.store(false);
        m_Connectivity.store(TrackerConnectivityState::AuthenticatedReachable);
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Calls.clear();
    }

  private:
    mutable std::mutex m_Mutex;
    std::vector<Call> m_Calls;
    std::atomic<bool> m_Hold{false};
    std::atomic<TrackerConnectivityState> m_Connectivity{TrackerConnectivityState::AuthenticatedReachable};
    std::string m_FocusedPaneId;
    std::string m_BackendKey;
    std::map<std::string, std::string> m_PaneSites;
};

// TU-lifetime: the dialog's worker keeps a pointer to the facet until its submit returns.
RecordingWorklogMutations g_worklogMutations;
CachedTicket g_worklogTicket;
bool g_worklogOpenRequested = false;
// The grid pane the harness draws the dialog in, as the pane-window loop does (g_ui.activePaneForDraw).
// An empty id draws it outside any pane, like the field viewer.
GridPane g_worklogDrawingPane;
// Another grid pane that shows the same issue in the same column and draws the dialog first each frame, as a
// second visible pane window does. Empty: no such pane.
GridPane g_worklogOtherDrawingPane;
// The owning pane stopped drawing the cell (the pane was closed or hidden, or the row left its grid).
bool g_worklogOwnerPaneHidden = false;

const char* kWorklogHostWindow = "SmatchetTest::WorklogDialog";
const char* kWorklogIssueId = "WLOGTEST-1"; // no grid row has it, so only this harness draws the dialog
const char* kWorklogOwnerField = "timespent";
// Two tracker sites that both have an issue with the harness's key.
const char* kWorklogSiteKey = "Jira@site-a.example";
const char* kWorklogOtherSiteKey = "Jira@site-b.example";
// The draft-store scope the dialog keeps its entry under: the site, then the issue. Spelled out here
// rather than taken from the product helper, so a change of shape shows up as a failing case.
std::string WorklogDraftScopeForTest(const char* backendKey = kWorklogSiteKey) {
    return std::string(backendKey) + "|" + kWorklogIssueId;
}
// "0m" reads as a duration but logs nothing, so the dialog asks for a time above zero (the same words the field
// shows under itself). Built with the parser's own text so the case holds in any language.
std::string WorklogBadTimeError() {
    TrackerParseExplain::ParseMessage zero;
    zero.Id = TrackerParseExplain::ParseMessageId::DurationRequiredPositive;
    return TrackerParseExplain::DescribeParseMessage(zero);
}

using TicketFieldEditorWorklog::GetWorklogDialogSnapshotForTests;
using TicketFieldEditorWorklog::WorklogDialogTestSnapshot;

// GuiFunc: a host window that opens the dialog on request and draws it every frame, the way the
// owning grid cell does.
void DrawWorklogDialogHarness(ImGuiTestContext*) {
    AppController* app = SmatchetActiveUiTestAppController();
    if (!app) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420, 120), ImGuiCond_Appearing);
    if (ImGui::Begin(kWorklogHostWindow, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("worklog dialog host");
        // The pane loop has already run this frame, so the slot is free; it is put back right after.
        GridPane* const savedPane = g_ui.activePaneForDraw;
        if (!g_worklogOtherDrawingPane.id.empty()) {
            // Its own id scope, as its own pane window has: the open popup is not under it.
            g_ui.activePaneForDraw = &g_worklogOtherDrawingPane;
            ImGui::PushID(g_worklogOtherDrawingPane.id.c_str());
            TicketFieldEditorWorklog::RenderTimeTrackingModal(*app, g_worklogMutations, g_worklogTicket,
                                                              kWorklogOwnerField);
            ImGui::PopID();
        }
        g_ui.activePaneForDraw = g_worklogDrawingPane.id.empty() ? savedPane : &g_worklogDrawingPane;
        // Opened inside the pane's draw, as the grid cell does on a click.
        if (g_worklogOpenRequested) {
            g_worklogOpenRequested = false;
            TicketFieldEditorWorklog::OpenWorklogDialog(g_worklogTicket, kWorklogOwnerField);
        }
        if (!g_worklogOwnerPaneHidden) {
            TicketFieldEditorWorklog::RenderTimeTrackingModal(*app, g_worklogMutations, g_worklogTicket,
                                                              kWorklogOwnerField);
        }
        g_ui.activePaneForDraw = savedPane;
    }
    ImGui::End();
}

template <typename Pred> bool WorklogYieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

// The dialog's window while it is the topmost popup; null when it is closed or a popup of its own
// (the duration suggestions, the date picker, the templates) is open above it.
ImGuiWindow* WorklogDialogOnTop() {
    ImGuiContext& g = *GImGui;
    if (g.OpenPopupStack.Size == 0) {
        return nullptr;
    }
    ImGuiWindow* top = g.OpenPopupStack.back().Window;
    return (top != nullptr && (top->Flags & ImGuiWindowFlags_Modal) != 0) ? top : nullptr;
}

// Closes the dialog the way a finished save does and releases a held submit. Every case ends here,
// passed or failed: an open modal or a waiting worker would break the tests that follow.
void CloseWorklogDialog(ImGuiTestContext* ctx) {
    g_worklogMutations.SetHold(false);
    TicketFieldEditorWorklog::RequestCloseWorklogDialogForTests();
    if (!WorklogYieldUntil(ctx, 120, []() { return !GetWorklogDialogSnapshotForTests().Active; })) {
        ctx->LogError("worklog dialog still open after the close request");
    }
}

// Opens a fresh dialog and waits for its first draw. False (and a failed check) when the app is
// missing or the dialog never appeared.
// `focusedPaneId` / `backendKey` are the focused pane the controller latches; `drawingPaneId` is the pane
// the dialog is opened from and drawn in (empty: outside any pane), on site `drawingPaneBackendKey` (null:
// the controller knows no such pane, so it has no tracker).
bool OpenWorklogDialogForTest(ImGuiTestContext* ctx, bool keepDrafts = false, const char* backendKey = kWorklogSiteKey,
                              const char* focusedPaneId = "", const char* drawingPaneId = "",
                              const char* drawingPaneBackendKey = nullptr) {
    if (!keepDrafts) {
        smatchet::drafts::ResetForTests(); // an entry an earlier case kept must not prefill this one
    }
    if (SmatchetActiveUiTestAppController() == nullptr) {
        IM_CHECK_NO_RET(SmatchetActiveUiTestAppController() != nullptr);
        return false;
    }
    if (GetWorklogDialogSnapshotForTests().Active) {
        CloseWorklogDialog(ctx); // left open by a case that failed part-way
    }
    g_worklogMutations.Reset();
    g_worklogMutations.SetFocusedPane(focusedPaneId, backendKey);
    g_worklogMutations.ClearPaneSites();
    if (!std::string(focusedPaneId).empty()) {
        g_worklogMutations.SetPaneSite(focusedPaneId, backendKey);
    }
    if (!std::string(drawingPaneId).empty() && drawingPaneBackendKey != nullptr) {
        g_worklogMutations.SetPaneSite(drawingPaneId, drawingPaneBackendKey);
    }
    g_worklogDrawingPane = GridPane{};
    g_worklogDrawingPane.id = drawingPaneId;
    g_worklogOtherDrawingPane = GridPane{};
    g_worklogOwnerPaneHidden = false;
    g_worklogTicket = CachedTicket{};
    g_worklogTicket.id = kWorklogIssueId;
    g_worklogTicket.fieldValues["timeestimate"] = "4h";
    g_worklogOpenRequested = true;
    const bool opened = WorklogYieldUntil(
        ctx, 60, []() { return GetWorklogDialogSnapshotForTests().Active && WorklogDialogOnTop() != nullptr; });
    IM_CHECK_NO_RET(opened);
    return opened;
}

bool WaitForCaretInTimeSpent(ImGuiTestContext* ctx) {
    return WorklogYieldUntil(ctx, 30, []() { return GetWorklogDialogSnapshotForTests().TimeSpentActive; });
}

ImGuiTest* RegisterWorklogDialogTest(ImGuiTestEngine* engine, const char* name) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DurationInlineEdit", name);
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawWorklogDialogHarness(ctx); };
    return t;
}

// E. The dialog opens with the caret in Time spent and nothing on top of it, so typing lands there.
// Fails if PrimeTimeSpentFocus and the duration field's last-active storage key drift apart: the
// suggestions popup would open over the field and take the keyboard.
void RegisterWorklogOpensWithCaretInTimeSpent(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_OpensWithCaretInTimeSpent");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->Yield(6); // long enough for a suggestions popup to have opened, had the field asked for one
        IM_CHECK_NO_RET(WorklogDialogOnTop() != nullptr);
        IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().TimeSpentActive);

        ctx->KeyChars("1h");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(GetWorklogDialogSnapshotForTests().TimeSpent.c_str(), "1h");
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        CloseWorklogDialog(ctx);
    };
}

// F. Enter in Time spent on a value that does not parse: the error shows, nothing is sent, and the
// caret goes back into the field with the text still there.
void RegisterWorklogEnterOnBadTimeSpent(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EnterOnBadTimeSpent_ErrorAndCaretBack");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("0m");
        ctx->KeyPress(ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(
            ctx, 30, []() { return GetWorklogDialogSnapshotForTests().ErrorMsg == WorklogBadTimeError(); }));
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->Yield(6);
        const WorklogDialogTestSnapshot snap = GetWorklogDialogSnapshotForTests();
        IM_CHECK_NO_RET(snap.Active);
        IM_CHECK_NO_RET(snap.TimeSpentActive);
        IM_CHECK_NO_RET(WorklogDialogOnTop() != nullptr);
        IM_CHECK_STR_EQ_NO_RET(snap.TimeSpent.c_str(), "0m");
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        CloseWorklogDialog(ctx);
    };
}

// G. Enter in Time spent saves; Ctrl+Enter while that save is running sends nothing more; the dialog
// closes when the save lands.
void RegisterWorklogEnterSavesOnce(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EnterSaves_CtrlEnterWhileSavingSendsNothing");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        g_worklogMutations.SetHold(true);
        ctx->KeyChars("30m");
        ctx->KeyPress(ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 120, []() {
            return g_worklogMutations.CallCount() == 1 && GetWorklogDialogSnapshotForTests().SubmitInFlight;
        }));

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield(6);
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 1);
        IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().Active);

        g_worklogMutations.SetHold(false);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
        IM_CHECK_NO_RET(calls.size() == 1);
        if (calls.size() == 1) {
            IM_CHECK_STR_EQ_NO_RET(calls.front().IssueId.c_str(), kWorklogIssueId);
            IM_CHECK_STR_EQ_NO_RET(calls.front().TimeSpent.c_str(), "30m");
        }
        CloseWorklogDialog(ctx);
    };
}

// H. Ctrl+Enter from the description saves once, with the description as typed (no newline added).
void RegisterWorklogCtrlEnterFromDescription(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_CtrlEnterInDescription_SavesOnce");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("1h");
        ImGuiWindow* dialog = WorklogDialogOnTop();
        IM_CHECK_NO_RET(dialog != nullptr);
        if (dialog != nullptr) {
            ctx->SetRef(dialog->ID);
            ctx->ItemClick("##WorklogDesc");
            ctx->KeyChars("notes");
            ctx->Yield(2);
            IM_CHECK_STR_EQ_NO_RET(GetWorklogDialogSnapshotForTests().Description.c_str(), "notes");
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
            IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
            const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
            IM_CHECK_NO_RET(calls.size() == 1);
            if (calls.size() == 1) {
                IM_CHECK_STR_EQ_NO_RET(calls.front().TimeSpent.c_str(), "1h");
                IM_CHECK_STR_EQ_NO_RET(calls.front().Description.c_str(), "notes");
            }
        }
        CloseWorklogDialog(ctx);
    };
}

// N. Save prints its key (Ctrl+Enter) and the empty description shows its placeholder inside the box,
// which goes once something is typed (INPUT_UX_AUDIT.md #6). Fails if the dialog goes back to a plain
// Save button or a bare multiline field.
void RegisterWorklogSaveNamesItsKey(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_SaveNamesItsKey_DescriptionPlaceholder");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(smatchet::ui::KeyHintDrawnForTest("Save").c_str(), "Ctrl+Enter");
        IM_CHECK_NO_RET(smatchet::ui::PlaceholderDrawnForTest("##WorklogDesc"));
        ImGuiWindow* dialog = WorklogDialogOnTop();
        IM_CHECK_NO_RET(dialog != nullptr);
        if (dialog != nullptr) {
            ctx->SetRef(dialog->ID);
            ctx->ItemClick("##WorklogDesc");
            ctx->KeyChars("notes");
            ctx->Yield(3); // the last placeholder draw ages out of the seam's two-frame window
            IM_CHECK_STR_EQ_NO_RET(GetWorklogDialogSnapshotForTests().Description.c_str(), "notes");
            IM_CHECK_NO_RET(!smatchet::ui::PlaceholderDrawnForTest("##WorklogDesc"));
        }
        CloseWorklogDialog(ctx);
    };
}

// I. Esc on a dialog nothing was typed into closes it at once, although the caret is in a field.
void RegisterWorklogEscOnUntouchedDialog(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EscUntouched_ClosesAtOnce");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        CloseWorklogDialog(ctx);
    };
}

// J. Esc with text typed only leaves the field: the text and the dialog stay. The next Esc cancels.
void RegisterWorklogEscKeepsTypedText(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EscWithText_LeavesFieldThenCancels");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("1h");
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(4);
        const WorklogDialogTestSnapshot afterFirst = GetWorklogDialogSnapshotForTests();
        IM_CHECK_NO_RET(afterFirst.Active);
        IM_CHECK_NO_RET(!afterFirst.TimeSpentActive);
        IM_CHECK_STR_EQ_NO_RET(afterFirst.TimeSpent.c_str(), "1h");

        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        CloseWorklogDialog(ctx);
    };
}

// K. Ctrl+Enter saves even when the keyboard cursor sits on Cancel: the key must not also press the
// button under the cursor (that would close the dialog instead of showing the validation error).
void RegisterWorklogCtrlEnterDoesNotPressFocusedButton(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_CtrlEnterOnCancel_SavesInsteadOfCancelling");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("0m");
        ctx->KeyPress(ImGuiKey_Escape); // leaves the field, text kept: no item is active any more
        ctx->Yield(2);
        ImGuiWindow* dialog = WorklogDialogOnTop();
        IM_CHECK_NO_RET(dialog != nullptr);
        if (dialog != nullptr) {
            ctx->SetRef(dialog->ID);
            ctx->NavMoveTo("Cancel");
            ctx->Yield(2);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
            IM_CHECK_NO_RET(WorklogYieldUntil(
                ctx, 30, []() { return GetWorklogDialogSnapshotForTests().ErrorMsg == WorklogBadTimeError(); }));
            ctx->Yield(4);
            IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().Active);
            IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        }
        CloseWorklogDialog(ctx);
    };
}

// L. Esc with something typed into the start date (reached from the keyboard, so no calendar opens)
// only leaves that field, although Time spent and the description are still empty.
void RegisterWorklogEscKeepsTypedDate(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EscWithTypedDate_LeavesFieldThenCancels");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyPress(ImGuiKey_Tab); // out of Time spent, onto the suggestions button: no field is active
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return GImGui->ActiveId == 0; }));
        ImGuiWindow* dialog = WorklogDialogOnTop();
        IM_CHECK_NO_RET(dialog != nullptr);
        if (dialog != nullptr) {
            ctx->SetRef(dialog->ID);
            const ImGuiID dateId = ctx->GetID("##WorklogDateStarted/##FriendlyDate");
            ctx->NavMoveTo("##WorklogDateStarted/##FriendlyDate");
            ctx->NavInput(); // Enter on the keyboard cursor: the caret goes into the date field
            IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, [dateId]() { return GImGui->ActiveId == dateId; }));
            ctx->KeyChars("x");
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield(4);
            IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().Active);
            IM_CHECK_NO_RET(WorklogDialogOnTop() != nullptr);
            IM_CHECK_NO_RET(GImGui->ActiveId == 0);

            ctx->KeyPress(ImGuiKey_Escape);
            IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
            IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        }
        CloseWorklogDialog(ctx);
    };
}
// M. Cancel (or the second Esc) with something typed keeps it: the entry is stored as it is typed, and
// the next open of the same issue puts it back with a line that says so. Clear drops it.
void RegisterWorklogCancelKeepsEntry(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_EscKeepsEntry_RestoredOnReopen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("1h");
        ctx->Yield(2);
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "1h");

        ctx->KeyPress(ImGuiKey_Escape); // leaves the field
        ctx->Yield(4);
        ctx->KeyPress(ImGuiKey_Escape); // cancels
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);

        IM_CHECK_NO_RET(OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true));
        ctx->Yield(3);
        const WorklogDialogTestSnapshot reopened = GetWorklogDialogSnapshotForTests();
        IM_CHECK_STR_EQ_NO_RET(reopened.TimeSpent.c_str(), "1h");
        IM_CHECK_NO_RET(reopened.DraftRestored);

        // Clear empties the box and drops the entry.
        ImGuiWindow* dialog = WorklogDialogOnTop();
        IM_CHECK_NO_RET(dialog != nullptr);
        if (dialog != nullptr) {
            ctx->SetRef(dialog->ID);
            ctx->ItemClick("Clear");
            ctx->Yield(4);
            IM_CHECK_STR_EQ_NO_RET(GetWorklogDialogSnapshotForTests().TimeSpent.c_str(), "");
            IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        }
        CloseWorklogDialog(ctx);
    };
}

// O. The kept entry belongs to one tracker site (#2505). Two sites can have an issue with the same key:
// opening it on the other site starts empty, and a save there does not erase the first site's entry.
void RegisterWorklogKeptEntryStaysOnItsSite(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_KeptEntryStaysOnItsTrackerSite");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OpenWorklogDialogForTest(ctx)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("1h");
        ctx->Yield(2);
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "1h");
        IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", kWorklogIssueId, stored));

        ctx->KeyPress(ImGuiKey_Escape); // leaves the field
        ctx->Yield(4);
        ctx->KeyPress(ImGuiKey_Escape); // cancels
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));

        IM_CHECK_NO_RET(OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true, kWorklogOtherSiteKey));
        ctx->Yield(3);
        const WorklogDialogTestSnapshot otherSite = GetWorklogDialogSnapshotForTests();
        IM_CHECK_STR_EQ_NO_RET(otherSite.TimeSpent.c_str(), "");
        IM_CHECK_NO_RET(!otherSite.DraftRestored);

        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("30m");
        ctx->KeyPress(ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
        IM_CHECK_NO_RET(calls.size() == 1);
        if (calls.size() == 1) {
            IM_CHECK_STR_EQ_NO_RET(calls.front().TimeSpent.c_str(), "30m");
        }
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "1h");
        IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), stored));

        IM_CHECK_NO_RET(OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true, kWorklogSiteKey));
        ctx->Yield(3);
        const WorklogDialogTestSnapshot ownSite = GetWorklogDialogSnapshotForTests();
        IM_CHECK_STR_EQ_NO_RET(ownSite.TimeSpent.c_str(), "1h");
        IM_CHECK_NO_RET(ownSite.DraftRestored);
        CloseWorklogDialog(ctx);
    };
}

// P. A click in a pane that is not focused (#2505). The pane focus follows the click a frame or two late,
// so when the dialog opens the focused pane is still another pane, on another tracker site. The dialog
// acts for the pane whose cell opened it from its first draw, with no wait for the focus: it shows and
// erases that site's kept entry, never the focused pane's, and Save sends there with focus still elsewhere.
void RegisterWorklogActsForThePaneThatDrawsIt(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_ActsForThePaneThatDrawsIt");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* kClickedPane = "worklog-pane-a"; // on kWorklogSiteKey, opens and draws the dialog
        const char* kFocusedPane = "worklog-pane-b"; // on kWorklogOtherSiteKey, focused the whole time
        smatchet::drafts::ResetForTests();
        smatchet::drafts::Put("worklog.time", WorklogDraftScopeForTest(), "1h");
        smatchet::drafts::Put("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), "9h");
        if (!OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true, kWorklogOtherSiteKey, kFocusedPane, kClickedPane,
                                      kWorklogSiteKey)) {
            g_worklogDrawingPane = GridPane{};
            return;
        }
        // Site A's entry is back on the first draw, while pane B still holds focus.
        const WorklogDialogTestSnapshot opened = GetWorklogDialogSnapshotForTests();
        IM_CHECK_STR_EQ_NO_RET(opened.TimeSpent.c_str(), "1h");
        IM_CHECK_NO_RET(opened.DraftRestored);

        // The tracker drops while the dialog is up: the route must see that, not the state at the open.
        g_worklogMutations.SetConnectivity(TrackerConnectivityState::TransportDown);
        ctx->Yield(2);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
        IM_CHECK_NO_RET(calls.size() == 1);
        if (calls.size() == 1) {
            IM_CHECK_STR_EQ_NO_RET(calls.front().TimeSpent.c_str(), "1h");
            IM_CHECK_STR_EQ_NO_RET(calls.front().TargetPaneId.c_str(), kClickedPane);
            IM_CHECK_STR_EQ_NO_RET(calls.front().TargetBackendKey.c_str(), kWorklogSiteKey);
            IM_CHECK_NO_RET(calls.front().TargetConnectivity == TrackerConnectivityState::TransportDown);
        }
        g_worklogMutations.SetConnectivity(TrackerConnectivityState::AuthenticatedReachable);
        smatchet::drafts::DraftEntry stored;
        // The entry the save finished is the one it sent: site A's is erased, site B's is untouched.
        IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored));
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "9h");
        if (GetWorklogDialogSnapshotForTests().Active) {
            CloseWorklogDialog(ctx); // the owning pane must still draw it to close it
        }
        g_worklogDrawingPane = GridPane{};
    };
}

// Q. Another visible pane shows the same issue in the same column and draws it before the owning pane each
// frame. Its draw leaves the open alone: the dialog stays up, and the owning pane still saves it.
void RegisterWorklogOtherPaneDrawKeepsTheOpen(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_AnotherPaneDrawingTheIssueKeepsItOpen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* kOwnerPane = "worklog-pane-a";
        const char* kOtherPane = "worklog-pane-b";
        if (!OpenWorklogDialogForTest(ctx, /*keepDrafts=*/false, kWorklogSiteKey, kOwnerPane, kOwnerPane,
                                      kWorklogSiteKey)) {
            g_worklogDrawingPane = GridPane{};
            return;
        }
        g_worklogOtherDrawingPane = GridPane{};
        g_worklogOtherDrawingPane.id = kOtherPane;
        ctx->Yield(10);
        IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().Active);
        IM_CHECK_NO_RET(WorklogDialogOnTop() != nullptr);

        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("2h");
        ctx->KeyPress(ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
        IM_CHECK_NO_RET(calls.size() == 1);
        if (calls.size() == 1) {
            IM_CHECK_STR_EQ_NO_RET(calls.front().TimeSpent.c_str(), "2h");
            IM_CHECK_STR_EQ_NO_RET(calls.front().TargetPaneId.c_str(), kOwnerPane);
        }
        if (GetWorklogDialogSnapshotForTests().Active) {
            CloseWorklogDialog(ctx);
        }
        g_worklogOtherDrawingPane = GridPane{};
        g_worklogDrawingPane = GridPane{};
    };
}

// R. The owning pane has no tracker (no live context): the dialog fails closed. It puts back no entry, not
// even the focused pane's site's, keeps nothing of what is typed, and Save says why in the dialog and sends
// nothing.
void RegisterWorklogOwnerPaneWithoutTrackerFailsClosed(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_PaneWithoutTrackerKeepsNothingAndCannotSave");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const char* kGonePane = "worklog-pane-gone"; // the controller knows no such pane
        const char* kFocusedPane = "worklog-pane-b"; // on kWorklogOtherSiteKey
        smatchet::drafts::ResetForTests();
        smatchet::drafts::Put("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), "9h");
        if (!OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true, kWorklogOtherSiteKey, kFocusedPane, kGonePane,
                                      /*drawingPaneBackendKey=*/nullptr)) {
            g_worklogDrawingPane = GridPane{};
            return;
        }
        ctx->Yield(30); // longer than any wait for the focus: the dialog never falls back to the focused pane
        const WorklogDialogTestSnapshot opened = GetWorklogDialogSnapshotForTests();
        IM_CHECK_STR_EQ_NO_RET(opened.TimeSpent.c_str(), "");
        IM_CHECK_NO_RET(!opened.DraftRestored);

        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        ctx->KeyChars("1h");
        ctx->Yield(2);
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", std::string("|") + kWorklogIssueId, stored));
        IM_CHECK_NO_RET(!smatchet::drafts::Get("worklog.time", kWorklogIssueId, stored));
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "9h");

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield(10);
        const WorklogDialogTestSnapshot afterSave = GetWorklogDialogSnapshotForTests();
        IM_CHECK_NO_RET(afterSave.Active);
        IM_CHECK_NO_RET(!afterSave.SubmitInFlight);
        IM_CHECK_NO_RET(!afterSave.ErrorMsg.empty());
        IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 0);
        CloseWorklogDialog(ctx);
        g_worklogDrawingPane = GridPane{};
    };
}

// Whether the toast history holds a success toast for the harness's issue.
bool WorklogSavedToastShown() {
    const std::vector<ToastHistoryEntry>& history = SmatchetToastManager::Instance().History();
    for (const ToastHistoryEntry& entry : history) {
        if (entry.Type == ToastType::Success && entry.Message == kWorklogIssueId) {
            return true;
        }
    }
    return false;
}

// S. The owning pane stops drawing the cell while a save is running (the pane closed or hid, or the row left
// its grid). With another pane still drawing the issue, the open ends at once; without one, it ends when the
// save lands. Either way the outcome reaches the user as a toast, never only a dialog nobody can see.
void RunWorklogOwnerPaneStopsDrawing(ImGuiTestContext* ctx, bool otherPaneDraws) {
    const char* kOwnerPane = "worklog-pane-a";
    const char* kOtherPane = "worklog-pane-b";
    if (!OpenWorklogDialogForTest(ctx, /*keepDrafts=*/false, kWorklogSiteKey, kOwnerPane, kOwnerPane,
                                  kWorklogSiteKey)) {
        g_worklogDrawingPane = GridPane{};
        return;
    }
    if (otherPaneDraws) {
        g_worklogOtherDrawingPane = GridPane{};
        g_worklogOtherDrawingPane.id = kOtherPane;
    }
    IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
    g_worklogMutations.SetHold(true);
    ctx->KeyChars("1h");
    ctx->KeyPress(ImGuiKey_Enter);
    IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 120, []() {
        return g_worklogMutations.CallCount() == 1 && GetWorklogDialogSnapshotForTests().SubmitInFlight;
    }));
    SmatchetToastManager::Instance().ClearHistory();
    g_worklogOwnerPaneHidden = true;
    if (otherPaneDraws) {
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 10, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
    } else {
        ctx->Yield(4);
    }
    g_worklogMutations.SetHold(false);
    IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return WorklogSavedToastShown(); }));
    IM_CHECK_NO_RET(!GetWorklogDialogSnapshotForTests().Active);
    IM_CHECK_NO_RET(g_worklogMutations.CallCount() == 1);
    g_worklogOwnerPaneHidden = false;
    if (GetWorklogDialogSnapshotForTests().Active) {
        CloseWorklogDialog(ctx);
    }
    g_worklogOtherDrawingPane = GridPane{};
    g_worklogDrawingPane = GridPane{};
}

void RegisterWorklogOwnerPaneStopsDrawing(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_OwnerPaneStopsDrawing_OtherPaneEndsTheOpen");
    t->TestFunc = [](ImGuiTestContext* ctx) { RunWorklogOwnerPaneStopsDrawing(ctx, /*otherPaneDraws=*/true); };
    ImGuiTest* alone = RegisterWorklogDialogTest(engine, "WorklogDialog_OwnerPaneStopsDrawing_SaveResultIsAToast");
    alone->TestFunc = [](ImGuiTestContext* ctx) { RunWorklogOwnerPaneStopsDrawing(ctx, /*otherPaneDraws=*/false); };
}

// T. Two tracker sites share an issue key. A save running for it on site A neither blocks Save on site B nor
// holds back site B's kept entry, and site A's save landing leaves that entry alone.
void RegisterWorklogSubmitOnOneSiteLeavesTheOther(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterWorklogDialogTest(engine, "WorklogDialog_SaveOnOneSiteLeavesTheOtherSitesEntry");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        smatchet::drafts::ResetForTests();
        smatchet::drafts::Put("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), "2h");
        if (!OpenWorklogDialogForTest(ctx, /*keepDrafts=*/true, kWorklogSiteKey)) {
            return;
        }
        IM_CHECK_NO_RET(WaitForCaretInTimeSpent(ctx));
        g_worklogMutations.SetHold(true);
        ctx->KeyChars("1h");
        ctx->KeyPress(ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 120, []() {
            return g_worklogMutations.CallCount() == 1 && GetWorklogDialogSnapshotForTests().SubmitInFlight;
        }));
        TicketFieldEditorWorklog::RequestCloseWorklogDialogForTests(); // closed while site A's save runs
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 30, []() { return !GetWorklogDialogSnapshotForTests().Active; }));

        // The same issue key, now on site B; the held save on site A keeps running (no Reset here).
        g_worklogMutations.SetFocusedPane("", kWorklogOtherSiteKey);
        g_worklogOpenRequested = true;
        IM_CHECK_NO_RET(WorklogYieldUntil(
            ctx, 60, []() { return GetWorklogDialogSnapshotForTests().Active && WorklogDialogOnTop() != nullptr; }));
        ctx->Yield(2);
        const WorklogDialogTestSnapshot otherSite = GetWorklogDialogSnapshotForTests();
        IM_CHECK_NO_RET(!otherSite.SubmitInFlight);
        IM_CHECK_STR_EQ_NO_RET(otherSite.TimeSpent.c_str(), "2h");
        IM_CHECK_NO_RET(otherSite.DraftRestored);

        g_worklogMutations.SetHold(false);
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, [&stored]() {
            return !smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(), stored);
        }));
        IM_CHECK_NO_RET(smatchet::drafts::Get("worklog.time", WorklogDraftScopeForTest(kWorklogOtherSiteKey), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "2h");
        IM_CHECK_NO_RET(GetWorklogDialogSnapshotForTests().Active);
        IM_CHECK_STR_EQ_NO_RET(GetWorklogDialogSnapshotForTests().TimeSpent.c_str(), "2h");

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        IM_CHECK_NO_RET(WorklogYieldUntil(ctx, 300, []() { return !GetWorklogDialogSnapshotForTests().Active; }));
        const std::vector<RecordingWorklogMutations::Call> calls = g_worklogMutations.Calls();
        IM_CHECK_NO_RET(calls.size() == 2);
        if (calls.size() == 2) {
            IM_CHECK_STR_EQ_NO_RET(calls[0].TargetBackendKey.c_str(), kWorklogSiteKey);
            IM_CHECK_STR_EQ_NO_RET(calls[1].TargetBackendKey.c_str(), kWorklogOtherSiteKey);
            IM_CHECK_STR_EQ_NO_RET(calls[1].TimeSpent.c_str(), "2h");
        }
        if (GetWorklogDialogSnapshotForTests().Active) {
            CloseWorklogDialog(ctx);
        }
    };
}
} // namespace

extern "C" void SmatchetRegisterDurationInlineEditCommitTests(ImGuiTestEngine* engine) {
    RegisterExitEmptyUnchangedNoCommit(engine);                // A: empty unchanged exit → no PUT, ends
    RegisterChangeThenCommit(engine);                          // B: type → Enter → exactly one commit
    RegisterEscapeCancelsNoCommit(engine);                     // C: Escape → no PUT, ends
    RegisterUnchangedExistingValueNoCommit(engine);            // D: existing unchanged → no PUT, ends
    RegisterWorklogOpensWithCaretInTimeSpent(engine);          // E: worklog dialog, caret on open
    RegisterWorklogEnterOnBadTimeSpent(engine);                // F: Enter on a bad value
    RegisterWorklogEnterSavesOnce(engine);                     // G: Enter saves once
    RegisterWorklogCtrlEnterFromDescription(engine);           // H: Ctrl+Enter from the description
    RegisterWorklogEscOnUntouchedDialog(engine);               // I: Esc, nothing typed
    RegisterWorklogEscKeepsTypedText(engine);                  // J: Esc, text typed
    RegisterWorklogCtrlEnterDoesNotPressFocusedButton(engine); // K: Ctrl+Enter with the cursor on Cancel
    RegisterWorklogEscKeepsTypedDate(engine);                  // L: Esc, start date typed into
    RegisterWorklogCancelKeepsEntry(engine);                   // M: Esc Esc keeps the entry, reopen restores
    RegisterWorklogSaveNamesItsKey(engine);                    // N: Save names Ctrl+Enter, placeholder
    RegisterWorklogKeptEntryStaysOnItsSite(engine);            // O: the kept entry is per tracker site
    RegisterWorklogActsForThePaneThatDrawsIt(engine);          // P: a click in a pane focus has not reached
    RegisterWorklogOtherPaneDrawKeepsTheOpen(engine);          // Q: another pane's draw keeps the open
    RegisterWorklogOwnerPaneWithoutTrackerFailsClosed(engine); // R: a pane with no tracker fails closed
    RegisterWorklogOwnerPaneStopsDrawing(engine);              // S: the owning pane stops drawing mid-save
    RegisterWorklogSubmitOnOneSiteLeavesTheOther(engine);      // T: a save on one site, the same key on another
}

#endif // SMATCHET_BUILD_UI_TESTS
