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
// OBSERVATION SURFACE — pendingEdits (std::vector<PendingFieldEdit>), the caller-owned sink
// RenderTextInlineEdit::QueueEdit appends to. A spurious commit = a non-empty vector; a correct
// no-op = an empty vector. Deterministic, in-process, no network, no golden image.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "TicketFieldEditor_detail.h" // SmatchetTest_RenderTextInlineEdit forwarder

#include "CachedTicketTypes.h"  // CachedTicket
#include "SmatchetUiSession.h"  // PendingFieldEdit
#include "SpreadsheetState.h"   // SpreadsheetState
#include "TrackerFieldSchema.h" // TrackerField

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>
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
// engine (see the residue note in the test-author report: focus-loss-commit for duration is the
// one uncovered gesture).
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

// NOTE on the two duration gestures NOT covered as bucket-E here (see test-author residue report):
//   - Pure FOCUS-LOSS commit of a *changed* duration value, and
//   - In-place EDIT (backspace/replace) of an existing duration value,
// both fight the duration widget's per-frame focus self-management (needRepositionAndFocus +
// type-to-edit splice + deselect-on-open caret reset) when driven by synthetic engine input, so
// they cannot be driven deterministically without product changes. The COMMIT path itself is
// proven by case B (type → Enter), and the no-op/no-PUT + session-end invariants (the three fixed
// bugs) are fully covered by A/C/D. The pure DECISION logic for those gestures is already covered
// by tests/Core/TicketFieldEditorCommitPolicyPure.test.cpp.

} // namespace

extern "C" void SmatchetRegisterDurationInlineEditCommitTests(ImGuiTestEngine* engine) {
    RegisterExitEmptyUnchangedNoCommit(engine);     // A: empty unchanged exit → no PUT, ends
    RegisterChangeThenCommit(engine);               // B: type → Enter → exactly one commit
    RegisterEscapeCancelsNoCommit(engine);          // C: Escape → no PUT, ends
    RegisterUnchangedExistingValueNoCommit(engine); // D: existing unchanged → no PUT, ends
}

#endif // SMATCHET_BUILD_UI_TESTS
