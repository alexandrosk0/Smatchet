// Quick-create issue popup. See SmatchetQuickCreateIssueUi.h.
// Dual-target: no GL/GLFW here; the engine-context prefill degrades to an empty
// description when no host snapshot exists (standalone build).

// SMATCHET_DEVIATION(rule=duplication; reason=include-block clone, audit #12; owner=cpp-audit; revisit=2026-09-30)
#include "SmatchetQuickCreateIssueUi.h"

// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=popup needs the create pipeline (CreateIssueAsync/QueueCreateOffline/GetRequiredFieldSet/BuildDraftFromLastTicket) plus SmatchetProjectPicker::Draw which takes AppController&, same facet set as the grandfathered SmatchetNewIssueDraftUi.cpp; owner=quick-create-issue-unreal-context; revisit=2026-12-31)
#include "AppController.h"
// clang-format on
#include "ConfigManager.h"
#include "Diagnostics/EngineContextFormat.h"
#include "Diagnostics/EngineHostContext.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "SmatchetLocalization.h"
#include "SmatchetProjectPicker.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "StringUtil.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization wrapper.
#define ImGui SmatchetLocalizedImGui

#include <cfloat>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace {

const std::size_t kSummaryBufCap = 1024u;
const std::size_t kQcDescBufCap = 64u * 1024u;

std::string BufText(const std::vector<char>& buf) {
    // Buffers are NUL-terminated by InputText/InputTextMultiline.
    return buf.empty() ? std::string() : std::string(buf.data());
}

void AssignBuf(std::vector<char>& buf, std::size_t cap, const std::string& text) {
    buf.assign(cap, '\0');
    if (!text.empty()) {
        std::strncpy(buf.data(), text.c_str(), cap - 1);
    }
}

// Kick the engine-context prefill on a worker: read + parse the host snapshot and
// format it through the Preferences toggles, then post the markdown back. Never
// clobbers a description the user already edited.
void LaunchContextPrefill(AppController& app, UiDrawSession& d) {
    if (d.quickCreateCtxGenerating || d.quickCreateDescUserEdited) {
        return;
    }
    d.quickCreateCtxGenerating = true;
    const smatchet::hostctx::EngineContextToggles toggles = smatchet::hostctx::TogglesFromConfig(d.cfg);
    app.LaunchBackgroundTask([&app, &d, toggles]() {
        const std::string snapshotJson = smatchet::hostctx::GetSnapshotJson();
        auto text = std::make_shared<std::string>();
        if (!snapshotJson.empty()) {
            std::string parseErr;
            const nlohmann::json snapshot = smatchet::json_safe::ParseBounded(snapshotJson, parseErr);
            if (parseErr.empty()) {
                *text = smatchet::hostctx::BuildEngineContextMarkdown(snapshot, toggles);
            } else {
                LOG_WARN("QuickCreateIssueUi: host context snapshot rejected: %s", parseErr.c_str());
            }
        }
        app.PostToMainThread([&d, text]() {
            if (!d.quickCreateDescUserEdited && !text->empty()) {
                AssignBuf(d.quickCreateDescBuf, kQcDescBufCap, *text);
            }
            d.quickCreateCtxGenerating = false;
        });
    });
}

// First-frame-open seed: allocate buffers, reset submit state, seed project/type
// from the last-ticket draft heuristics, then apply (and clear) any prefills the
// `issue.quick_create.open` command args carried.
void SeedOnOpen(AppController& app, UiDrawSession& d) {
    const IssueDraft heuristics = app.BuildDraftFromLastTicket(d.cfg);
    d.quickCreateDraft = IssueDraft{};
    d.quickCreateDraft.ProjectKey = heuristics.ProjectKey;
    d.quickCreateDraft.IssueTypeId = heuristics.IssueTypeId;
    d.quickCreateDraft.IssueTypeName = heuristics.IssueTypeName;
    if (!d.quickCreateArgProject.empty()) {
        d.quickCreateDraft.ProjectKey = d.quickCreateArgProject;
    }
    if (!d.quickCreateArgIssueType.empty()) {
        // Resolve id-or-name against the catalog options; unresolved falls back to name-only
        // (the create payload builder resolves names server-side where possible).
        d.quickCreateDraft.IssueTypeId.clear();
        d.quickCreateDraft.IssueTypeName = d.quickCreateArgIssueType;
        for (const TrackerField& f : app.GetAvailableFields()) {
            if (f.Id != "issuetype") {
                continue;
            }
            for (const TrackerFieldOption& opt : f.AllowedValueOptions) {
                if (opt.Id == d.quickCreateArgIssueType || opt.Value == d.quickCreateArgIssueType) {
                    d.quickCreateDraft.IssueTypeId = opt.Id;
                    d.quickCreateDraft.IssueTypeName = opt.Value;
                    break;
                }
            }
            break;
        }
    }
    AssignBuf(d.quickCreateSummaryBuf, kSummaryBufCap, d.quickCreateArgSummary);
    AssignBuf(d.quickCreateDescBuf, kQcDescBufCap, d.quickCreateArgDescription);
    // A description passed via args is caller content — the context prefill must not replace it.
    d.quickCreateDescUserEdited = !d.quickCreateArgDescription.empty();
    d.quickCreateArgSummary.clear();
    d.quickCreateArgDescription.clear();
    d.quickCreateArgProject.clear();
    d.quickCreateArgIssueType.clear();
    d.quickCreateError.clear();
    d.quickCreateInFlight = false;
    d.quickCreateQueueOffered = false;
    d.quickCreateCtxGenerating = false;
    d.quickCreateLastFetchedProjectKey = d.quickCreateDraft.ProjectKey;
}

void ResetAndClose(UiDrawSession& d) {
    d.showQuickCreateIssue = false;
    d.quickCreateDraft = IssueDraft{};
    d.quickCreateSummaryBuf.clear();
    d.quickCreateDescBuf.clear();
    d.quickCreateError.clear();
    d.quickCreateQueueOffered = false;
    d.quickCreateDescUserEdited = false;
}

// Fold the finished create future into toast / banner state. Runs even while the
// window is hidden so a submit-then-close still reports its outcome.
void PollCreateFuture(AppController& app, UiDrawSession& d) {
    if (!d.quickCreateInFlight || !d.quickCreateFuture.valid() ||
        d.quickCreateFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    const IssueCreateResult r = d.quickCreateFuture.get();
    d.quickCreateInFlight = false;
    if (r.Ok) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.quick_create", "Quick create"),
            SmatchetLocalization::Format("quickcreate.created", "Created %s", r.IssueKey.c_str()), ToastType::Success);
        ResetAndClose(d);
        return;
    }
    if (r.ErrorTransient) {
        // Connectivity-shaped failure — stage the draft in the offline queue instead.
        if (app.QueueCreateOffline(d.quickCreateDraft) > 0) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.quick_create", "Quick create"),
                SmatchetLocalization::T("quickcreate.queued_offline", "Offline detected; queued for replay."),
                ToastType::Info);
            ResetAndClose(d);
            return;
        }
        d.quickCreateError = "Create failed (" + r.Error + ") and queue offline failed.";
        return;
    }
    d.quickCreateError = r.Error;
    // Server-side required fields this popup doesn't render → point at the full flow.
    d.quickCreateQueueOffered = !r.MissingFieldIds.empty();
}

void OnSubmit(AppController& app, UiDrawSession& d) {
    d.quickCreateDraft.FieldValues["summary"] = BufText(d.quickCreateSummaryBuf);
    const std::string description = BufText(d.quickCreateDescBuf);
    if (description.empty()) {
        d.quickCreateDraft.FieldValues.erase("description");
    } else {
        d.quickCreateDraft.FieldValues["description"] = description;
    }
    const std::vector<std::string> missing = IssueDraftHelpers::MissingRequiredFields(
        d.quickCreateDraft, app.GetRequiredFieldSet(d.quickCreateDraft.ProjectKey, d.quickCreateDraft.IssueTypeId,
                                                    d.quickCreateDraft.IssueTypeName));
    if (!missing.empty()) {
        const std::vector<std::string> names = IssueDraftHelpers::MapFieldIdsToNames(missing, app.GetAvailableFields());
        d.quickCreateError = "Missing required fields: " + JoinStrings(names, ", ");
        d.quickCreateQueueOffered = true;
        return;
    }
    d.quickCreateError.clear();
    d.quickCreateQueueOffered = false;
    d.quickCreateFuture = app.CreateIssueAsync(d.quickCreateDraft);
    d.quickCreateInFlight = true;
}

// Project row: the shared hybrid picker; a change kicks the per-project catalog
// refetch so required-fields/create-meta cover the new project before submit.
void DrawProjectRow(AppController& app, UiDrawSession& d) {
    ImGui::TextUnformatted(SmatchetLocalization::T("quickcreate.project", "Project"));
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const TrackerConfig& cfg = d.cfg;
    std::string backendKind;
    std::string endpoint;
    SmatchetProjectPicker::ResolveBackendKindAndEndpoint(cfg, backendKind, endpoint);
    std::string selected = d.quickCreateDraft.ProjectKey;
    if (!SmatchetProjectPicker::Draw("quick_create_project", d.quickCreateProjectPickerState, app, backendKind,
                                     endpoint, selected)) {
        return;
    }
    d.quickCreateDraft.ProjectKey = selected;
    if (selected.empty() || selected == d.quickCreateLastFetchedProjectKey) {
        return;
    }
    d.quickCreateLastFetchedProjectKey = selected;
    const TrackerConfig refetchCfg = cfg;
    app.LaunchBackgroundTask([&app, refetchCfg, selected]() {
        try {
            app.RefreshFieldCatalog(refetchCfg, selected);
        } catch (...) {
            // RefreshFieldCatalog logs internally; never let an escape cross the worker boundary.
            LOG_DEBUG("QuickCreateIssueUi: catalog refresh worker caught exception");
        }
    });
}

// Issue-type row: plain combo over the catalog's issuetype options; falls back to
// the seeded name (config default) when the catalog carries no options yet.
void DrawIssueTypeRow(AppController& app, UiDrawSession& d) {
    ImGui::TextUnformatted(SmatchetLocalization::T("quickcreate.type", "Type"));
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const TrackerField* typeField = nullptr;
    for (const TrackerField& f : app.GetAvailableFields()) {
        if (f.Id == "issuetype") {
            typeField = &f;
            break;
        }
    }
    const std::string& preview = d.quickCreateDraft.IssueTypeName;
    if (typeField == nullptr || typeField->AllowedValueOptions.empty()) {
        ImGui::TextDisabled("%s", preview.empty()
                                      ? SmatchetLocalization::T("quickcreate.type_none", "(no issue types cached)")
                                      : preview.c_str());
        return;
    }
    if (!ImGui::BeginCombo("##quickcreate_type", preview.empty() ? "(choose)" : preview.c_str())) {
        return;
    }
    for (const TrackerFieldOption& opt : typeField->AllowedValueOptions) {
        const bool isSelected = (opt.Id == d.quickCreateDraft.IssueTypeId) || (opt.Value == preview);
        if (ImGui::Selectable(opt.Value.c_str(), isSelected)) {
            d.quickCreateDraft.IssueTypeId = opt.Id;
            d.quickCreateDraft.IssueTypeName = opt.Value;
        }
    }
    ImGui::EndCombo();
}

// Error banner + Create/Cancel + refresh-context, with Ctrl+Enter submit / Esc cancel.
void DrawActions(AppController& app, UiDrawSession& d) {
    if (!d.quickCreateError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", d.quickCreateError.c_str());
        ImGui::PopTextWrapPos();
        if (d.quickCreateQueueOffered) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T(
                                          "quickcreate.full_flow_hint",
                                          "Fields beyond this popup are required — use \"+ New issue\" in the grid."));
        }
    }
    ImGui::Separator();

    const bool canSubmit =
        !d.quickCreateInFlight && !BufText(d.quickCreateSummaryBuf).empty() && !d.quickCreateDraft.ProjectKey.empty();
    if (!canSubmit) {
        ImGui::BeginDisabled();
    }
    const bool submitClicked =
        ImGui::Button(d.quickCreateInFlight ? SmatchetLocalization::T("quickcreate.creating", "Creating…")
                                            : SmatchetLocalization::T("common.create", "Create"));
    if (!canSubmit) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    const bool cancelClicked = ImGui::Button(SmatchetLocalization::T("common.cancel", "Cancel"));
    ImGui::SameLine();
    const bool refreshDisabled = d.quickCreateCtxGenerating || d.quickCreateDescUserEdited;
    if (refreshDisabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(SmatchetLocalization::T("quickcreate.refresh_context", "Refresh context"))) {
        LaunchContextPrefill(app, d);
    }
    if (refreshDisabled) {
        ImGui::EndDisabled();
        if (d.quickCreateDescUserEdited && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s", SmatchetLocalization::T("quickcreate.refresh_locked", "Description edited — refresh disabled."));
        }
    }

    const bool ctrlHeld = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    if (submitClicked || (canSubmit && ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Enter, false))) {
        OnSubmit(app, d);
    }
    if (!d.quickCreateInFlight && (cancelClicked || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ResetAndClose(d);
    }
}

} // namespace

void SmatchetQuickCreateIssueUi_Draw(AppController& app, UiDrawSession& d) {
    PollCreateFuture(app, d);

    if (!d.showQuickCreateIssue) {
        return;
    }

    if (d.quickCreateOpenLatch) {
        SeedOnOpen(app, d);
        LaunchContextPrefill(app, d);
    }

    ImGui::SetNextWindowSize(ImVec2(580, 520), ImGuiCond_FirstUseEver);
    SmatchetWindowExpand::BeginWindow(d, "Quick Create Issue");
    if (!ImGui::Begin("Quick Create Issue", &d.showQuickCreateIssue)) {
        d.quickCreateOpenLatch = false;
        ImGui::End();
        return;
    }
    SmatchetWindowExpand::DrawToggle(d);

    DrawProjectRow(app, d);
    DrawIssueTypeRow(app, d);

    ImGui::TextUnformatted(SmatchetLocalization::T("quickcreate.summary", "Summary"));
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (d.quickCreateSummaryBuf.size() < kSummaryBufCap) {
        d.quickCreateSummaryBuf.assign(kSummaryBufCap, '\0');
    }
    if (d.quickCreateOpenLatch) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##quickcreate_summary", d.quickCreateSummaryBuf.data(), d.quickCreateSummaryBuf.size());

    ImGui::TextUnformatted(SmatchetLocalization::T("quickcreate.description", "Description"));
    if (d.quickCreateCtxGenerating && BufText(d.quickCreateDescBuf).empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("quickcreate.context_generating", "(gathering context…)"));
    }
    if (d.quickCreateDescBuf.size() < kQcDescBufCap) {
        d.quickCreateDescBuf.assign(kQcDescBufCap, '\0');
    }
    if (ImGui::InputTextMultiline("##quickcreate_desc", d.quickCreateDescBuf.data(), d.quickCreateDescBuf.size(),
                                  ImVec2(-1.0f, 240.0f))) {
        d.quickCreateDescUserEdited = true;
    }

    DrawActions(app, d);

    d.quickCreateOpenLatch = false;
    ImGui::End();
}

#undef ImGui
