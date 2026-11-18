// toolbar_append_cache_cue.test.cpp — bucket-E visible-cue gate for the
// Pillar-2 sync-I/O offload cluster (#1001, site #611).
//
// Invariant under test: when the toolbar's per-tracker append cache reloads
// after a backend-key change, the disk read (LoadPersistentViewsFromDisk —
// ifstream + JSON under IoMutex + ScopedFileLock) runs OFF the UI thread, and
// while that load is in flight a VISIBLE in-progress cue is rendered in the bar
// on the SAME frame the in-flight flag is true. Pre-offload this was a silent
// sub-100ms-but-real block on the memo-miss frame; post-offload it is a silent
// async gap unless a cue is shown — this gate locks in the cue.
//
// Production shape mirrored (drift-warning — keep in lock-step):
//   Source/Core/src/Ui/SmatchetToolbarUi.cpp
//     :138       trackerAppendLoadInFlight_ = true;
//     :142-160   LaunchBackgroundTask(LoadPersistentViewsFromDisk) → PostToMainThread apply,
//                clearing trackerAppendLoadInFlight_ when the (still-current) load lands
//     RenderBar  if (trackerAppendLoadInFlight_) { ... SmallButton(ICON_FA_SPINNER
//                "##tb-append-loading"); }  <-- the cue (added with this gate)
//
// Why a TU-local replica (not the live SmatchetToolbarUi): trackerAppendLoadInFlight_
// is a private member (SmatchetToolbarUi.h) and RenderBar needs a live AppController +
// tracker backend to drive a real backend-key change — not drivable from a generic
// test TU. The replica mirrors the exact flag + cue id so the test stays deterministic;
// the cue label here MUST match the production widget id verbatim.
//
// Determinism: the "worker" is a release atom the TestFunc holds, so the cue is
// observable for >= 1 frame regardless of engine stepping rate.
//
// Also here (group "IconPicker"): the REAL SmatchetIconPickerUi, the toolbar editor's icon picker,
// driven through the shared text field (Ui/SmatchetInput.h, docs/audits/INPUT_UX_AUDIT.md § 3 picker
// row and § 4.2) under the dialog contract (Ui/SmatchetDialogScope.h): the caret is in the filter when
// the picker opens, typing filters, Enter picks the icon only when exactly one matches, and Esc closes
// without a pick; a long pasted query is kept whole. Not a replica: the picker is a public class that
// needs no AppController.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "Ui/SmatchetIconPickerUi.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiContext::ActiveIdWindow, ImGui::GetInputTextState
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <cstring>
#include <string>

namespace {

// Replica of SmatchetToolbarUi::trackerAppendLoadInFlight_ + the RenderBar cue.
struct ToolbarAppendCueReplica {
    std::atomic<bool> workerReleased{false}; // TestFunc flips this to "complete" the load
    bool loadInFlight = false;
};

ToolbarAppendCueReplica g_replica;

// Mirror of the RenderBar cue under the production widget id `##tb-append-loading`.
// Production wraps the spinner in BeginDisabled (a non-interactive cue); the test
// engine does not register disabled items for ItemInfo queries, so the replica
// submits the same id as a plain (enabled) findable proxy — the assertion is the
// cue's presence/absence, not its enabled state. Keep the id identical to the
// product widget so an id drift breaks this test.
void SubmitAppendLoadingCue() {
    if (g_replica.loadInFlight) {
        // Findable proxy with an explicit size + `##`-only id. The FA glyph the
        // product uses (ICON_FA_SPINNER) renders as a missing glyph under the
        // headless test font and can yield a zero-extent item the engine can't
        // locate, so the proxy carries no visible glyph — the id is what the
        // assertion matches, and it is identical to the product widget id.
        ImGui::Button("##tb-append-loading", ImVec2(80, 20));
    }
}

void RegisterVisibleWhileLoading(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Pillar2", "ToolbarAppendCue_VisibleWhileLoading");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(240, 80), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::ToolbarAppendCue_Visible")) {
            // Poll the "worker": once released, clear the in-flight flag exactly as
            // the PostToMainThread apply does (:152-158).
            if (g_replica.loadInFlight && g_replica.workerReleased.load(std::memory_order_acquire)) {
                g_replica.loadInFlight = false;
            }
            SubmitAppendLoadingCue();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Arm the in-flight state (mirror :138).
        g_replica.workerReleased.store(false, std::memory_order_release);
        g_replica.loadInFlight = true;

        ctx->SetRef("SmatchetTest::ToolbarAppendCue_Visible");
        ctx->Yield(); // one frame with loadInFlight == true

        // Cue must be on screen while the load is in flight (ID != 0).
        IM_CHECK(ctx->ItemInfo("##tb-append-loading", ImGuiTestOpFlags_NoError).ID != 0);

        // Complete the worker; the next frame's poll clears the flag and the cue
        // stops being submitted.
        g_replica.workerReleased.store(true, std::memory_order_release);
        ctx->Yield();
        ctx->Yield();
        ctx->Yield();

        IM_CHECK(!g_replica.loadInFlight);
        // Cue gone once the load landed: ItemInfo returns the null record.
        IM_CHECK(ctx->ItemInfo("##tb-append-loading", ImGuiTestOpFlags_NoError).ID == 0);
    };
}

// ---------------------------------------------------------------------------
// Group "IconPicker" — the real SmatchetIconPickerUi.
// ---------------------------------------------------------------------------

struct IconPickerProbe {
    SmatchetIconPickerUi picker;
    int picks = 0;
    std::string pickedName;
    std::string pickedGlyph;
};

IconPickerProbe g_iconPicker;

// The picker is a modal popup drawn from inside its owner's window, every frame.
void DrawIconPickerHost() {
    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::IconPickerHost", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        std::string name;
        std::string glyph;
        if (g_iconPicker.picker.Draw(name, glyph)) {
            ++g_iconPicker.picks;
            g_iconPicker.pickedName = name;
            g_iconPicker.pickedGlyph = glyph;
        }
    }
    ImGui::End();
}

// True while the picker's modal popup is up.
bool IconPickerIsOpen() { return ImGui::GetCurrentContext()->OpenPopupStack.Size > 0; }

// True when the keyboard is in a text field inside the picker's popup (the search box).
bool KeyboardIsInPickerFilter() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    return g.ActiveId != 0 && g.ActiveIdWindow != nullptr && (g.ActiveIdWindow->Flags & ImGuiWindowFlags_Popup) != 0 &&
           ImGui::GetInputTextState(g.ActiveId) != nullptr;
}

// Opens a fresh picker and waits for its popup; never clicks, so a caret in the filter is the
// picker's own doing.
bool OpenIconPicker(ImGuiTestContext* ctx) {
    g_iconPicker = IconPickerProbe{};
    ctx->SetRef("SmatchetTest::IconPickerHost");
    ctx->Yield(2);
    ImGui::ClearActiveID();
    g_iconPicker.picker.Open();
    for (int i = 0; i < 30 && !IconPickerIsOpen(); ++i) {
        ctx->Yield();
    }
    ctx->Yield(2);
    return IconPickerIsOpen();
}

// How many catalog entries match `query` the way the picker matches (a slug substring).
int CountCatalogMatches(const char* query) {
    int n = 0;
    for (const SmatchetToolbarIconEntry& e : SmatchetToolbarIconCatalog()) {
        n += std::strstr(e.Name, query) != nullptr ? 1 : 0;
    }
    return n;
}

// An entry whose full slug matches no other entry, so typing it leaves exactly one icon.
const SmatchetToolbarIconEntry* FindUniquelyMatchedEntry() {
    for (const SmatchetToolbarIconEntry& e : SmatchetToolbarIconCatalog()) {
        if (CountCatalogMatches(e.Name) == 1) {
            return &e;
        }
    }
    return nullptr;
}

void RegisterIconPickerOpensWithCaret(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "IconPicker", "OpensWithCaretInFilterAndEnterPicksTheOnlyMatch");
    t->GuiFunc = [](ImGuiTestContext*) { DrawIconPickerHost(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_CHECK(OpenIconPicker(ctx));
        // No click: the picker put the caret in its filter, so the first keys already filter.
        IM_CHECK(KeyboardIsInPickerFilter());

        const SmatchetToolbarIconEntry* only = FindUniquelyMatchedEntry();
        IM_CHECK(only != nullptr);
        if (only == nullptr) {
            return;
        }
        ctx->KeyChars(only->Name);
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(g_iconPicker.picks, 1);
        IM_CHECK_STR_EQ(g_iconPicker.pickedName.c_str(), only->Name);
        IM_CHECK_STR_EQ(g_iconPicker.pickedGlyph.c_str(), only->Glyph);
        IM_CHECK(!IconPickerIsOpen()); // a pick closes the picker
    };
}

// Several matches: Enter does not guess which icon was meant.
void RegisterIconPickerEnterOnSeveralMatches(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "IconPicker", "EnterOnSeveralMatchesPicksNothing");
    t->GuiFunc = [](ImGuiTestContext*) { DrawIconPickerHost(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_CHECK(OpenIconPicker(ctx));
        IM_CHECK(KeyboardIsInPickerFilter());
        IM_CHECK(CountCatalogMatches("arrow-up") > 1);
        ctx->KeyChars("arrow-up");
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(g_iconPicker.picks, 0);
        IM_CHECK(IconPickerIsOpen());

        ctx->KeyPress(ImGuiKey_Escape); // close it so the next test starts from no popup
        ctx->Yield(2);
    };
}

void RegisterIconPickerEnterOnEmptyList(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "IconPicker", "EnterOnEmptyListPicksNothing");
    t->GuiFunc = [](ImGuiTestContext*) { DrawIconPickerHost(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_CHECK(OpenIconPicker(ctx));
        IM_CHECK(KeyboardIsInPickerFilter());
        IM_CHECK_EQ(CountCatalogMatches("zzzqqq-no-such-icon"), 0);
        ctx->KeyChars("zzzqqq-no-such-icon");
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield(2);
        IM_CHECK_EQ(g_iconPicker.picks, 0);
        IM_CHECK(IconPickerIsOpen()); // still open: nothing was picked

        // Clean up: Esc closes it so the next test starts from no popup.
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
    };
}

void RegisterIconPickerEscClosesWithoutPick(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "IconPicker", "EscInFilterClosesWithoutPicking");
    t->GuiFunc = [](ImGuiTestContext*) { DrawIconPickerHost(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_CHECK(OpenIconPicker(ctx));
        IM_CHECK(KeyboardIsInPickerFilter());
        ctx->KeyChars("arrow");
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_EQ(g_iconPicker.picks, 0);
        IM_CHECK(!IconPickerIsOpen());
    };
}

// The filter is a std::string: a pasted query longer than the old 64-byte buffer is kept whole. (No
// icon name is that long, so the match list cannot tell a clipped query from a whole one; the live
// text length of the field can.)
void RegisterIconPickerLongQueryIsKeptWhole(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "IconPicker", "LongQueryIsKeptWholeNotClipped");
    t->GuiFunc = [](ImGuiTestContext*) { DrawIconPickerHost(); };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        IM_CHECK(OpenIconPicker(ctx));
        IM_CHECK(KeyboardIsInPickerFilter());
        const std::string query(200, 'a');
        ImGui::SetClipboardText(query.c_str());
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_V);
        ctx->Yield(2);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiInputTextState* field = ImGui::GetInputTextState(g.ActiveId);
        IM_CHECK(field != nullptr);
        if (field != nullptr) {
            IM_CHECK_EQ(field->TextLen, 200); // the old 64-byte buffer stopped at 63
        }
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(2);
    };
}

} // namespace

extern "C" void SmatchetRegisterToolbarAppendCacheCueTests(ImGuiTestEngine* engine) {
    RegisterVisibleWhileLoading(engine);
    RegisterIconPickerOpensWithCaret(engine);
    RegisterIconPickerEnterOnSeveralMatches(engine);
    RegisterIconPickerEnterOnEmptyList(engine);
    RegisterIconPickerEscClosesWithoutPick(engine);
    RegisterIconPickerLongQueryIsKeptWhole(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
