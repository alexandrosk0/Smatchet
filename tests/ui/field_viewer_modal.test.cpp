// field_viewer_modal.test.cpp — bucket-E tests for the generic field-viewer modal shell (History +
// Comments), the comments post box's keyboard contract, and the long-text editor's close guard (the
// other modal the grid pane draws). Registered in the "OfflineFirst" group so scripts/dev/test-ui-offline-first.sh runs
// them against the same fixture (the modal renders only from a focused grid pane, as the Comments tests need). Under
// any other fixture they skip with an informational log.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "CachedTicketTypes.h"
#include "DraftStore.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "FakeNetworkSwitch.h"
#include "FakeTrackerClient.h" // the posts the fixture tracker received
#include "SmatchetCommentsModalUi.h"
#include "TicketFieldEditor_detail.h" // OpenLongTextEditor + its test snapshot
#include "TrackerFieldSchema.h"
#include "Types/ConnectivityTypes.h"
#include "Ui/SmatchetFieldViewerModalUi.h"
#include "UiTestWriteScope.h" // the fresh profile is read-only, which disables the inputs under test

#include "imgui.h"
#include "imgui_internal.h" // the popup stack: which modal is on top
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool OfflineFirstFixtureActive(ImGuiTestContext* ctx) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* fixture = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (fixture != nullptr && std::strstr(fixture, "offline-first") != nullptr) {
        return true;
    }
    ctx->LogInfo("SKIP: offline-first fixture not active");
    return false;
}

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, int maxFrames, Pred done) {
    for (int i = 0; i < maxFrames && !done(); ++i) {
        ctx->Yield();
    }
    return done();
}

CachedTicket MakeTicketWithHistory(const char* id, const char* history) {
    CachedTicket ticket;
    ticket.id = id;
    ticket.fieldValues["history"] = history;
    return ticket;
}

// Close whatever viewer is open the way a user does (Esc); the grid pane must be focused to render it.
void CloseViewerWithEscape(ImGuiTestContext* ctx) {
    ctx->KeyPress(ImGuiKey_Escape);
    if (!YieldUntil(ctx, 30, []() { return !GetFieldViewerSnapshotForTests().Active; })) {
        ctx->LogWarning("field viewer still open after Esc (grid pane not focused?)");
    }
}

// Pull the connectivity probe forward until the tracker reads as reachable: the post box is disabled
// while it does not.
bool WaitForOnline(ImGuiTestContext* ctx, AppController& app) {
    return YieldUntil(ctx, 600, [&app]() {
        if (app.GetLastTrackerConnectivityState() == TrackerConnectivityState::AuthenticatedReachable) {
            return true;
        }
        app.RequestTrackerProbeNow();
        return false;
    });
}

// The topmost open popup when it is a modal (a viewer, an editor, or the confirm above one), else null.
ImGuiWindow* TopModal() {
    ImGuiContext& g = *GImGui;
    if (g.OpenPopupStack.Size == 0) {
        return nullptr;
    }
    ImGuiWindow* top = g.OpenPopupStack.back().Window;
    return (top != nullptr && (top->Flags & ImGuiWindowFlags_Modal) != 0) ? top : nullptr;
}

// Clicks the button `label` of the topmost modal. False (and no click) when that modal has no such button.
bool ClickInTopModal(ImGuiTestContext* ctx, const char* label) {
    ImGuiWindow* top = TopModal();
    if (top == nullptr) {
        return false;
    }
    ctx->SetRef(top->ID);
    if (!ctx->ItemExists(label)) {
        return false;
    }
    ctx->ItemClick(label);
    return true;
}

bool WaitForComposerCaret(ImGuiTestContext* ctx) {
    return YieldUntil(ctx, 60, []() { return GetCommentsModalSnapshotForTests().ComposerActive; });
}

// Closes the comments viewer from wherever a case left it: answers the discard confirm when it is up,
// otherwise presses Esc (which first leaves the post box, then asks, then closes).
void CloseCommentsViewer(ImGuiTestContext* ctx) {
    for (int i = 0; i < 6 && GetFieldViewerSnapshotForTests().Active; ++i) {
        if (!ClickInTopModal(ctx, "Discard comment")) {
            ctx->KeyPress(ImGuiKey_Escape);
        }
        ctx->Yield(3);
    }
    if (GetFieldViewerSnapshotForTests().Active) {
        ctx->LogError("comments viewer still open after the close sequence");
    }
}

void OpenLongTextForTest(AppController& app, const char* seed) {
    TrackerField field;
    field.Id = "description";
    field.Name = "Description";
    OpenLongTextEditor(app, "OFF-1", field, "Description", seed, std::string());
}

// Types `text` into the long-text editor and waits until the editor reports it as an unsaved edit.
// That only happens when the editor has the keyboard, so it also proves where the caret is.
bool TypeIntoLongText(ImGuiTestContext* ctx, const char* text) {
    ctx->Yield(4); // the popup opens and hands the keyboard to the editor
    ctx->KeyChars(text);
    return YieldUntil(ctx, 30, [text]() {
        const LongTextEditorTestSnapshot snap = SmatchetTest_GetLongTextEditorSnapshot();
        return snap.Dirty && snap.Buffer.find(text) != std::string::npos;
    });
}

// Closes the long-text editor from wherever a case left it, discarding what was typed.
void CloseLongTextForTest(ImGuiTestContext* ctx) {
    for (int i = 0; i < 6 && SmatchetTest_GetLongTextEditorSnapshot().Active; ++i) {
        if (!ClickInTopModal(ctx, "Discard changes")) {
            ctx->KeyPress(ImGuiKey_Escape);
        }
        ctx->Yield(3);
    }
    if (SmatchetTest_GetLongTextEditorSnapshot().Active) {
        ctx->LogError("long-text editor still open after the close sequence");
        CloseLongTextEditor(); // never leave its state behind for the next test
    }
}

} // namespace

// OfflineFirst/FieldViewer_HistoryOpensAndEscCloses: the History field opens the generic viewer
// window titled "History" for that issue, and Esc closes it.
static void RegisterFieldViewerHistoryOpensAndEscCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_HistoryOpensAndEscCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "[Ana] 2026-01-02\nstatus: To Do -> Done");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "history"));
        ctx->Yield(2);
        const FieldViewerSnapshot open = GetFieldViewerSnapshotForTests();
        IM_CHECK_NO_RET(open.Active);
        IM_CHECK_NO_RET(open.FieldId == "history");
        IM_CHECK_NO_RET(open.IssueId == "OFF-1");
        IM_CHECK_NO_RET(open.Title == "History");

        CloseViewerWithEscape(ctx);
        const bool closed = !GetFieldViewerSnapshotForTests().Active;
        if (!closed) {
            // Never leave a modal open for the next test; the render path is the only closer.
            ctx->LogError("History viewer did not close on Esc");
        }
        IM_CHECK_NO_RET(closed);
    };
}

// OfflineFirst/FieldViewer_FieldWithoutViewerIsRejected: a field with no viewer opens nothing.
static void RegisterFieldViewerUnknownFieldRejected(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_FieldWithoutViewerIsRejected");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(!OpenFieldViewerForField(*app, ticket, "description"));
        IM_CHECK_NO_RET(!GetFieldViewerSnapshotForTests().Active);
    };
}

// OfflineFirst/FieldViewer_HistoryReplacesComments: opening History while Comments is open replaces
// it, and the replaced content's OnClose resets the comments state (late post-backs are dropped).
static void RegisterFieldViewerHistoryReplacesComments(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "FieldViewer_HistoryReplacesComments");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (!OfflineFirstFixtureActive(ctx)) {
            return;
        }
        AppController* app = SmatchetActiveUiTestAppController();
        if (!app) {
            IM_CHECK_NO_RET(app != nullptr);
            return;
        }
        OpenCommentsModal(*app, "OFF-1");
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().Active);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().FieldId == "comments");
        ctx->Yield(2);

        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "[Ana] 2026-01-02\nstatus: To Do -> Done");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "history"));
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().Active);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().FieldId == "history");
        ctx->Yield(2);

        CloseViewerWithEscape(ctx);
        IM_CHECK_NO_RET(!GetFieldViewerSnapshotForTests().Active);
    };
}

// The cases below type into the comments post box and the long-text editor. Both are disabled or
// refuse the keyboard in read-only mode (the fresh-profile default), so each takes a write scope.
#define SMATCHET_INPUT_CASE_PREAMBLE(ctx, app)                                                                         \
    if (!OfflineFirstFixtureActive(ctx)) {                                                                             \
        return;                                                                                                        \
    }                                                                                                                  \
    smatchet::drafts::ResetForTests(); /* a draft one case leaves must not prefill the next */                         \
    smatchet_tests::ScopedFakeNetworkReset networkReset;                                                               \
    BucketE::UiTestWriteScope writeScope;                                                                              \
    AppController* app = SmatchetActiveUiTestAppController();                                                          \
    if (!app) {                                                                                                        \
        IM_CHECK_NO_RET(app != nullptr);                                                                               \
        return;                                                                                                        \
    }                                                                                                                  \
    IM_CHECK_NO_RET(WaitForOnline(ctx, *app))

// OfflineFirst/Comments_UserOpenPutsCaretInPostBox: opening Comments from a cell puts the caret in
// the post box, and Esc on the untouched box closes the viewer in one press.
static void RegisterCommentsUserOpenPutsCaretInPostBox(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_UserOpenPutsCaretInPostBox");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));

        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() { return !GetFieldViewerSnapshotForTests().Active; }));
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_ProgrammaticOpenLeavesKeyboardAlone: an open nobody asked for with a click
// (a command, a deep link) does not take the keyboard.
static void RegisterCommentsProgrammaticOpenLeavesKeyboard(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_ProgrammaticOpenLeavesKeyboardAlone");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenCommentsModal(*app, "OFF-1");
        ctx->Yield(10);
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().Active);
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().ComposerActive);
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_EscKeepsTypedText: Esc in the post box leaves the box with the comment still
// in it; the next Esc asks before discarding, and "Keep writing" keeps it.
static void RegisterCommentsEscKeepsTypedText(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_EscKeepsTypedText");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyChars("draft one");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "draft one");

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().ComposerActive);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "draft one");

        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
        IM_CHECK_NO_RET(ClickInTopModal(ctx, "Keep writing"));
        ctx->Yield(3);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "draft one");
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_TitleBarCloseKeepsDraft: the title-bar X with a comment typed does not lose it:
// the viewer comes back with the comment and the question that Close and Esc ask.
static void RegisterCommentsTitleBarCloseKeepsDraft(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_TitleBarCloseKeepsDraft");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyChars("draft one");
        ctx->Yield(2);

        ImGuiWindow* viewer = TopModal();
        IM_CHECK_NO_RET(viewer != nullptr);
        if (viewer != nullptr) {
            const ImGuiID viewerId = viewer->ID;
            ctx->WindowClose(viewerId);
            // Back, with the question above it: the topmost modal is no longer the viewer itself.
            IM_CHECK_NO_RET(YieldUntil(ctx, 30, [viewerId]() {
                ImGuiWindow* top = TopModal();
                return GetFieldViewerSnapshotForTests().Active && top != nullptr && top->ID != viewerId;
            }));
            IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "draft one");

            IM_CHECK_NO_RET(ClickInTopModal(ctx, "Keep writing"));
            ctx->Yield(3);
            IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
            IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "draft one");
        }
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_BlankBoxCannotPost: a box holding only spaces posts nothing, by the button or
// by Ctrl+Enter, and Ctrl+Enter gives the caret back.
static void RegisterCommentsBlankBoxCannotPost(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_BlankBoxCannotPost");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(app->BackendShared().get());
        IM_CHECK_NO_RET(fake != nullptr);
        if (!fake) {
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().PostEnabled);
        ctx->KeyChars("   ");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "   ");
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().PostEnabled);

        const std::size_t postsBefore = fake->AddCommentCalls().size();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield(4);
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().PostInFlight);
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->Yield(20); // a post would have reached the fake tracker by now
        IM_CHECK_NO_RET(fake->AddCommentCalls().size() == postsBefore);
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_CtrlEnterPosts: Ctrl+Enter in the post box sends the comment once, clears the
// box and gives the caret back for the next one.
static void RegisterCommentsCtrlEnterPosts(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_CtrlEnterPosts");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        auto* fake = dynamic_cast<smatchet_tests::FakeTrackerClient*>(app->BackendShared().get());
        IM_CHECK_NO_RET(fake != nullptr);
        if (!fake) {
            return;
        }
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        const std::size_t postsBefore = fake->AddCommentCalls().size();
        ctx->KeyChars("sent from the keyboard");
        ctx->Yield(2);
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().PostEnabled);

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        // The post is over when the box is empty again (cleared by the result on this thread).
        IM_CHECK_NO_RET(YieldUntil(ctx, 600, []() {
            const CommentsModalSnapshot snap = GetCommentsModalSnapshotForTests();
            return !snap.PostInFlight && snap.PostDraft.empty();
        }));
        IM_CHECK_NO_RET(fake->AddCommentCalls().size() == postsBefore + 1);
        if (fake->AddCommentCalls().size() == postsBefore + 1) {
            IM_CHECK_STR_EQ_NO_RET(fake->AddCommentCalls().back().IssueKey.c_str(), "OFF-1");
            IM_CHECK_STR_EQ_NO_RET(fake->AddCommentCalls().back().Body.c_str(), "sent from the keyboard");
        }
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_EscOnFilledBoxNobodyTypedIn: Esc in a box that holds text the user has not
// typed into since the caret arrived (a template, or a draft the caret was put back into) only
// leaves the box. It must not ask to close: that would put "Discard comment" one key away.
static void RegisterCommentsEscOnFilledBoxOnlyLeaves(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_EscOnFilledBoxNobodyTypedIn");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenCommentsModal(*app, "OFF-1", "template text", CommentsOpenFocus::Composer);
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyPress(ImGuiKey_Escape);
        ctx->Yield(3);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().ComposerActive);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "template text");
        IM_CHECK_NO_RET(!ClickInTopModal(ctx, "Keep writing")); // no discard question came up
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_TemplateOpenKeepsPrefill: a template opens Comments with its text in the box
// and the caret there; typing adds to the template instead of replacing it.
static void RegisterCommentsTemplateOpenKeepsPrefill(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_TemplateOpenKeepsPrefill");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenCommentsModal(*app, "OFF-1", "template text", CommentsOpenFocus::Composer);
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "template text");
        ctx->KeyChars("!");
        ctx->Yield(2);
        const std::string draft = GetCommentsModalSnapshotForTests().PostDraft;
        IM_CHECK_NO_RET(draft.find("template text") != std::string::npos);
        IM_CHECK_NO_RET(draft.size() == std::strlen("template text") + 1);
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/Comments_OtherIssueStartsWithEmptyBox: opening another issue's comments while typing
// starts that issue with an empty box. The text being typed must not follow the caret across.
static void RegisterCommentsOtherIssueStartsEmpty(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_OtherIssueStartsWithEmptyBox");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket first = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, first, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyChars("for the first issue");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "for the first issue");

        const CachedTicket second = MakeTicketWithHistory("OFF-2", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, second, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->Yield(4);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "");
        ctx->KeyChars("b");
        ctx->Yield(2);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "b");
        IM_CHECK_STR_EQ_NO_RET(GetFieldViewerSnapshotForTests().IssueId.c_str(), "OFF-2");
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/LongText_UntouchedEscCloses: with nothing typed, Esc closes the editor at once.
static void RegisterLongTextUntouchedEscCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "LongText_UntouchedEscCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenLongTextForTest(*app, "seed text");
        ctx->Yield(4);
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Active);
        IM_CHECK_NO_RET(!SmatchetTest_GetLongTextEditorSnapshot().Dirty);
        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() { return !SmatchetTest_GetLongTextEditorSnapshot().Active; }));
        CloseLongTextForTest(ctx);
    };
}

// OfflineFirst/LongText_EscWithTextAsksAndKeeps: the editor opens with the caret in it; Esc with typed
// text asks first, Esc again keeps editing, and the caret is back where it was.
static void RegisterLongTextEscWithTextAsksAndKeeps(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "LongText_EscWithTextAsksAndKeeps");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenLongTextForTest(*app, "seed text");
        IM_CHECK_NO_RET(TypeIntoLongText(ctx, "abc"));

        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(
            YieldUntil(ctx, 30, []() { return SmatchetTest_GetLongTextEditorSnapshot().DiscardConfirmOpen; }));
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Active);

        ctx->KeyPress(ImGuiKey_Escape);
        IM_CHECK_NO_RET(
            YieldUntil(ctx, 30, []() { return !SmatchetTest_GetLongTextEditorSnapshot().DiscardConfirmOpen; }));
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Active);
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Dirty);

        ctx->Yield(3);
        ctx->KeyChars("d");
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() {
            return SmatchetTest_GetLongTextEditorSnapshot().Buffer.find("abcd") != std::string::npos;
        }));
        CloseLongTextForTest(ctx);
        IM_CHECK_NO_RET(!SmatchetTest_GetLongTextEditorSnapshot().Active);
    };
}

// OfflineFirst/LongText_CancelAsks_DiscardCloses: Cancel with typed text asks first; Ctrl+Enter while
// the question is up saves nothing; "Discard changes" closes the editor.
static void RegisterLongTextCancelAsksDiscardCloses(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "LongText_CancelAsks_DiscardCloses");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenLongTextForTest(*app, "seed text");
        IM_CHECK_NO_RET(TypeIntoLongText(ctx, "abc"));

        IM_CHECK_NO_RET(ClickInTopModal(ctx, "Cancel"));
        IM_CHECK_NO_RET(
            YieldUntil(ctx, 30, []() { return SmatchetTest_GetLongTextEditorSnapshot().DiscardConfirmOpen; }));

        // A save would close the editor. It must stay open, with the text still unsaved.
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield(4);
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Active);
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Dirty);
        IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().DiscardConfirmOpen);

        IM_CHECK_NO_RET(ClickInTopModal(ctx, "Discard changes"));
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() { return !SmatchetTest_GetLongTextEditorSnapshot().Active; }));
        CloseLongTextForTest(ctx);
    };
}

// OfflineFirst/LongText_TitleBarCloseKeepsText: the title-bar X with typed text does not lose it: the
// editor comes back with the text and the same question.
static void RegisterLongTextTitleBarCloseKeepsText(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "LongText_TitleBarCloseKeepsText");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenLongTextForTest(*app, "seed text");
        IM_CHECK_NO_RET(TypeIntoLongText(ctx, "abc"));

        ImGuiWindow* editor = TopModal();
        IM_CHECK_NO_RET(editor != nullptr);
        if (editor != nullptr) {
            ctx->WindowClose(editor->ID);
            IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() {
                const LongTextEditorTestSnapshot snap = SmatchetTest_GetLongTextEditorSnapshot();
                return snap.Active && snap.DiscardConfirmOpen;
            }));
            IM_CHECK_NO_RET(SmatchetTest_GetLongTextEditorSnapshot().Buffer.find("abc") != std::string::npos);

            IM_CHECK_NO_RET(ClickInTopModal(ctx, "Keep editing"));
            ctx->Yield(3);
            const LongTextEditorTestSnapshot kept = SmatchetTest_GetLongTextEditorSnapshot();
            IM_CHECK_NO_RET(kept.Active);
            IM_CHECK_NO_RET(!kept.DiscardConfirmOpen);
            IM_CHECK_NO_RET(kept.Dirty);
        }
        CloseLongTextForTest(ctx);
    };
}

// The draft store key the comments box uses for `issueId` on the focused tracker.
static std::string CommentsDraftScopeForTest(AppController& app, const char* issueId) {
    return app.FocusedCacheBackendKey() + "|" + issueId;
}

// OfflineFirst/Comments_DraftSurvivesReplaceAndComesBack: what is typed in the comment box is kept as it
// is typed. Opening another issue's comments (which replaces the viewer) does not lose it, and opening
// the first issue again brings it back with a line that says so.
static void RegisterCommentsDraftSurvivesReplace(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_DraftSurvivesReplaceAndComesBack");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket first = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, first, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyChars("kept across a close");
        ctx->Yield(2);

        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(smatchet::drafts::Get("comments", CommentsDraftScopeForTest(*app, "OFF-1"), stored));
        IM_CHECK_STR_EQ_NO_RET(stored.Text.c_str(), "kept across a close");

        const CachedTicket second = MakeTicketWithHistory("OFF-2", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, second, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->Yield(3);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "");
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().DraftRestored);

        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, first, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->Yield(3);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "kept across a close");
        IM_CHECK_NO_RET(GetCommentsModalSnapshotForTests().DraftRestored);
        CloseCommentsViewer(ctx); // discards: the box is not empty
    };
}

// OfflineFirst/Comments_DiscardErasesTheKeptDraft: Esc asks, and choosing "Discard comment" throws the
// text away for good: nothing is kept, and the next open starts with an empty box.
static void RegisterCommentsDiscardErasesDraft(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "Comments_DiscardErasesTheKeptDraft");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        const CachedTicket ticket = MakeTicketWithHistory("OFF-1", "x");
        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->KeyChars("to be thrown away");
        ctx->Yield(2);
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(smatchet::drafts::Get("comments", CommentsDraftScopeForTest(*app, "OFF-1"), stored));

        ctx->KeyPress(ImGuiKey_Escape); // leaves the box
        ctx->Yield(3);
        ctx->KeyPress(ImGuiKey_Escape); // asks
        ctx->Yield(3);
        IM_CHECK_NO_RET(GetFieldViewerSnapshotForTests().Active);
        IM_CHECK_NO_RET(smatchet::drafts::Get("comments", CommentsDraftScopeForTest(*app, "OFF-1"), stored));

        IM_CHECK_NO_RET(ClickInTopModal(ctx, "Discard comment"));
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() { return !GetFieldViewerSnapshotForTests().Active; }));
        IM_CHECK_NO_RET(!smatchet::drafts::Get("comments", CommentsDraftScopeForTest(*app, "OFF-1"), stored));

        IM_CHECK_NO_RET(OpenFieldViewerForField(*app, ticket, "comments"));
        IM_CHECK_NO_RET(WaitForComposerCaret(ctx));
        ctx->Yield(3);
        IM_CHECK_STR_EQ_NO_RET(GetCommentsModalSnapshotForTests().PostDraft.c_str(), "");
        IM_CHECK_NO_RET(!GetCommentsModalSnapshotForTests().DraftRestored);
        CloseCommentsViewer(ctx);
    };
}

// OfflineFirst/LongText_DraftComesBackOnReopen: the edit typed in the long-text editor is kept per issue
// and field. Closing the editor without answering the question (here the way a replace or an exit does,
// by dropping its state) leaves it, and the next open puts it back with the dirty guard armed; "Discard
// changes" erases it.
static void RegisterLongTextDraftComesBack(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "OfflineFirst", "LongText_DraftComesBackOnReopen");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        SMATCHET_INPUT_CASE_PREAMBLE(ctx, app);
        OpenLongTextForTest(*app, "seed text");
        IM_CHECK_NO_RET(TypeIntoLongText(ctx, "abc"));
        smatchet::drafts::DraftEntry stored;
        IM_CHECK_NO_RET(smatchet::drafts::Get("longtext", "OFF-1|description", stored));
        IM_CHECK_NO_RET(stored.Text.find("abc") != std::string::npos);
        IM_CHECK_STR_EQ_NO_RET(stored.Base.c_str(), "seed text");

        CloseLongTextEditor(); // not a discard: the draft stays
        ctx->Yield(3);
        IM_CHECK_NO_RET(smatchet::drafts::Get("longtext", "OFF-1|description", stored));

        OpenLongTextForTest(*app, "seed text");
        IM_CHECK_NO_RET(YieldUntil(ctx, 30, []() { return SmatchetTest_GetLongTextEditorSnapshot().Active; }));
        ctx->Yield(3);
        const LongTextEditorTestSnapshot restored = SmatchetTest_GetLongTextEditorSnapshot();
        IM_CHECK_NO_RET(restored.DraftRestored);
        IM_CHECK_NO_RET(restored.Dirty);
        IM_CHECK_NO_RET(restored.Buffer.find("abc") != std::string::npos);

        // Esc asks (the restored text is unsaved), and discarding erases the draft.
        CloseLongTextForTest(ctx);
        IM_CHECK_NO_RET(!SmatchetTest_GetLongTextEditorSnapshot().Active);
        IM_CHECK_NO_RET(!smatchet::drafts::Get("longtext", "OFF-1|description", stored));

        OpenLongTextForTest(*app, "seed text");
        ctx->Yield(4);
        IM_CHECK_NO_RET(!SmatchetTest_GetLongTextEditorSnapshot().DraftRestored);
        IM_CHECK_NO_RET(!SmatchetTest_GetLongTextEditorSnapshot().Dirty);
        CloseLongTextForTest(ctx);
    };
}

#undef SMATCHET_INPUT_CASE_PREAMBLE

extern "C" void SmatchetRegisterFieldViewerModalTests(ImGuiTestEngine* engine) {
    RegisterFieldViewerHistoryOpensAndEscCloses(engine);
    RegisterFieldViewerUnknownFieldRejected(engine);
    RegisterFieldViewerHistoryReplacesComments(engine);
    RegisterCommentsUserOpenPutsCaretInPostBox(engine);
    RegisterCommentsProgrammaticOpenLeavesKeyboard(engine);
    RegisterCommentsEscKeepsTypedText(engine);
    RegisterCommentsTitleBarCloseKeepsDraft(engine);
    RegisterCommentsBlankBoxCannotPost(engine);
    RegisterCommentsCtrlEnterPosts(engine);
    RegisterCommentsEscOnFilledBoxOnlyLeaves(engine);
    RegisterCommentsTemplateOpenKeepsPrefill(engine);
    RegisterCommentsOtherIssueStartsEmpty(engine);
    RegisterLongTextUntouchedEscCloses(engine);
    RegisterLongTextEscWithTextAsksAndKeeps(engine);
    RegisterLongTextCancelAsksDiscardCloses(engine);
    RegisterLongTextTitleBarCloseKeepsText(engine);
    RegisterCommentsDraftSurvivesReplace(engine);
    RegisterCommentsDiscardErasesDraft(engine);
    RegisterLongTextDraftComesBack(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
