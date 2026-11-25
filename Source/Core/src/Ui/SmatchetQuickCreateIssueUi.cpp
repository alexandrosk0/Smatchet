// Quick-create issue popup. See SmatchetQuickCreateIssueUi.h.
// Dual-target: no GL/GLFW here; the engine-context prefill degrades to an empty
// description when no host snapshot exists (standalone build).

#include "SmatchetQuickCreateIssueUi.h"

// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=popup needs the create pipeline (CreateIssueAsync/QueueCreateOffline/GetRequiredFieldSet/BuildDraftFromLastTicket) plus SmatchetProjectPicker::Draw which takes AppController&, same facet set as the grandfathered SmatchetNewIssueDraftUi.cpp; owner=quick-create-issue-unreal-context; revisit=2027-11-30)
#include "AppController.h"
#include "ConfigManager.h"
#include "DraftStore.h"
#include "Diagnostics/EngineContextFormat.h"
#include "Diagnostics/EngineHostContext.h"
#include "FilterDropdown.h"
#include "TrackerOptionDropdown.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "RecentSelections.h"
#include "SmatchetLocalization.h"
#include "SmatchetProjectPicker.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "StringUtil.h"

#include "imgui.h"
#include "imgui_internal.h"
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

// What is typed here is kept in the draft store (DraftStore.h), so closing the window, a restart or a crash
// does not lose it (INPUT_UX_AUDIT.md #51). One popup, so one scope. Only the two texts are kept: the
// project and type come from the open's own seed. The description is kept only once the user edited it:
// the context prefill writes it otherwise, and runs again at each open.
constexpr const char* kQuickCreateSummaryDraftSurface = "quickcreate.summary";
constexpr const char* kQuickCreateDescriptionDraftSurface = "quickcreate.description";
// Set when a create of the kept text failed in a way that may still have created the issue.
constexpr const char* kQuickCreateMaybeCreatedSurface = "quickcreate.maybe_created";
constexpr const char* kQuickCreateDraftScope = "popup";

// The text last handed to the draft store, to notice a change without allocating each frame.
std::string s_quickCreateSyncedSummary;
std::string s_quickCreateSyncedDescription;
// The fields were filled from a kept entry at this open (the restored line shows).
bool s_quickCreateDraftRestored = false;
// The open passed its own text (summary= / description=): the kept entry is left alone until the user edits.
bool s_quickCreateSyncSuspended = false;

const char* BufCStr(const std::vector<char>& buf) { return buf.empty() ? "" : buf.data(); }

void SyncQuickCreateDraft(const UiDrawSession& d) {
    if (d.quickCreateInFlight || s_quickCreateSyncSuspended) {
        return; // being sent (the draft is erased when it lands), or the caller's text, not yet the user's
    }
    bool changed = false;
    const char* summary = BufCStr(d.quickCreateSummaryBuf);
    if (s_quickCreateSyncedSummary != summary) {
        s_quickCreateSyncedSummary = summary;
        smatchet::drafts::Put(kQuickCreateSummaryDraftSurface, kQuickCreateDraftScope, s_quickCreateSyncedSummary);
        changed = true;
    }
    const char* description = d.quickCreateDescUserEdited ? BufCStr(d.quickCreateDescBuf) : "";
    if (s_quickCreateSyncedDescription != description) {
        s_quickCreateSyncedDescription = description;
        smatchet::drafts::Put(kQuickCreateDescriptionDraftSurface, kQuickCreateDraftScope,
                              s_quickCreateSyncedDescription);
        changed = true;
    }
    if (changed && s_quickCreateSyncedSummary.empty() && s_quickCreateSyncedDescription.empty()) {
        // Nothing kept any more, so nothing a later draft could be mistaken for.
        smatchet::drafts::Erase(kQuickCreateMaybeCreatedSurface, kQuickCreateDraftScope);
    }
}

void EraseQuickCreateDraft() {
    s_quickCreateDraftRestored = false;
    if (s_quickCreateSyncSuspended) {
        return; // the caller's text, never edited: a kept draft this open did not show stays kept
    }
    smatchet::drafts::Erase(kQuickCreateSummaryDraftSurface, kQuickCreateDraftScope);
    smatchet::drafts::Erase(kQuickCreateDescriptionDraftSurface, kQuickCreateDraftScope);
    smatchet::drafts::Erase(kQuickCreateMaybeCreatedSurface, kQuickCreateDraftScope);
    s_quickCreateSyncedSummary.clear();
    s_quickCreateSyncedDescription.clear();
}

// At open, after the seed: puts back a kept summary / description, unless the open passed its own text
// (`summary=` / `description=` arguments are what the caller wants in the fields). Then neither comes back,
// and the kept entry stays as it is until the user edits a field.
void RestoreQuickCreateDraft(UiDrawSession& d, bool openPassedText) {
    s_quickCreateDraftRestored = false;
    s_quickCreateSyncSuspended = openPassedText;
    if (openPassedText) {
        return;
    }
    smatchet::drafts::DraftEntry kept;
    if (smatchet::drafts::Get(kQuickCreateSummaryDraftSurface, kQuickCreateDraftScope, kept)) {
        AssignBuf(d.quickCreateSummaryBuf, kSummaryBufCap, kept.Text);
        s_quickCreateDraftRestored = true;
    }
    if (smatchet::drafts::Get(kQuickCreateDescriptionDraftSurface, kQuickCreateDraftScope, kept)) {
        AssignBuf(d.quickCreateDescBuf, kQcDescBufCap, kept.Text);
        d.quickCreateDescUserEdited = true; // typed by the user: the context prefill must not replace it
        s_quickCreateDraftRestored = true;
    }
    s_quickCreateSyncedSummary = BufCStr(d.quickCreateSummaryBuf);
    s_quickCreateSyncedDescription = d.quickCreateDescUserEdited ? BufCStr(d.quickCreateDescBuf) : "";
}

void LaunchContextPrefill(AppController& app, UiDrawSession& d);

// The "restored" line with its Clear button, above the fields. Clear empties both and gathers the context
// description again, as a fresh open does; the next sync erases the kept entry.
void DrawQuickCreateDraftNotice(AppController& app, UiDrawSession& d) {
    if (!s_quickCreateDraftRestored || d.quickCreateInFlight) {
        // Hidden while a create runs: Clear would empty the text being sent, and its result settles the entry.
        return;
    }
    smatchet::drafts::DraftEntry maybeCreated;
    if (smatchet::drafts::Get(kQuickCreateMaybeCreatedSurface, kQuickCreateDraftScope, maybeCreated)) {
        ImGui::TextColored(SmatchetTheme::GetActiveSemanticColors().WarningText, "%s",
                           SmatchetLocalization::T("quickcreate.draft_restored_maybe_created",
                                                   "Restored your unsaved issue. Its last Create may have gone "
                                                   "through: check the tracker before creating it again."));
    } else {
        ImGui::TextDisabled("%s",
                            SmatchetLocalization::T("quickcreate.draft_restored", "Restored your unsaved issue."));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(SmatchetLocalization::T("quickcreate.draft_clear", "Clear"))) {
        AssignBuf(d.quickCreateSummaryBuf, kSummaryBufCap, std::string());
        AssignBuf(d.quickCreateDescBuf, kQcDescBufCap, std::string());
        d.quickCreateDescUserEdited = false;
        s_quickCreateDraftRestored = false;
        smatchet::drafts::Erase(kQuickCreateMaybeCreatedSurface, kQuickCreateDraftScope);
        LaunchContextPrefill(app, d);
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

void ClearOpenArgs(UiDrawSession& d) {
    d.quickCreateArgSummary.clear();
    d.quickCreateArgDescription.clear();
    d.quickCreateArgProject.clear();
    d.quickCreateArgIssueType.clear();
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
        const std::shared_ptr<const std::vector<TrackerField>> catalog = app.GetAvailableFields();
        for (const TrackerField& f : *catalog) {
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
    RestoreQuickCreateDraft(d, !d.quickCreateArgSummary.empty() || !d.quickCreateArgDescription.empty());
    ClearOpenArgs(d);
    d.quickCreateError.clear();
    d.quickCreateInFlight = false;
    d.quickCreateQueueOffered = false;
    d.quickCreateCtxGenerating = false;
    d.quickCreateLastFetchedProjectKey = d.quickCreateDraft.ProjectKey;
}

const char* const kDiscardPopupId = "Discard this issue?##quickcreate_discard";

// The user typed something this popup would lose: a summary, or a description they edited
// (the engine-context prefill alone is not theirs).
bool QuickCreateHasUserContent(const UiDrawSession& d) {
    return !TrimCopy(BufText(d.quickCreateSummaryBuf)).empty() ||
           (d.quickCreateDescUserEdited && !TrimCopy(BufText(d.quickCreateDescBuf)).empty());
}

// A popup (a dropdown or the discard question) was open over the quick-create window at the end
// of the last frame. ImGui's keyboard-nav cancel closes that popup in NewFrame and gives the
// keyboard back to the window before it draws, so the same Esc would otherwise also reach the
// window and close it, or open the discard question again.
bool s_quickCreatePopupOpenLastFrame = false;

// Is a popup open that the window being drawn opened, directly or through another popup? A popup of some
// other window does not count: it must not swallow this window's next Esc. (ImGui's own Esc handling closes
// the top popup whoever owns it, but a foreign popup takes the focus, and ReadQuickCreateKeys reads no key
// unless this window has it, so one Esc cannot close both.)
bool PopupOpenOverCurrentWindow() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImGuiWindow* root = ImGui::GetCurrentWindow()->RootWindow;
    for (int i = 0; i < g.OpenPopupStack.Size; ++i) {
        for (const ImGuiWindow* w = g.OpenPopupStack[i].Window; w != nullptr; w = w->ParentWindow) {
            if (w->RootWindow == root) {
                return true;
            }
        }
    }
    return false;
}

// Keys for the popup, read before its fields draw so a field keeps its text: an InputText that
// is still active when a key reaches it acts on it itself. Only while the popup itself has the
// keyboard, and never for the Esc that just closed a dropdown or the discard question.
struct QuickCreateKeys {
    bool Submit = false;
    bool Escape = false;
};

QuickCreateKeys ReadQuickCreateKeys() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    QuickCreateKeys keys;
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy)) {
        return keys;
    }
    keys.Submit = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
                  ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter);
    keys.Escape = !keys.Submit && !s_quickCreatePopupOpenLastFrame && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool fieldActive =
        g.ActiveId != 0 && g.ActiveIdWindow != nullptr && g.ActiveIdWindow->RootWindow == g.CurrentWindow->RootWindow;
    if (fieldActive && (keys.Submit || keys.Escape)) {
        ImGui::ClearActiveID();
    }
    if (keys.Submit) {
        g.NavActivateId = 0;
        g.NavActivateDownId = 0;
        g.NavActivatePressedId = 0;
    }
    return keys;
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
        EraseQuickCreateDraft();
        ResetAndClose(d);
        return;
    }
    if (IsOfflineQueueableFailure(r)) {
        // The tracker provably did not create it — stage the draft in the offline queue instead.
        std::string queueRefusal;
        if (app.QueueCreateOffline(d.quickCreateDraft, queueRefusal) > 0) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.quick_create", "Quick create"),
                SmatchetLocalization::T("quickcreate.queued_offline", "Offline detected; queued for replay."),
                ToastType::Info);
            EraseQuickCreateDraft();
            ResetAndClose(d);
            return;
        }
        d.quickCreateError = "Create failed (" + r.Error + ") and queue offline failed." +
                             (queueRefusal.empty() ? std::string() : " " + queueRefusal);
        return;
    }
    // A create that may have landed is never queued: resending it could create the issue twice. The kept
    // text stays, marked, so a later open says to check before creating it again. Not the caller's text
    // (sync suspended): the kept draft is another one, never sent.
    if (IsAmbiguousCreateFailure(r) && !s_quickCreateSyncSuspended) {
        smatchet::drafts::Put(kQuickCreateMaybeCreatedSurface, kQuickCreateDraftScope, "1");
    }
    d.quickCreateError = IsAmbiguousCreateFailure(r) ? r.Error + " " + kAmbiguousCreateHint : r.Error;
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
        const std::vector<std::string> names =
            IssueDraftHelpers::MapFieldIdsToNames(missing, *app.GetAvailableFields());
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
    // typeField points into this snapshot, so it is held for the rest of the draw.
    const std::shared_ptr<const std::vector<TrackerField>> catalog = app.GetAvailableFields();
    const TrackerField* typeField = nullptr;
    for (const TrackerField& f : *catalog) {
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
    if (!ImGui::BeginCombo("##quickcreate_type", preview.empty() ? "(choose)" : preview.c_str(),
                           ImGuiComboFlags_HeightLargest)) {
        return;
    }
    // The shared dropdown body; recents share the "issuetype" scope with the grid and the draft row.
    std::vector<smatchet::dropdown::Item> items;
    items.reserve(typeField->AllowedValueOptions.size());
    for (const TrackerFieldOption& opt : typeField->AllowedValueOptions) {
        smatchet::dropdown::Item item = smatchet::dropdown::ItemForOption(opt);
        item.Checked = (opt.Id == d.quickCreateDraft.IssueTypeId) || (opt.Value == preview);
        items.push_back(item);
    }
    const std::string recentsScope = smatchet::recents::MakeScopeKey(app.FocusedCacheBackendKey(), "issuetype");
    const smatchet::dropdown::Event ev =
        smatchet::dropdown::DrawBody(items, smatchet::dropdown::SingleSelect(recentsScope));
    if (ev.Kind == smatchet::dropdown::EventKind::Picked && ev.ItemIndex >= 0) {
        // `preview` aliases IssueTypeName: nothing reads it past this point.
        const TrackerFieldOption& opt = typeField->AllowedValueOptions[static_cast<size_t>(ev.ItemIndex)];
        d.quickCreateDraft.IssueTypeId = opt.Id;
        d.quickCreateDraft.IssueTypeName = opt.Value;
    }
    ImGui::EndCombo();
}

// Error banner + Create/Cancel + refresh-context, with Ctrl+Enter submit / Esc cancel.
void DrawActions(AppController& app, UiDrawSession& d, const QuickCreateKeys& keys, bool summaryEnter) {
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

    // Create button, Ctrl+Enter anywhere in the popup, or Enter in Summary: one submit path. A
    // key press that cannot create yet says why, the way the disabled Create button would.
    if (submitClicked || (canSubmit && (keys.Submit || summaryEnter))) {
        OnSubmit(app, d);
    } else if (!d.quickCreateInFlight && (keys.Submit || summaryEnter)) {
        d.quickCreateError = SmatchetLocalization::T("quickcreate.needs_summary_and_project",
                                                     "Type a summary and pick a project to create the issue.");
    }
    // Cancel and Esc close an empty popup at once and ask first when something is typed.
    if (!d.quickCreateInFlight && (cancelClicked || keys.Escape)) {
        if (QuickCreateHasUserContent(d)) {
            ImGui::OpenPopup(kDiscardPopupId);
        } else {
            ResetAndClose(d);
        }
    }
    if (ImGui::BeginPopup(kDiscardPopupId)) {
        ImGui::TextUnformatted(SmatchetLocalization::T("quickcreate.discard_confirm",
                                                       "Discard this issue? The summary and description you typed "
                                                       "will be lost."));
        if (ImGui::Button(SmatchetLocalization::T("quickcreate.discard", "Discard"))) {
            ImGui::CloseCurrentPopup();
            EraseQuickCreateDraft();
            ResetAndClose(d);
        }
        ImGui::SameLine();
        if (ImGui::Button(SmatchetLocalization::T("quickcreate.keep_editing", "Keep editing"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace

void SmatchetQuickCreateIssueUi_Draw(AppController& app, UiDrawSession& d) {
    PollCreateFuture(app, d);

    if (!d.showQuickCreateIssue) {
        return;
    }

    if (d.quickCreateOpenLatch) {
        if (d.quickCreateInFlight && d.quickCreateFuture.valid()) {
            // Reopened while its create still runs: show what was sent, as it was, and keep polling. Seeding
            // would bring the kept text back over it and drop the running create, so its result could not settle
            // the entry and a second submit could file the issue twice. The open's own arguments are dropped.
            if (!d.quickCreateArgSummary.empty() || !d.quickCreateArgDescription.empty() ||
                !d.quickCreateArgProject.empty() || !d.quickCreateArgIssueType.empty()) {
                LOG_INFO("QuickCreateIssueUi: open arguments ignored, a create is still running");
            }
            ClearOpenArgs(d);
        } else {
            SeedOnOpen(app, d);
            LaunchContextPrefill(app, d);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(580, 520), ImGuiCond_FirstUseEver);
    SmatchetWindowExpand::BeginWindow(d, "Quick Create Issue");
    if (!ImGui::Begin("Quick Create Issue", &d.showQuickCreateIssue)) {
        s_quickCreatePopupOpenLastFrame = false;
        d.quickCreateOpenLatch = false;
        ImGui::End();
        return;
    }
    SmatchetWindowExpand::DrawToggle(d);
    const QuickCreateKeys keys = ReadQuickCreateKeys();

    DrawQuickCreateDraftNotice(app, d);
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
    const bool summaryEnter = ImGui::InputText("##quickcreate_summary", d.quickCreateSummaryBuf.data(),
                                               d.quickCreateSummaryBuf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited() && !d.quickCreateInFlight) {
        // The user's text now. Not while a create runs: its result settles the entry as it was when sent.
        s_quickCreateSyncSuspended = false;
    }

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
        if (!d.quickCreateInFlight) {
            s_quickCreateSyncSuspended = false;
        }
    }

    DrawActions(app, d, keys, summaryEnter);
    if (d.showQuickCreateIssue) {
        SyncQuickCreateDraft(d);
    }
    s_quickCreatePopupOpenLastFrame = PopupOpenOverCurrentWindow();

    d.quickCreateOpenLatch = false;
    ImGui::End();
}

#undef ImGui
