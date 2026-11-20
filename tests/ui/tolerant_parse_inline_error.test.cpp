// tolerant_parse_inline_error.test.cpp — bucket-E (ImGui Test Engine) coverage for the parse-and-explain layer
// as the user meets it (INPUT_UX_AUDIT.md § 4.5: #38 durations, #40 dates).
//
// WHAT THIS LOCKS IN, through the real editors:
//   * The grid's inline duration cell explains text that does not read, under the field, while it is typed;
//     Enter on it keeps the cell open and commits nothing; a natural spelling ("90 m") commits the canonical
//     form ("1h 30m"); losing focus on text that does not read saves nothing and says why in a toast.
//   * The generic date field (worklog start date, annotate "before" date) reads "tomorrow", shows the date it
//     read under the field, commits it on Enter, and explains text that is not a date without touching the value.
//   * The grid's date picker has a "Type a date" row that explains an unreadable date.
//
// OBSERVATION SURFACE — what the editors do, not how they draw it: the PendingFieldEdit vector the duration cell
// appends to, the string the date field edits, the values the date picker queues, the live toasts, and
// smatchet::ui::InlineErrorTextForTests() (the last text the shared inline-error widget drew). The expected
// texts are built with the same TrackerParseExplain calls the editors use, so the cases hold in any language.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "TicketFieldEditor_detail.h" // SmatchetTest_RenderTextInlineEdit forwarder

#include "CachedTicketTypes.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h" // PendingFieldEdit
#include "SpreadsheetState.h"
#include "Tracker/TrackerDateTimePure.h"
#include "Tracker/TrackerFieldValueParseExplain.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldSchema.h"
#include "Ui/SmatchetInlineError.h"
#include "Ui/SmatchetToast.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>
#include <vector>

namespace {

using TrackerParseExplain::BareNumberUnit;
using TrackerParseExplain::DateOrder;
using TrackerParseExplain::DescribeParseMessage;
using TrackerParseExplain::ParseDateInput;
using TrackerParseExplain::ParseDurationInput;

// -----------------------------------------------------------------------------------------------------------
// The grid's inline duration cell
// -----------------------------------------------------------------------------------------------------------

struct DurationCellFixture {
    CachedTicket Ticket;
    TrackerField Field;
    SpreadsheetState State;
    std::vector<PendingFieldEdit> Edits;
    int ElsewhereClicks = 0;
};
DurationCellFixture g_cell;

const char* kCellIssue = "TOL-1";
const char* kCellField = "timeoriginalestimate"; // a duration field: the cell uses the duration widget
const char* kCellWindow = "SmatchetTest::TolerantDurationCell";

void ResetCell() {
    g_cell.Ticket = CachedTicket{};
    g_cell.Ticket.id = kCellIssue;
    g_cell.Field = TrackerField{};
    g_cell.Field.Id = kCellField;
    g_cell.Field.Name = "Original estimate";
    g_cell.State = SpreadsheetState{};
    g_cell.Edits.clear();
    g_cell.ElsewhereClicks = 0;
    smatchet::ui::ResetInlineErrorForTests();
}

bool CellEditing() { return g_cell.State.IsEditingField(kCellIssue, kCellField); }

void DrawCellHarness(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_Appearing);
    if (ImGui::Begin(kCellWindow, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (CellEditing()) {
            SmatchetTest_RenderTextInlineEdit(g_cell.Ticket, g_cell.Field, g_cell.State, g_cell.Edits);
        } else {
            ImGui::TextUnformatted("(not editing)");
        }
        if (ImGui::Button("Elsewhere##tolerant_cell")) {
            ++g_cell.ElsewhereClicks;
        }
    }
    ImGui::End();
}

// The text the cell should be explaining for `typed`, built the way the editor builds it.
std::string ExplainedDuration(const char* typed) {
    return DescribeParseMessage(ParseDurationInput(typed, BareNumberUnit::Minutes).Message);
}

// Opens the cell on an empty value and types `typed` into it.
void OpenCellAndType(ImGuiTestContext* ctx, const char* typed) {
    ResetCell();
    g_cell.State.StartEditingField(kCellIssue, kCellField, "");
    ctx->SetRef(kCellWindow);
    ctx->Yield(2);
    ctx->KeyChars(typed);
    ctx->Yield(2);
}

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

void RegisterUnreadableDurationExplainedEnterKeepsEditing(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "DurationCell_UnreadableExplained_EnterKeepsEditing");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawCellHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        OpenCellAndType(ctx, "1.5x");
        IM_CHECK_STR_EQ(g_cell.State.EditBuffer, "1.5x");

        // While typing: the reason is under the field, naming the part that is wrong.
        const std::string expected = ExplainedDuration("1.5x");
        IM_CHECK(!expected.empty());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorKindForTests().c_str(), "overlay");

        // Enter on text that does not read: the cell stays open, nothing is committed, the reason stays.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(CellEditing());
        IM_CHECK_EQ(static_cast<int>(g_cell.Edits.size()), 0);
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());

        // Put the caret back (the cell takes it on its own), replace the text, and it commits canonically.
        IM_CHECK(YieldUntil(ctx, 20, [] { return GImGui->ActiveId != 0; }));
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
        ctx->KeyChars("1.5h");
        ctx->Yield(2);
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), ""); // it reads now: no reason
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(!CellEditing());
        IM_CHECK_EQ(static_cast<int>(g_cell.Edits.size()), 1);
        if (!g_cell.Edits.empty()) {
            IM_CHECK_EQ(static_cast<int>(g_cell.Edits.front().Values.size()), 1);
            IM_CHECK_STR_EQ(g_cell.Edits.front().Values.front().c_str(), "1h 30m");
        }
    };
}

void RegisterNaturalDurationCommitsCanonical(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "DurationCell_NaturalSpellingCommitsCanonical");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawCellHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        OpenCellAndType(ctx, "90 m");
        IM_CHECK_STR_EQ(g_cell.State.EditBuffer, "90 m");
        // It reads, so no reason is shown.
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), "");
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK(!CellEditing());
        IM_CHECK_EQ(static_cast<int>(g_cell.Edits.size()), 1);
        if (!g_cell.Edits.empty() && !g_cell.Edits.front().Values.empty()) {
            // "90 m" is 90 minutes: the committed text is the spelling the tracker and the sort understand.
            IM_CHECK_STR_EQ(g_cell.Edits.front().Values.front().c_str(), "1h 30m");
        }
    };
}

void RegisterFocusLossOnUnreadableToasts(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "DurationCell_FocusLossOnUnreadable_NothingSavedAndToast");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawCellHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        OpenCellAndType(ctx, "3x");
        // Click another cell until the edit ends (the suggestions popup may eat the first click).
        for (int i = 0; i < 12 && CellEditing(); ++i) {
            ctx->MouseMove("Elsewhere##tolerant_cell");
            ctx->MouseClick(0);
            ctx->Yield(2);
        }
        IM_CHECK(!CellEditing());
        IM_CHECK_EQ(static_cast<int>(g_cell.Edits.size()), 0); // nothing was saved
        const std::string title = SmatchetLocalization::T("field_editor.duration_not_saved", "Time not saved");
        const std::string body = std::string("Original estimate: ") + ExplainedDuration("3x");
        IM_CHECK(SmatchetToastManager::Instance().HasLive(title, body)); // and the user is told why
    };
}

// -----------------------------------------------------------------------------------------------------------
// The generic date field
// -----------------------------------------------------------------------------------------------------------

std::string g_dateValue;
int g_dateChanges = 0;
const char* kDateWindow = "SmatchetTest::TolerantDate";
const char* kDateFieldRef = "##TolerantDate/##FriendlyDate";

void DrawDateHarness(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(520, 160), ImGuiCond_Appearing);
    if (ImGui::Begin(kDateWindow, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (TrackerDateTimeFieldEditor::RenderGenericDatePicker("##TolerantDate", g_dateValue, false, 340.0f)) {
            ++g_dateChanges;
        }
        ImGui::Button("Elsewhere##tolerant_date");
    }
    ImGui::End();
}

// Today on the user's calendar, as the field reads it.
TrackerParseExplain::CivilDate LocalToday() {
    TrackerParseExplain::CivilDate today;
    TrackerDateTimePure::TodayLocalYmd(today.Year, today.Month, today.Day);
    return today;
}

// The caret goes into the date field from the keyboard: a click on it also opens the calendar popup.
bool FocusDateField(ImGuiTestContext* ctx) {
    ctx->SetRef(kDateWindow);
    const ImGuiID id = ctx->GetID(kDateFieldRef);
    ctx->NavMoveTo(kDateFieldRef);
    ctx->NavInput(); // Enter on the keyboard cursor: the caret goes into the field
    return YieldUntil(ctx, 20, [id] { return GImGui->ActiveId == id; });
}

void ResetDate(const char* value) {
    g_dateValue = value;
    g_dateChanges = 0;
    smatchet::ui::ResetInlineErrorForTests();
}

void RegisterDateTomorrowPreviewsAndCommits(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "DateField_TomorrowPreviewedAndCommittedOnEnter");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDateHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetDate("2026-01-01");
        ctx->Yield(2);
        IM_CHECK(FocusDateField(ctx));
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
        ctx->KeyChars("tomorrow");
        ctx->Yield(2);

        // While typing: the date it read, under the field. Nothing is committed yet.
        const TrackerParseExplain::CivilDate tomorrow = TrackerParseExplain::AddDays(LocalToday(), 1);
        const std::string iso = TrackerParseExplain::FormatIsoDate(tomorrow);
        const std::string preview = SmatchetLocalization::Format("parse.date.preview", "= %s", iso.c_str());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), preview.c_str());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorKindForTests().c_str(), "hint");
        IM_CHECK_STR_EQ(g_dateValue.c_str(), "2026-01-01");

        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_STR_EQ(g_dateValue.c_str(), iso.c_str()); // the date field now holds tomorrow
        IM_CHECK(g_dateChanges >= 1);
    };
}

void RegisterDateUnreadableExplainedValueKept(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "DateField_UnreadableExplained_ValueKept");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDateHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetDate("2026-01-01");
        ctx->Yield(2);
        IM_CHECK(FocusDateField(ctx));
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
        ctx->KeyChars("32/13");
        ctx->Yield(2);

        const std::string expected =
            DescribeParseMessage(ParseDateInput("32/13", LocalToday(), DateOrder::MonthFirst).Message);
        IM_CHECK(!expected.empty());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorKindForTests().c_str(), "overlay");

        // Enter on it changes nothing, and the reason is still there.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_STR_EQ(g_dateValue.c_str(), "2026-01-01");
        IM_CHECK_EQ(g_dateChanges, 0);
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());

        // The caret comes back by itself, with the same text still in the box and still explained.
        const ImGuiID fieldId = ctx->GetID(kDateFieldRef);
        IM_CHECK(YieldUntil(ctx, 30, [fieldId] { return GImGui->ActiveId == fieldId; }));
        ctx->Yield(3);
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());

        // Clicking away from it now says why the date did not change, and leaves the value alone: it must not
        // silently snap back to the stored date as if nothing had been typed.
        ctx->MouseMove("Elsewhere##tolerant_date");
        ctx->MouseClick(0);
        ctx->Yield(3);
        IM_CHECK_STR_EQ(g_dateValue.c_str(), "2026-01-01");
        IM_CHECK_EQ(g_dateChanges, 0);
        const std::string title = SmatchetLocalization::T("date_field.not_changed", "Date not changed");
        IM_CHECK(SmatchetToastManager::Instance().HasLive(title, expected));
    };
}

// -----------------------------------------------------------------------------------------------------------
// The grid's date picker: its "Type a date" row
// -----------------------------------------------------------------------------------------------------------

struct DatePickerFixture {
    CachedTicket Ticket;
    TrackerField Field;
    SpreadsheetState State;
    std::vector<std::vector<std::string>> Queued;
};
DatePickerFixture g_picker;
const char* kPickerWindow = "SmatchetTest::TolerantDatePicker";

void DrawPickerHarness(ImGuiTestContext*) {
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_Appearing);
    if (ImGui::Begin(kPickerWindow, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (g_picker.State.IsEditingField(g_picker.Ticket.id, g_picker.Field.Id)) {
            TrackerDateTimeFieldEditor::RenderDateTimeFieldEditor(
                g_picker.Ticket, g_picker.Field, "2026-01-01", g_picker.State,
                [](const std::string&, const TrackerField&, const std::vector<std::string>& values) {
                    g_picker.Queued.push_back(values);
                },
                "compact", 21, true);
        } else {
            ImGui::TextUnformatted("(not editing)");
        }
    }
    ImGui::End();
}

// The modal picker while it is the topmost popup, or null.
ImGuiWindow* PickerOnTop() {
    ImGuiContext& g = *GImGui;
    if (g.OpenPopupStack.Size == 0) {
        return nullptr;
    }
    ImGuiWindow* top = g.OpenPopupStack.back().Window;
    return (top != nullptr && (top->Flags & ImGuiWindowFlags_Modal) != 0) ? top : nullptr;
}

void RegisterGridDatePickerTypedRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "TolerantParse", "GridDatePicker_TypedRowExplainsAndApplies");
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawPickerHarness(ctx); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        g_picker.Ticket = CachedTicket{};
        g_picker.Ticket.id = "TOL-2";
        g_picker.Field = TrackerField{};
        g_picker.Field.Id = "duedate";
        g_picker.Field.Name = "Due date";
        g_picker.Field.Type = "date";
        g_picker.State = SpreadsheetState{};
        g_picker.Queued.clear();
        smatchet::ui::ResetInlineErrorForTests();
        g_picker.State.StartEditingField(g_picker.Ticket.id, g_picker.Field.Id, "2026-01-01");

        IM_CHECK(YieldUntil(ctx, 30, [] { return PickerOnTop() != nullptr; }));
        ImGuiWindow* picker = PickerOnTop();
        IM_CHECK(picker != nullptr);
        if (picker == nullptr) {
            return;
        }
        ctx->SetRef(picker->ID);

        // A date that is not on the calendar: the reason shows under the row and Enter applies nothing.
        ctx->ItemClick("##typedDate");
        ctx->KeyChars("32/13");
        ctx->Yield(2);
        const std::string expected =
            DescribeParseMessage(ParseDateInput("32/13", LocalToday(), DateOrder::MonthFirst).Message);
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorTextForTests().c_str(), expected.c_str());
        IM_CHECK_STR_EQ(smatchet::ui::InlineErrorKindForTests().c_str(), "error");
        // Apply is off while the text does not read, so a half-typed date is never applied by accident.
        IM_CHECK((ctx->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) != 0);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(g_picker.Queued.size()), 0);
        IM_CHECK(g_picker.State.IsEditingField(g_picker.Ticket.id, g_picker.Field.Id));

        // A date that reads: Enter applies it, as the Apply button would.
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
        ctx->KeyChars("2026-12-24");
        ctx->Yield(2);
        IM_CHECK((ctx->ItemInfo("Apply").ItemFlags & ImGuiItemFlags_Disabled) == 0);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(3);
        IM_CHECK_EQ(static_cast<int>(g_picker.Queued.size()), 1);
        if (!g_picker.Queued.empty() && !g_picker.Queued.front().empty()) {
            IM_CHECK_STR_EQ(g_picker.Queued.front().front().c_str(), "2026-12-24");
        }
        IM_CHECK(!g_picker.State.IsEditingField(g_picker.Ticket.id, g_picker.Field.Id));
    };
}

} // namespace

extern "C" void SmatchetRegisterTolerantParseInlineErrorTests(ImGuiTestEngine* engine) {
    RegisterUnreadableDurationExplainedEnterKeepsEditing(engine); // 1: grid duration cell, unreadable then fixed
    RegisterNaturalDurationCommitsCanonical(engine);              // 2: "90 m" commits "1h 30m"
    RegisterFocusLossOnUnreadableToasts(engine);                  // 3: click away on "3x": nothing saved, toast
    RegisterDateTomorrowPreviewsAndCommits(engine);               // 4: generic date field, "tomorrow"
    RegisterDateUnreadableExplainedValueKept(engine);             // 5: generic date field, "32/13"
    RegisterGridDatePickerTypedRow(engine);                       // 6: grid date picker "Type a date" row
}

#endif // SMATCHET_BUILD_UI_TESTS
