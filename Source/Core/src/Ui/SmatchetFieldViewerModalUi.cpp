#include "Ui/SmatchetFieldViewerModalUi.h"

#include "CachedTicketTypes.h"
#include "SmatchetCommentsModalUi.h"
#include "Ui/SmatchetActivityLogViewerUi.h"
#include "Ui/SmatchetFieldViewerKindPure.h"
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
}

bool OpenFieldViewerForField(AppController& app, const CachedTicket& ticket, const std::string& fieldId) {
    switch (SmatchetFieldViewer::FieldViewerKindForFieldId(fieldId)) {
    case SmatchetFieldViewer::FieldViewerKind::Comments:
        OpenCommentsModal(app, ticket.id);
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

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (s_Viewer.JustOpened) {
        // OpenPopup must be called at the same window-depth as BeginPopupModal — both here, outside
        // the table, so ImGui can anchor the popup to this stable top-level window.
        ImGui::OpenPopup(kFieldViewerPopupId);
        const ImVec2 modalSize(viewport->Size.x * 0.5f, viewport->Size.y * 0.6f);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        s_Viewer.JustOpened = false;
    }

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
        // P2-M3: Esc while an input is being edited only defocuses it (ImGui's own
        // revert-and-deactivate) — it must not also tear down the modal around the draft. Only a
        // second, unfocused Esc reaches the close path.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
            wantClose = true;
        }
        if (!modalOpen) { // title-bar X — same guard as Close/Esc (the local resets next frame)
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
            if (ImGui::Button(s_Viewer.Content->KeepButtonLabel())) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (closeNow || discardConfirmed) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (closeNow || discardConfirmed) {
            CloseFieldViewer();
        }
    } else {
        // Modal dismissed without our intervention (title-bar X) — stay self-consistent.
        CloseFieldViewer();
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
