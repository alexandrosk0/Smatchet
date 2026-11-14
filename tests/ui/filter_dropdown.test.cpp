// filter_dropdown.test.cpp — bucket-E (ImGui Test Engine) coverage for the shared dropdown body
// (smatchet::dropdown::DrawBody, FilterDropdown.cpp) that every tracker value picker draws.
//
// WHAT THIS LOCKS IN — the behaviour a row-model unit test cannot see, because it only exists once the
// real ImGui focus / input-queue / popup machinery runs:
//   1. Typed characters ALWAYS reach the filter: after the popup opens, after a row click, after
//      wheel-scrolling the list, and while the scrollbar is being dragged.
//   2. Up / Down move the highlight and Enter acts on it; a single-select pick closes the popup.
//   3. A tick-box (multi-select) pick keeps the popup open.
//   4. A pick shows up in the pinned Recent section on the next open, and that section is hidden
//      while a filter is typed.
//   5. Escape closes the popup without a pick.
//   6. Unticking a value records nothing; the 'Add "<text>"' row creates, records and clears the filter.
//   7. Enter acts on the option that matches the filter, never on a ticked option the filter only keeps
//      listed, and never on the clear row of a picker with no options (which shows its empty-state text).
//   8. A recent pick outside the option list can be picked; PageDown moves a page.
//   9. Options that arrive while the popup is open: a list that was empty gets its default highlight
//      (also after an arrow key found nothing to move to), and a highlight the user moved stays on its
//      option when rows are inserted above it.
//  10. The Enter press that opens a keyboard-focused combo only opens it; the next Enter picks.
//
// COMPLEMENTS tests/Core/FilterDropdownPure.test.cpp (row model, highlight moves, scroll maths) and
// tests/Core/RecentSelections.test.cpp (the store). THIS suite drives the production DrawBody inside a
// real BeginCombo popup, so the focus hand-off, the character splice and the popup close are the real
// code. The fixture owns only the option list and the event log.
//
// OBSERVATION SURFACE — the Event DrawBody returns each frame (logged by the GuiFunc) plus whether the
// combo popup is still open. Deterministic, in-process, no network, no golden image.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "FilterDropdown.h"
#include "RecentSelections.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

const char* kWindowRef = "SmatchetTest::FilterDropdown";
const char* kComboRef = "##picker";
const char* kRecentsScope = "uitest|filter_dropdown";

// Twelve options: one more than the ten the Recent section needs. Filter strings used below match
// exactly one label ("io", "kap") except "a", whose first match in list order is "alpha"; a lone "o"
// (what "io" degrades to when a key is lost) first matches "epsilon", a lone "i" first matches "epsilon" too.
const char* const kLabels[] = {"alpha", "beta",  "gamma", "delta", "epsilon", "zeta",
                               "eta",   "theta", "iota",  "kappa", "lambda",  "omicron"};
const int kLabelCount = static_cast<int>(sizeof(kLabels) / sizeof(kLabels[0]));

// Per-test fixture, reached through ImGuiTest::UserData. GuiFunc and TestFunc run on the same thread in
// alternating frames, so no synchronisation is needed.
struct DropdownFixture {
    bool multiSelect = false;
    bool allowCreate = false;
    bool keepCheckedVisible = false;
    bool showClear = false;
    bool showUnknownRecents = false;
    bool emptyTextBlank = false; // "" instead of the default "No options"
    int recentsMinOptions = 10;
    int skipFirst = 0; // the picker lists kLabels[skipFirst .. itemCount)
    int itemCount = kLabelCount;
    bool checked[kLabelCount] = {};
    bool comboOpen = false;
    float popupContentHeight = 0.0f;   // the popup's content height, while it is open
    ImGuiWindow* rowsWindow = nullptr; // the scrolling list inside the popup, while it is open
    std::vector<smatchet::dropdown::Event> events;
};

DropdownFixture g_fixture;

// Fresh fixture and an empty, non-persisting recents store (nothing here may reach a config file; the
// store a live session had is put back by the test's TeardownFunc). A test sets the fixture's other
// switches after this call, before it opens the combo.
void ResetFixture(bool multiSelect) {
    g_fixture = DropdownFixture();
    g_fixture.multiSelect = multiSelect;
    smatchet::recents::BeginSharedTestScope();
}

// GuiFunc — a combo whose popup body is THE PRODUCTION CALL, the way every picker hosts it.
void DrawDropdownHarness(ImGuiTestContext* ctx) {
    auto* s = static_cast<DropdownFixture*>(ctx->Test->UserData);
    s->comboOpen = false;
    s->rowsWindow = nullptr;
    ImGui::SetNextWindowSize(ImVec2(460, 120), ImGuiCond_Appearing);
    if (ImGui::Begin(kWindowRef, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::BeginCombo(kComboRef, "(choose)", ImGuiComboFlags_HeightLargest)) {
            s->comboOpen = true;
            // A test may change skipFirst / itemCount while the popup is open: options arriving late.
            smatchet::dropdown::Item items[kLabelCount];
            const int listed = s->itemCount > s->skipFirst ? s->itemCount - s->skipFirst : 0;
            for (int i = 0; i < listed; ++i) {
                items[i].Id = kLabels[s->skipFirst + i];
                items[i].Label = kLabels[s->skipFirst + i];
                items[i].Checked = s->checked[s->skipFirst + i];
            }
            smatchet::dropdown::Options options;
            options.MultiSelect = s->multiSelect;
            options.KeepCheckedVisible = s->keepCheckedVisible;
            options.AllowCreate = s->allowCreate;
            options.ClearLabel = s->showClear ? "<clear>" : nullptr;
            options.RecentsScope = kRecentsScope;
            options.RecentsMinOptions = s->recentsMinOptions;
            options.RecentsShowUnknown = s->showUnknownRecents;
            options.EmptyText = s->emptyTextBlank ? "" : nullptr;
            const smatchet::dropdown::Event ev = smatchet::dropdown::DrawBody(items, listed, options);
            if (ev.Kind != smatchet::dropdown::EventKind::None) {
                s->events.push_back(ev);
                const bool toggles = s->multiSelect && ev.Kind == smatchet::dropdown::EventKind::Picked;
                if (toggles && ev.ItemIndex >= 0 && ev.ItemIndex < listed) {
                    s->checked[s->skipFirst + ev.ItemIndex] = !s->checked[s->skipFirst + ev.ItemIndex];
                }
            }
            const ImGuiWindow* popup = ImGui::GetCurrentWindow();
            s->popupContentHeight = popup->ContentSize.y;
            for (ImGuiWindow* child : popup->DC.ChildWindows) {
                if (std::strstr(child->Name, "##dropdown_rows") != nullptr) {
                    s->rowsWindow = child;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::End();
}

void OpenCombo(ImGuiTestContext* ctx) {
    ctx->SetRef(kWindowRef);
    ctx->ItemClick(kComboRef);
    ctx->Yield(2);
}

int EventCount() { return static_cast<int>(g_fixture.events.size()); }

// The id of the n-th logged event, or "" when there is none.
const char* EventId(int n) {
    return (n >= 0 && n < EventCount()) ? g_fixture.events[static_cast<size_t>(n)].Id.c_str() : "";
}

bool EventIsPick(int n) {
    return n >= 0 && n < EventCount() &&
           g_fixture.events[static_cast<size_t>(n)].Kind == smatchet::dropdown::EventKind::Picked;
}

ImGuiTest* RegisterDropdownTest(ImGuiTestEngine* engine, const char* name) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "FilterDropdown", name);
    t->UserData = &g_fixture;
    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawDropdownHarness(ctx); };
    // Runs after TestFunc whether it passed or not: the session's own recent picks come back.
    t->TeardownFunc = [](ImGuiTestContext*) { smatchet::recents::EndSharedTestScope(); };
    return t;
}

// ============================================================================
// A. Type straight after opening → the filter takes it → Enter picks the match and closes.
// ============================================================================
void RegisterTypeThenEnterPicks(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "TypeOnOpen_FiltersAndEnterPicks");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.comboOpen);

        // No click on the filter field: the body takes the keys by itself.
        ctx->KeyChars("io"); // losing either key leaves a filter whose first match is not "iota"
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);

        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK(EventIsPick(0));
        IM_CHECK_STR_EQ(EventId(0), "iota");
        IM_CHECK(!g_fixture.comboOpen); // a single-select pick closes the popup
    };
}

// ============================================================================
// B. Arrow keys move the highlight; Enter acts on the highlighted row.
// ============================================================================
void RegisterArrowsThenEnterPicks(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "ArrowKeys_MoveHighlight_EnterPicks");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);

        // Highlight starts on the first row ("alpha"); two steps down is "gamma", one back up "beta".
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->KeyPress(ImGuiKey_UpArrow);
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);

        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "beta");
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// C. Tick-box mode: a row click toggles and keeps the popup open; keys typed after the click still
//    reach the filter.
// ============================================================================
void RegisterTypeAfterRowClick(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "MultiSelect_TypeAfterRowClick_ReachesFilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/true);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.rowsWindow != nullptr);
        if (g_fixture.rowsWindow == nullptr) {
            return;
        }

        // Row 1 is "beta" (no clear row, no Recent section yet).
        ctx->SetRef(g_fixture.rowsWindow);
        ctx->ItemClick("$$1/##row");
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "beta");
        IM_CHECK(g_fixture.checked[1]);
        IM_CHECK(g_fixture.comboOpen); // a tick-box pick keeps the popup open

        // The click took keyboard focus off the filter field; typing must land in it anyway.
        // "io": losing the first key, or the second key replacing the first, would leave "o", whose
        // first match is "epsilon".
        ctx->KeyChars("io");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 2);
        IM_CHECK_STR_EQ(EventId(1), "iota");
        IM_CHECK(g_fixture.checked[8]);
        IM_CHECK(g_fixture.comboOpen);

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// D. Wheel-scroll the list, then type → the filter takes it.
// ============================================================================
void RegisterTypeWhileScrolling(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "TypeAfterWheelScroll_ReachesFilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.rowsWindow != nullptr);
        if (g_fixture.rowsWindow == nullptr) {
            return;
        }

        const ImRect list = g_fixture.rowsWindow->Rect();
        ctx->MouseMoveToPos(list.GetCenter());
        ctx->MouseWheelY(-2.0f);
        ctx->Yield(2);
        IM_CHECK(g_fixture.rowsWindow != nullptr && g_fixture.rowsWindow->Scroll.y > 0.0f); // it did scroll

        ctx->KeyChars("io"); // see C for why "io"
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);

        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "iota");
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// G. Keys typed WHILE the scrollbar is held down reach the filter.
// ============================================================================
// The one case only the character splice can serve: with a mouse button down the filter field cannot
// take focus back, so without the splice these keys are dropped. (Cases A / C / D pass through the
// refocus alone.)
void RegisterTypeWhileScrollbarHeld(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "TypeWhileScrollbarHeld_ReachesFilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.rowsWindow != nullptr);
        if (g_fixture.rowsWindow == nullptr) {
            return;
        }
        IM_CHECK(g_fixture.rowsWindow->ScrollbarY); // twelve rows, ten visible
        const ImRect bar = ImGui::GetWindowScrollbarRect(g_fixture.rowsWindow, ImGuiAxis_Y);
        ctx->MouseMoveToPos(bar.GetCenter());
        ctx->MouseDown(0);
        ctx->Yield(2);
        IM_CHECK(ctx->UiContext->ActiveId != 0); // the scrollbar holds the mouse, not the filter field

        ctx->KeyChars("io");
        ctx->Yield(2);
        ctx->MouseUp(0);
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);

        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "iota");
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// E. A pick is pinned in the Recent section on the next open; typing a filter hides that section.
// ============================================================================
void RegisterRecentSection(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "RecentSection_PinnedAfterPick_HiddenWhileFiltering");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);

        // First open: no recents yet. Pick "kappa" through the filter.
        OpenCombo(ctx);
        ctx->KeyChars("kap");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "kappa");
        const std::vector<std::string> recents = smatchet::recents::SharedGet(kRecentsScope);
        IM_CHECK_EQ(static_cast<int>(recents.size()), 1);

        // Second open: the first selectable row is now the pinned recent, not "alpha".
        OpenCombo(ctx);
        IM_CHECK(g_fixture.comboOpen);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 2);
        IM_CHECK_STR_EQ(EventId(1), "kappa");

        // Third open: "a" matches "kappa" too, but with the Recent section hidden the first match in
        // list order is "alpha".
        OpenCombo(ctx);
        ctx->KeyChars("a");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 3);
        IM_CHECK_STR_EQ(EventId(2), "alpha");

        // Newest first, no duplicate for the repeated "kappa" pick.
        const std::vector<std::string> after = smatchet::recents::SharedGet(kRecentsScope);
        IM_CHECK_EQ(static_cast<int>(after.size()), 2);
        if (after.size() == 2) {
            IM_CHECK_STR_EQ(after[0].c_str(), "alpha");
            IM_CHECK_STR_EQ(after[1].c_str(), "kappa");
        }
        smatchet::recents::BeginSharedTestScope();
    };
}

// ============================================================================
// F. Escape closes the popup with no pick and records nothing.
// ============================================================================
void RegisterEscapeCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "Escape_ClosesWithoutPick");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.comboOpen);

        ctx->KeyChars("zzz"); // a filter with no match must not get in Escape's way
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);

        IM_CHECK(!g_fixture.comboOpen);
        IM_CHECK_EQ(EventCount(), 0);
        IM_CHECK(smatchet::recents::SharedGet(kRecentsScope).empty());
    };
}

// ============================================================================
// H. Tick-box mode records a value when it is ticked ON; unticking records nothing.
// ============================================================================
void RegisterUntickRecordsNothing(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "MultiSelect_UntickRecordsNothing");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/true);
        OpenCombo(ctx);
        IM_CHECK(g_fixture.rowsWindow != nullptr);
        if (g_fixture.rowsWindow == nullptr) {
            return;
        }
        ctx->SetRef(g_fixture.rowsWindow);
        ctx->ItemClick("$$1/##row"); // tick "beta"
        ctx->Yield(2);
        ctx->ItemClick("$$8/##row"); // tick "iota"
        ctx->Yield(2);
        ctx->ItemClick("$$1/##row"); // untick "beta"
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 3);
        IM_CHECK(!g_fixture.checked[1]);
        IM_CHECK(g_fixture.checked[8]);

        // Had the untick been recorded, "beta" would be the newest entry.
        const std::vector<std::string> recents = smatchet::recents::SharedGet(kRecentsScope);
        IM_CHECK_EQ(static_cast<int>(recents.size()), 2);
        if (recents.size() == 2) {
            IM_CHECK_STR_EQ(recents[0].c_str(), "iota");
            IM_CHECK_STR_EQ(recents[1].c_str(), "beta");
        }
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        smatchet::recents::BeginSharedTestScope();
    };
}

// ============================================================================
// I. The 'Add "<text>"' row: Enter creates the trimmed text, records it, and clears the filter.
// ============================================================================
void RegisterCreateRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "Create_TrimsRecordsAndClearsFilter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/true);
        g_fixture.allowCreate = true;
        OpenCombo(ctx);

        ctx->KeyChars(" New "); // matches no option: the only row is the create row
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        if (EventCount() == 1) {
            IM_CHECK(g_fixture.events[0].Kind == smatchet::dropdown::EventKind::Created);
            IM_CHECK_EQ(g_fixture.events[0].ItemIndex, -1);
        }
        IM_CHECK_STR_EQ(EventId(0), "New");
        IM_CHECK(g_fixture.comboOpen);
        const std::vector<std::string> recents = smatchet::recents::SharedGet(kRecentsScope);
        IM_CHECK_EQ(static_cast<int>(recents.size()), 1);
        if (recents.size() == 1) {
            IM_CHECK_STR_EQ(recents[0].c_str(), "New");
        }

        // The filter was cleared: "io" now finds "iota". Left in place it would read "Newio" and create again.
        ctx->KeyChars("io");
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 2);
        IM_CHECK(EventIsPick(1));
        IM_CHECK_STR_EQ(EventId(1), "iota");

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        smatchet::recents::BeginSharedTestScope();
    };
}

// ============================================================================
// J. A ticked option that a filter keeps listed never takes Enter from the option that matches.
// ============================================================================
void RegisterKeptTickedDoesNotTakeEnter(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "KeepCheckedVisible_EnterActsOnTheMatch");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/true);
        g_fixture.keepCheckedVisible = true;
        g_fixture.checked[0] = true; // "alpha" is ticked and sorts above every match
        OpenCombo(ctx);

        ctx->KeyChars("io"); // rows: alpha (kept), iota
        ctx->Yield(2);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "iota");
        IM_CHECK(g_fixture.checked[0]); // "alpha" was not unticked
        IM_CHECK(g_fixture.checked[8]);

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        smatchet::recents::BeginSharedTestScope();
    };
}

// ============================================================================
// K. A recent pick that is not among the options (the list has not loaded) can still be picked.
// ============================================================================
void RegisterUnknownRecentPick(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "UnknownRecent_PickedWithNoOptions");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        g_fixture.itemCount = 0;
        g_fixture.recentsMinOptions = -1;
        g_fixture.showUnknownRecents = true;
        smatchet::recents::RecordAndPersist(kRecentsScope, "PROJ"); // persistence is off after the reset
        OpenCombo(ctx);
        IM_CHECK(g_fixture.rowsWindow != nullptr); // the Recent section is listed with no options

        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK(EventIsPick(0));
        IM_CHECK_STR_EQ(EventId(0), "PROJ");
        if (EventCount() == 1) {
            IM_CHECK_EQ(g_fixture.events[0].ItemIndex, -1);
        }
        IM_CHECK(!g_fixture.comboOpen);
        smatchet::recents::BeginSharedTestScope();
    };
}

// ============================================================================
// L. PageDown moves the highlight a page (ten rows).
// ============================================================================
void RegisterPageDown(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "PageDown_MovesAPage");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        OpenCombo(ctx);

        ctx->KeyPress(ImGuiKey_PageDown); // "alpha" (row 0) + ten rows
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "lambda");
    };
}

// ============================================================================
// M. A picker with a clear row and no options: the empty-state text is shown under the clear row,
//    Enter does not clear the field, and the clear row is still one arrow key away.
// ============================================================================
void RegisterLoneClearRow(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "LoneClearRow_ShowsEmptyState_EnterDoesNotClear");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        g_fixture.itemCount = 0;
        g_fixture.showClear = true;

        // Without the empty-state text first, for the height to compare against.
        g_fixture.emptyTextBlank = true;
        OpenCombo(ctx);
        ctx->Yield(3);
        const float heightWithoutText = g_fixture.popupContentHeight;
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
        IM_CHECK(!g_fixture.comboOpen);

        g_fixture.emptyTextBlank = false;
        OpenCombo(ctx);
        ctx->Yield(3);
        IM_CHECK(g_fixture.rowsWindow != nullptr); // the clear row is listed
        IM_CHECK_GT(heightWithoutText, 0.0f);
        IM_CHECK_GT(g_fixture.popupContentHeight, heightWithoutText + 4.0f); // "No options" takes a line

        ctx->KeyPress(ImGuiKey_Enter); // nothing is highlighted: the clear row is never the default
        ctx->Yield(3);
        IM_CHECK_EQ(EventCount(), 0);
        IM_CHECK(g_fixture.comboOpen);

        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        if (EventCount() == 1) {
            IM_CHECK(g_fixture.events[0].Kind == smatchet::dropdown::EventKind::Cleared);
        }
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// N. The option list arrives while the popup is open: the first option becomes the highlight, even
//    though an arrow key was pressed while there was nothing to move to.
// ============================================================================
void RegisterOptionsArriveLate(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "OptionsArriveLate_DefaultHighlightApplies");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        g_fixture.itemCount = 0;
        OpenCombo(ctx);
        IM_CHECK(g_fixture.comboOpen);
        ctx->KeyPress(ImGuiKey_DownArrow); // no rows yet
        ctx->Yield(2);

        g_fixture.itemCount = kLabelCount; // the fetch lands
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "alpha");
        IM_CHECK(!g_fixture.comboOpen);
    };
}

// ============================================================================
// O. Rows inserted above a highlight the user moved: the highlight stays on its option.
// ============================================================================
void RegisterHighlightFollowsItsOption(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "RowsInsertedAbove_HighlightStaysOnItsOption");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        g_fixture.skipFirst = 2; // gamma, delta, ...
        OpenCombo(ctx);
        ctx->KeyPress(ImGuiKey_DownArrow); // gamma → delta (row 1)
        ctx->Yield(2);

        g_fixture.skipFirst = 0; // "alpha" and "beta" arrive above: row 1 is now "beta"
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "delta");
    };
}

// ============================================================================
// P. Enter on the keyboard-focused combo opens the popup. That same key press must not also act on
//    the highlighted row (it would pick it and close the popup at once); the next Enter does.
// ============================================================================
void RegisterEnterThatOpensDoesNotPick(ImGuiTestEngine* engine) {
    ImGuiTest* t = RegisterDropdownTest(engine, "EnterThatOpens_DoesNotPick");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        ResetFixture(/*multiSelect=*/false);
        ctx->SetRef(kWindowRef);
        ctx->NavMoveTo(kComboRef);
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK(g_fixture.comboOpen);
        IM_CHECK_EQ(EventCount(), 0);

        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(EventCount(), 1);
        IM_CHECK_STR_EQ(EventId(0), "alpha");
        IM_CHECK(!g_fixture.comboOpen);
    };
}

} // namespace

extern "C" void SmatchetRegisterFilterDropdownTests(ImGuiTestEngine* engine) {
    RegisterTypeThenEnterPicks(engine);         // A: type on open → Enter picks, closes
    RegisterArrowsThenEnterPicks(engine);       // B: Up / Down + Enter
    RegisterTypeAfterRowClick(engine);          // C: tick-box click keeps open; typing still filters
    RegisterTypeWhileScrolling(engine);         // D: wheel-scroll then type
    RegisterRecentSection(engine);              // E: Recent section pinned, hidden while filtering
    RegisterEscapeCloses(engine);               // F: Escape closes, no pick
    RegisterTypeWhileScrollbarHeld(engine);     // G: keys typed during a scrollbar drag
    RegisterUntickRecordsNothing(engine);       // H: unticking is not a recent pick
    RegisterCreateRow(engine);                  // I: the 'Add "<text>"' row
    RegisterKeptTickedDoesNotTakeEnter(engine); // J: Enter acts on the match, not a kept ticked option
    RegisterUnknownRecentPick(engine);          // K: a recent pick outside the options
    RegisterPageDown(engine);                   // L: PageDown
    RegisterLoneClearRow(engine);               // M: empty state under a lone clear row
    RegisterOptionsArriveLate(engine);          // N: options arriving under an open popup
    RegisterHighlightFollowsItsOption(engine);  // O: rows inserted above the highlight
    RegisterEnterThatOpensDoesNotPick(engine);  // P: the Enter that opens the combo is not a pick
}

#endif // SMATCHET_BUILD_UI_TESTS
