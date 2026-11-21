#include "Ui/SmatchetFieldViewerModalUi.h"

#include "CachedTicketTypes.h"
#include "SmatchetCommentsModalUi.h"
#include "Ui/DialogKeysPure.h"
#include "Ui/SmatchetActivityLogViewerUi.h"
#include "Ui/SmatchetFieldViewerKindPure.h"
#include "Ui/SmatchetModalPopupOpen.h"
#include "SmatchetLocalization.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <memory>
#include <string>
#include <utility>

namespace {

// "###" ids: the visible title varies per content / issue while the popup id stays stable.
constexpr const char* kFieldViewerPopupId = "###FieldViewerModal";
constexpr const char* kFieldViewerDiscardPopupId = "###FieldViewerDiscardConfirm";
// A dismissal this soon after the viewer came back to ask is not the user: it closes instead of looping.
constexpr int kFieldViewerReopenSettleFrames = 2;

/// Singleton state, decoupled from the grid cell so the modal survives the originating cell scrolling
/// out of view: the cell opens it, then the top-level `RenderFieldViewerModal` owns the lifecycle.
/// Mirrors the long-text modal's file-static state choice.
struct FieldViewerState {
    std::unique_ptr<IFieldViewerContent> Content;
    std::string IssueId;
    std::string FieldId;
    /// "<Title> — <issue>###id" labels, built once at open (no per-frame string building).
    std::string WindowLabel;
    std::string DiscardLabel;
    bool Active = false;
    bool JustOpened = false;
    /// The popup was dismissed around unsaved input (the title-bar X, or another popup taking its
    /// place): it comes back next frame with the discard question up.
    bool ReopenForDiscardConfirm = false;
    /// Frame of the last such return; negative when there was none.
    int ReopenFrame = -1;
};

FieldViewerState s_Viewer;

/// Runs the content's cleanup, then drops it and the open state.
void CloseFieldViewer() {
    if (s_Viewer.Content) {
        s_Viewer.Content->OnClose();
    }
    s_Viewer = FieldViewerState{};
}

} // namespace

void OpenFieldViewer(std::unique_ptr<IFieldViewerContent> content, const std::string& issueId,
                     const std::string& fieldId) {
    if (!content) {
        return;
    }
    // The previous content cleans up before the new one is installed (and before the caller seeds
    // any state of its own), so a stale OnClose can never wipe the fresh open.
    if (s_Viewer.Content) {
        s_Viewer.Content->OnClose();
    }
    s_Viewer.Content = std::move(content);
    s_Viewer.IssueId = issueId;
    s_Viewer.FieldId = fieldId;
    s_Viewer.WindowLabel = s_Viewer.Content->Title() + " \xE2\x80\x94 " + issueId + kFieldViewerPopupId;
    s_Viewer.DiscardLabel = std::string(s_Viewer.Content->DiscardConfirmTitle()) + kFieldViewerDiscardPopupId;
    s_Viewer.Active = true;
    s_Viewer.JustOpened = true;
    s_Viewer.ReopenForDiscardConfirm = false;
    s_Viewer.ReopenFrame = -1;
}

bool OpenFieldViewerForField(AppController& app, const CachedTicket& ticket, const std::string& fieldId) {
    switch (SmatchetFieldViewer::FieldViewerKindForFieldId(fieldId)) {
    case SmatchetFieldViewer::FieldViewerKind::Comments:
        OpenCommentsModal(app, ticket.id, std::string(), CommentsOpenFocus::Composer);
        return true;
    case SmatchetFieldViewer::FieldViewerKind::ActivityLog:
        OpenFieldViewer(MakeActivityLogViewerContent(ticket.GetFieldValueRef(fieldId)), ticket.id, fieldId);
        return true;
    case SmatchetFieldViewer::FieldViewerKind::None:
        break;
    }
    return false;
}

void RenderFieldViewerModal(AppController& app, bool readOnlyMode) {
    if (!s_Viewer.Active || !s_Viewer.Content) {
        return;
    }

    const bool reopenForConfirm = s_Viewer.ReopenForDiscardConfirm;
    // OpenPopup must be called at the same window-depth as BeginPopupModal — both here, outside
    // the table, so ImGui can anchor the popup to this stable top-level window.
    smatchet::ui::OpenOrBringBackModalPopup(kFieldViewerPopupId, s_Viewer.JustOpened, reopenForConfirm, 0.5f, 0.6f);

    // The title-bar X is driven by p_open. When it is clicked ImGui clears the flag and dismisses the
    // popup inside BeginPopupModal on that same frame, so the X cannot be turned down in place. The
    // dismissed branch below gives it the meaning of the Close button and Esc: a viewer with nothing
    // unsaved closes, unsaved input comes back next frame with the discard question up.
    bool modalOpen = true;
    if (ImGui::BeginPopupModal(s_Viewer.WindowLabel.c_str(), &modalOpen,
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        // Room for the Close row (separator + button) under the content's body. The body child keeps
        // its scrollbar as a fallback: content whose own layout overflows stays reachable.
        const float closeRowH = ImGui::GetStyle().ItemSpacing.y * 2.0f + ImGui::GetFrameHeight() + 1.0f;
        const float bodyH = ImGui::GetContentRegionAvail().y - closeRowH;
        ImGui::BeginChild("##FieldViewerBody", ImVec2(-FLT_MIN, bodyH > 0.0f ? bodyH : 0.0f), false);
        s_Viewer.Content->DrawBody(app, readOnlyMode);
        ImGui::EndChild();

        ImGui::Separator();
        bool wantClose = ImGui::Button("Close", ImVec2(100, 0));
        // P2-M3: Esc while an input is being edited only leaves that input (the content decides what
        // happens to its text; the comments box keeps it) — it must not also tear down the modal
        // around the draft. Only a second, unfocused Esc reaches the close path, unless the content
        // asks for the close itself (ConsumeCloseRequest). While the discard confirm is up, Esc is
        // its "keep writing" (handled inside it), not another close request that re-opens it.
        const bool discardWasOpen = ImGui::IsPopupOpen(kFieldViewerDiscardPopupId);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput && !discardWasOpen) {
            wantClose = true;
        }
        if (reopenForConfirm) { // back after a dismissal around unsaved input: ask now
            s_Viewer.ReopenForDiscardConfirm = false;
            s_Viewer.ReopenFrame = ImGui::GetFrameCount();
            wantClose = true;
        }
        if (s_Viewer.Content->ConsumeCloseRequest()) {
            wantClose = true;
        }
        bool closeNow = false;
        if (wantClose) {
            if (s_Viewer.Content->HasUnsavedInput()) {
                ImGui::OpenPopup(kFieldViewerDiscardPopupId);
            } else {
                closeNow = true;
            }
        }
        bool discardConfirmed = false;
        if (ImGui::BeginPopupModal(s_Viewer.DiscardLabel.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(s_Viewer.Content->DiscardConfirmText());
            if (ImGui::Button(s_Viewer.Content->DiscardButtonLabel())) {
                discardConfirmed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(s_Viewer.Content->KeepButtonLabel()) ||
                (discardWasOpen && !discardConfirmed && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (closeNow || discardConfirmed) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (discardConfirmed) {
            s_Viewer.Content->OnDiscard();
        }
        if (closeNow || discardConfirmed) {
            CloseFieldViewer();
        }
    } else {
        if (ImGui::IsPopupOpen(kFieldViewerPopupId)) {
            // Still open, just not drawn this frame (clipped, e.g. a minimised host): nothing was
            // dismissed, so the viewer and its input stay as they are.
            return;
        }
        // Dismissed without the Close row's say: the title-bar X, or another popup took its place.
        if (smatchet::dialogkeys::DismissedPopupComesBack(s_Viewer.Content->HasUnsavedInput(), reopenForConfirm,
                                                          ImGui::GetFrameCount(), s_Viewer.ReopenFrame,
                                                          kFieldViewerReopenSettleFrames)) {
            s_Viewer.ReopenForDiscardConfirm = true;
        } else {
            CloseFieldViewer();
        }
    }
}

FieldViewerSnapshot GetFieldViewerSnapshotForTests() {
    FieldViewerSnapshot snap;
    snap.Active = s_Viewer.Active;
    snap.FieldId = s_Viewer.FieldId;
    snap.IssueId = s_Viewer.IssueId;
    if (s_Viewer.Content) {
        snap.Title = s_Viewer.Content->Title();
    }
    return snap;
}
