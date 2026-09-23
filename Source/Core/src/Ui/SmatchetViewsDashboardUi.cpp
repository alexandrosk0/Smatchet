// SMATCHET_DEVIATION(rule=tu-line-ceiling; reason=column-view-save-simplification replaced the
// editor's scattered dirty-flag/buffer fields with one ViewDraft, net negative on the functional
// churn but the doc-comment expansion explaining the new invariants pushed the file ~15 lines
// past the ceiling; a companion-TU split (action methods vs draw functions) is a reasonable
// follow-up but out of scope for this change; owner=orchestrator; revisit=next touch of this file)
// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "SmatchetUI.h"

#include "SmatchetViewsDashboardUi_detail.h"
#include "SmatchetAutocompleteUi.h"
#include "AppController.h"
#include "Views.h"
#include "ViewColumnsPure.h"
#include "ConfigManager.h"
#include "ConfigSaveWorker.h"
#include "JiraBackendInstancesPure.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "SmatchetToast.h"
#include "SmatchetLocalization.h"
#include "Ui/SmatchetDestructiveButton.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"
#include "Logger.h"
#include "Ui/SmatchetIconButtons.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

using SmatchetViewsDashboardUiDetail::CategorizeAvailableFields;
using SmatchetViewsDashboardUiDetail::CategorizedFields;
using SmatchetViewsDashboardUiDetail::PrettyColumnLabel;

namespace {

// One enum value per editor tab. Persisted in UiDrawSession::viewsActiveTab.
enum ViewsEditorTab : int {
    Tab_Filter = 0,
    Tab_Fields = 1,
    Tab_Columns = 2,
    Tab_Sort = 3,
};

// Load the Views editor's draft from a saved view (column-view-save-simplification):
// d.viewDraft becomes a full editable COPY of `view`, and the ImGui text buffers are
// re-seeded as display mirrors of it. No separate selected-field set, column-order buffer,
// or dirty flag — the Fields/Columns/Sort tabs edit the draft's Columns and SortSpecs
// directly, and "dirty" is ViewDraftDiffersFromSaved(d.viewDraft, *activeView, 0.5f),
// computed fresh every frame in drawViewsEditorHeader. Resets autocomplete state too.
void LoadBuffersFromView(UiDrawSession& d, const ViewDefinition& view) {
    // [temp-debug] Instrument the reseed for UI test debugging. fprintf(stderr, ...) rather
    // than LOG_INFO: Logger writes to an in-memory ring + an opt-in file sink, never to
    // stdout/stderr, so it never reaches the bucket-E CI harness's captured child log.
    std::fprintf(stderr, "[LoadBuffersFromView] called with view.Id='%s', current d.viewDraftId='%s'\n",
                 view.Id.c_str(), d.viewDraftId.c_str());

    std::memset(d.fieldSearchBuf, 0, sizeof(d.fieldSearchBuf));
    d.viewJqlEditor.jqlAcpApplyReplace = false;
    d.viewJqlEditor.jqlAcpReplaceStart = -1;
    d.viewJqlEditor.jqlAcpReplaceEnd = -1;
    d.viewJqlEditor.jqlAcpReplaceText.clear();
    d.viewJqlEditor.jqlAcpListSelected = -1;
    d.viewJqlEditor.jqlAcpLastCursor = 0;
    d.viewJqlEditor.jqlAcpLastSelectionStart = 0;
    d.viewJqlEditor.jqlAcpLastSelectionEnd = 0;
    d.viewJqlEditor.jqlAcpWantsJqlInputFocus = false;
    d.viewJqlEditor.jqlAcpScrollToSelected = false;
    d.viewJqlEditor.jqlAcpCaretSnapFramesRemaining = 0;
    d.viewJqlEditor.jqlWantsApplyFromEnter = false;
    // Reset async user-search state too — otherwise a prior view's in-flight results / pending
    // dispatch bleed into the freshly loaded view (#5). Mirrors the early-return resets in
    // TrackerQueryAcp_TickDebouncedUserSearch; the running future (if any) is dropped as stale by
    // the next poll (armed id moved on) rather than destroyed here (would block the UI thread).
    d.viewJqlEditor.jqlAcpAsyncUserItems.clear();
    d.viewJqlEditor.jqlAcpAsyncUserError.clear();
    d.viewJqlEditor.jqlAcpUserSearchQuery.clear();
    d.viewJqlEditor.jqlAcpUserSearchFireAt = 0.0;
    d.viewJqlEditor.jqlAcpUserSearchInFlightId = 0;

    std::fprintf(stderr, "[LoadBuffersFromView] about to assign d.viewDraft = view\n");
    d.viewDraft = view;
    std::fprintf(stderr, "[LoadBuffersFromView] assigned d.viewDraft; now d.Columns has %zu entries\n",
                 d.viewDraft.Columns.size());
    d.viewDraftId = view.Id;
    std::fprintf(stderr, "[LoadBuffersFromView] complete; d.viewDraftId='%s'\n", d.viewDraftId.c_str());
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewNameBuf, view.Name);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlEditor.buf, view.Jql);
    d.selectedColumnOrderIndex = -1;
    d.viewsHasOriginalSnapshot = false; // grid-side-only field; unused by the editor, kept clean
    d.viewsKeyboardReorderRow = -1;
    d.viewsTitleEditing = false;
}

} // namespace

// Shared per-frame state for the drawViewsDashboardWindow section helpers (function-size
// decomposition). Constructed once at the top of drawViewsDashboardWindow from orchestrator-
// owned locals; passed by reference into each section helper. The action closures (apply,
// discard, activate, create) are captured once here so the tab bodies stay layout-only.
struct ViewsDashboardDrawCtx {
    AppController& app;
    UiDrawSession& d;
    const ViewsStore& store;
    const ViewDefinition* activeView;
    // Sidebar layout, computed once in the orchestrator and consumed by drawViewsSidebar.
    float sidebarWidth;
    std::function<void()> applyAndSync;
    std::function<void()> discardChanges;
    std::function<void()> createNewView;
    std::function<void(const std::string&)> requestActivate;
    std::function<void(const std::string&)> activateView;
};

void SmatchetUI::drawViewsSidebar(ViewsDashboardDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    const ViewsStore& store = ctx.store;
    const ViewDefinition* activeView = ctx.activeView;

    ImGui::BeginChild("ViewsSidebar", ImVec2(ctx.sidebarWidth, 0), true);
    {
        ImGui::TextUnformatted("Views");
        ImGui::SameLine();
        const float btnW = ImGui::CalcTextSize("+ New").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btnW);
        if (ImGui::SmallButton("+ New")) {
            ctx.createNewView();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##ViewsSidebarSearch", "Search views...", d.viewsSidebarSearchBuf,
                                 sizeof(d.viewsSidebarSearchBuf));
        ImGui::Spacing();

        ImGui::BeginChild("ViewsSidebarList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), false);
        for (const auto& view : store.Views) {
            if (!SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive(view.Name, d.viewsSidebarSearchBuf)) {
                continue;
            }
            const bool isActive = (view.Id == activeView->Id);
            ImGui::PushID(view.Id.c_str());
            std::string label;
            label.reserve(view.Name.size() + 4);
            label += isActive ? "* " : "  ";
            label += view.Name;
            if (ImGui::Selectable(label.c_str(), isActive, ImGuiSelectableFlags_AllowDoubleClick)) {
                ctx.requestActivate(view.Id);
            }
            // Hover-revealed context menu for rename / duplicate / delete.
            if (ImGui::BeginPopupContextItem("##ViewRowMenu")) {
                if (ImGui::MenuItem("Rename...")) {
                    // DR13b: the inline title editor is bound to the ACTIVE view, so a rename must
                    // switch to the right-clicked row first. A dirty editor defers that switch behind
                    // the discard-confirm; arming the rename here would rename the still-active
                    // (wrong) view. Begin editing only once a clean switch has actually landed.
                    if (view.Id == activeView->Id) {
                        d.viewsTitleEditing = true;
                    } else {
                        ctx.requestActivate(view.Id);
                        if (!d.viewsShowDiscardConfirm) {
                            d.viewsTitleEditing = true;
                        }
                    }
                }
                if (ImGui::MenuItem("Duplicate")) {
                    // DEFERRED create (Pillar 3 crash fix): creating here would reallocate
                    // store.Views WHILE this loop iterates it (and dangle ctx.activeView for
                    // the rest of the frame). Copy the payload now; apply next frame.
                    ViewDefinition dup = view;
                    dup.Id.clear();
                    dup.Name = view.Name + " (copy)";
                    d.viewsPendingCreate = true;
                    d.viewsPendingCreatePayload = std::move(dup);
                    d.viewsPendingCreateToastTitle = "View duplicated";
                    d.viewsPendingCreateToastMs = 1500;
                    d.viewsPendingCreateAdoptCfg = false;
                }
                ImGui::Separator();
                const bool canDelete = store.Views.size() > 1;
                if (!canDelete) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Delete view...")) {
                    // DR13b: delete targets the RIGHT-CLICKED view id, independent of which view is
                    // active or whether the active view's editor is dirty. Previously this force-
                    // activated the row (which a dirty editor defers) yet the confirm + latch still
                    // targeted the active view — deleting the wrong view. Deleting a non-active view
                    // leaves the current editing context untouched.
                    d.viewsPendingDeleteId = view.Id;
                    d.viewsShowDeleteConfirm = true;
                }
                if (!canDelete) {
                    ImGui::EndDisabled();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Footer line: count of saved views.
        ImGui::TextDisabled("%zu view%s", store.Views.size(), store.Views.size() == 1 ? "" : "s");
    }
    ImGui::EndChild();
}

// Slice 6 — the mobile drawer's saved-views section. Rebuilds the same ctx +
// action closures drawViewsDashboardWindow does and reuses drawViewsSidebar
// verbatim, so the drawer's view list, search, rename/duplicate/delete menu,
// and active marker all match the desktop Views sidebar. requestActivate is
// wrapped to also close the drawer once a view is picked. sidebarWidth is passed
// by the caller — the drawer path supplies its live content-region width; the
// modal-only path (drawMobileViewsModals) passes 0 because GetContentRegionAvail
// must not be queried there (it can run outside a live window → debug-ImGui
// assert), and drawViewsModals never reads sidebarWidth anyway.
ViewsDashboardDrawCtx SmatchetUI::buildMobileViewsCtx(AppController& app, UiDrawSession& d,
                                                      const ViewDefinition* activeView, float sidebarWidth) {
    const ViewsStore& store = ViewState.GetStoreMutable();
    auto applyAndSync = [this, &app, &d, activeView]() { viewsApplyAndSync(app, d, activeView); };
    auto discardChanges = [this, &d]() { viewsDiscardChanges(d); };
    auto activateView = [this, &app, &d](const std::string& id) { viewsActivateView(app, d, id); };
    auto requestActivate = [this, &app, &d, activeView](const std::string& id) {
        viewsRequestActivate(app, d, activeView, id);
        // Close the drawer only on an immediate (clean) activation. A dirty switch
        // latches viewsShowDiscardConfirm and must keep the drawer open so the
        // confirm flow stays coherent (#1117) — the shell-level modal renders on top.
        if (!d.viewsShowDiscardConfirm) {
            d.mobileDrawerOpen = false;
        }
    };
    auto createNewView = [this, &app, &d, activeView]() { viewsCreateNewView(app, d, activeView); };

    return ViewsDashboardDrawCtx{
        app,         d, store, activeView, sidebarWidth, applyAndSync, discardChanges, createNewView, requestActivate,
        activateView};
}

void SmatchetUI::drawMobileDrawerViews(AppController& app, UiDrawSession& d) {
    // [temp-debug] Log every frame to ensure function is called
    static int callCount = 0;
    ++callCount;
    std::fprintf(stderr, "[drawMobileDrawerViews] frame %d: called\n", callCount);

    ViewState.EnsureLoaded(d.cfg);
    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (!activeView) {
        std::fprintf(stderr, "[drawMobileDrawerViews] frame %d: activeView is nullptr\n", callCount);
        ImGui::TextDisabled("No views available.");
        return;
    }
    // Reload the draft whenever the active view id changed underneath us (column-view-save-
    // simplification: a mere layout drift — e.g. the grid autosaving a width/order change to
    // the same view — no longer force-reloads a possibly-mid-edit draft here; only an actual
    // view switch does. See d.viewDraft's doc comment for why this is safe: layout autosaves
    // independently of the editor's draft, and the two resynchronize on the next activate).
    std::fprintf(stderr, "[drawMobileDrawerViews] frame %d: d.viewDraftId='%s', activeView->Id='%s'\n", callCount,
                 d.viewDraftId.c_str(), activeView->Id.c_str());
    if (d.viewDraftId != activeView->Id) {
        std::fprintf(stderr, "[drawMobileDrawerViews] frame %d: IDs DO NOT MATCH — calling LoadBuffersFromView\n",
                     callCount);
        LoadBuffersFromView(d, *activeView);
    } else {
        std::fprintf(stderr, "[drawMobileDrawerViews] frame %d: IDs match — skipping LoadBuffersFromView\n",
                     callCount);
    }

    ViewsDashboardDrawCtx ctx = buildMobileViewsCtx(app, d, activeView, ImGui::GetContentRegionAvail().x);
    drawViewsSidebar(ctx);
}

// P1.2 — touch view quick-switcher band: a horizontal-scroll strip of saved-view tabs shown in
// the grid area (between the app bar and the content dock) so switching saved views needs no
// drawer trip or desktop menu bar. The active view is highlighted; a tap routes through the same
// dirty-aware viewsRequestActivate the drawer uses — a dirty switch latches the shell-level
// discard-confirm modal, kept rendering every frame by drawMobileViewsModals. A trailing "+"
// reuses viewsCreateNewView (deferred-create latch). Both helpers dereference *activeView, so the
// band is skipped when there is no active view. The chosen tab id is applied AFTER the loop so no
// store/activeView reference is used across a potential activation mutation. Mobile-only path.
void SmatchetUI::drawMobileViewQuickSwitcher(AppController& app, UiDrawSession& d, float bandHeight) {
    ViewState.EnsureLoaded(d.cfg);
    const ViewsStore& store = ViewState.GetStoreMutable();
    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (!activeView) {
        return;
    }
    const std::string& activeId = activeView->Id;

    std::string requestedId; // a tap latches the target id; activation runs after the loop
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, ImGui::GetStyle().ItemSpacing.y));
    if (ImGui::BeginChild("##MobileViewSwitcher", ImVec2(0.0f, bandHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        for (std::size_t i = 0; i < store.Views.size(); ++i) {
            const ViewDefinition& v = store.Views[i];
            if (i > 0) {
                ImGui::SameLine();
            }
            const bool active = (v.Id == activeId);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button(v.Name.c_str()) && v.Id != activeId) {
                requestedId = v.Id;
            }
            ImGui::PopID();
            if (active) {
                ImGui::PopStyleColor();
            }
        }
        if (!store.Views.empty()) {
            ImGui::SameLine();
        }
        if (ImGui::Button("+##MobileNewView")) {
            viewsCreateNewView(app, d, activeView); // deferred-create latch; consumed next frame
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (!requestedId.empty()) {
        viewsRequestActivate(app, d, activeView, requestedId);
    }
}

void SmatchetUI::drawMobileViewsModals(AppController& app, UiDrawSession& d) {
    // #1117: render the discard/delete-confirm popups regardless of drawer state. The drawer's
    // requestActivate latches viewsShowDiscardConfirm on a dirty switch, so the modal must be
    // driven from the always-rendered shell, not the drawer body. sidebarWidth is passed as 0:
    // this path can run outside a live window, so GetContentRegionAvail must not be queried, and
    // drawViewsModals never reads sidebarWidth.
    ViewState.EnsureLoaded(d.cfg);
    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (!activeView) {
        return;
    }
    ViewsDashboardDrawCtx ctx = buildMobileViewsCtx(app, d, activeView, 0.0f);
    drawViewsModals(ctx);
}

void SmatchetUI::drawViewsEditorHeader(ViewsDashboardDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;
    const ViewsStore& store = ctx.store;
    const ViewDefinition* activeView = ctx.activeView;

    // Dirty is DERIVED, never a stored flag (column-view-save-simplification): compared fresh
    // every frame against the live saved view, so it can never go stale or false-positive.
    const bool dirty = ViewDraftDiffersFromSaved(d.viewDraft, *activeView, 0.5f);

    // Title row. The buffer is a display mirror of d.viewDraft.Name (see its doc comment) —
    // committing writes straight into the draft, no separate dirty flag to set.
    if (d.viewsTitleEditing) {
        ImGui::SetNextItemWidth(-260.0f);
        if (ImGui::InputText("##ViewTitle", d.viewNameBuf, sizeof(d.viewNameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            d.viewsTitleEditing = false;
            d.viewDraft.Name = d.viewNameBuf;
        }
        if (ImGui::IsItemDeactivated()) {
            d.viewsTitleEditing = false;
            d.viewDraft.Name = d.viewNameBuf;
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(d.viewDraft.Name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename")) {
            d.viewsTitleEditing = true;
        }
        if (dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "  unsaved");
        }
    }

    // Action buttons on a separate row below the title for a more compact header.
    const bool disableDiscard = !dirty;
    if (disableDiscard) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Discard")) {
        ctx.discardChanges();
    }
    if (disableDiscard) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (SmatchetIconLeadingButton(ICON_FA_ARROWS_ROTATE, "Apply & Sync",
                                  "Apply changes and sync issues (Ctrl+Enter).")) {
        ctx.applyAndSync();
    }
    ImGui::SameLine();
    const bool disableDelete = (store.Views.size() <= 1);
    if (disableDelete) {
        ImGui::BeginDisabled();
    }
    SmatchetPushDestructiveButtonColors();
    if (ImGui::Button("Delete view")) {
        d.viewsPendingDeleteId = activeView->Id; // DR13b: header button deletes the active view.
        d.viewsShowDeleteConfirm = true;
    }
    SmatchetPopDestructiveButtonColors();
    if (disableDelete) {
        ImGui::EndDisabled();
    }

    // Sub-row: status / sync hints.
    ImGui::TextDisabled("Active view  ·  press Ctrl+Enter to apply  ·  Ctrl+N to create a new view");
    ImGui::Separator();
}

void SmatchetUI::drawViewsFilterTab(ViewsDashboardDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;

    if (ImGui::BeginTabItem("Filter")) {
        d.viewsActiveTab = Tab_Filter;
        ImGui::Spacing();
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(-FLT_MIN);
        // Buffer is a display mirror of d.viewDraft.Name — every keystroke lands directly on
        // the draft (column-view-save-simplification), no separate dirty flag to set.
        if (ImGui::InputText("##ViewNameInput", d.viewNameBuf, sizeof(d.viewNameBuf))) {
            d.viewDraft.Name = d.viewNameBuf;
        }

        ImGui::Spacing();
        const bool isPlane = smatchet::tracker::IsPlaneBackendType(d.cfg.TrackerType);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(isPlane ? "Plane filter" : "JQL");
        ImGui::SameLine();
        const std::string currentJql(d.viewJqlEditor.buf);
        const bool disableOpenJql = currentJql.empty() || d.cfg.Domain.empty() || isPlane;
        if (disableOpenJql) {
            ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton(isPlane ? "Open##Query" : "Open in browser")) {
            if (!isPlane) {
                // The buffer holds display names on Jira — hand the browser the id-canonical
                // query (names reverse-mapped) so the search matches what the view runs.
                app.OpenUrl(app.BuildJqlSearchUrl(d.cfg, TrackerQueryAcp_CanonicalQueryForApply(
                                                             d.cfg.TrackerType, app.GetAvailableFields(),
                                                             app.GetAvailableUsers(), d.viewJqlEditor, currentJql)));
            }
        }
        if (disableOpenJql) {
            ImGui::EndDisabled();
        }

        char beforeJql[sizeof(d.viewJqlEditor.buf)];
        std::memcpy(beforeJql, d.viewJqlEditor.buf, sizeof(beforeJql));
        SmatchetViewsDashboardUiDetail::DrawJqlQueryEditorEmbedded(app, d, d.viewJqlEditor);
        // Compare content, not just length — a same-length edit ("abc" -> "xyz") still counts (#6).
        // The cosmetic id->name rewrite consumes its flag here so it never touches the draft
        // (typing needs focus, the rewrite needs no focus — the two can't co-occur in a frame).
        const bool semanticRewrite = d.viewJqlEditor.jqlBufSemanticRewrite;
        d.viewJqlEditor.jqlBufSemanticRewrite = false;
        if (!semanticRewrite && std::strcmp(d.viewJqlEditor.buf, beforeJql) != 0) {
            // The buffer holds display names on Jira — persist the id-canonical query of
            // record (names reverse-mapped to account ids) into the draft, same conversion
            // the editor's Save used to apply once at commit time (BuildUpdatedView, now
            // gone — the draft IS what gets saved, so this must happen on every edit frame).
            d.viewDraft.Jql = TrackerQueryAcp_CanonicalQueryForApply(d.cfg.TrackerType, app.GetAvailableFields(),
                                                                     app.GetAvailableUsers(), d.viewJqlEditor,
                                                                     std::string(d.viewJqlEditor.buf));
        }
        ImGui::TextDisabled(isPlane
                                ? "field:value AND field:value  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list"
                                : "JQL tokens + catalog  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list");

        if (d.viewJqlEditor.jqlWantsApplyFromEnter) {
            d.viewJqlEditor.jqlWantsApplyFromEnter = false;
            ctx.applyAndSync();
        }

        ImGui::EndTabItem();
    }
}

namespace {

// One collapsible field group (System / Custom) with a per-field selection checkbox. Extracted from
// the renderFieldGroup lambda in drawViewsFieldsTab (over-100-line decomposition); behaviour-identical.
// `selectedFieldSet` is a per-frame LOCAL working set (drawViewsFieldsTab), not stored session
// state — the caller applies the result onto d.viewDraft.Columns after all groups are drawn
// (ApplyFieldSelectionToDraftColumns), so no dirty flag is set here; dirty is derived.
void DrawViewsFieldGroup(const char* groupName, const std::vector<const TrackerField*>& fields,
                         std::unordered_set<std::string>& selectedFieldSet) {
    if (fields.empty()) {
        return;
    }
    const size_t selectedInGroup = static_cast<size_t>(std::count_if(
        fields.begin(), fields.end(), [&](const TrackerField* f) { return f && selectedFieldSet.count(f->Id); }));
    const std::string label = std::string(groupName) + " (" + std::to_string(selectedInGroup) + "/" +
                              std::to_string(fields.size()) + ")###grp_" + groupName;
    if (!ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    for (const TrackerField* field : fields) {
        bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
        const std::string checkboxId = "##ViewField_" + field->Id;
        if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
            if (checked) {
                selectedFieldSet.insert(field->Id);
            } else {
                selectedFieldSet.erase(field->Id);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
    }
}

// The "Basic fields" group: the locked ID row plus the six core Jira fields in fixed order. Extracted
// from drawViewsFieldsTab during the over-100-line decomposition. Behaviour is identical.
void DrawViewsBasicFieldsGroup(const UiDrawSession& d, const std::vector<const TrackerField*>& basicFields,
                               std::unordered_set<std::string>& selectedFieldSet) {
    // Basic group: ID (always selected, locked) + the six core Jira fields.
    const bool hasVisibleId = SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive("id", d.fieldSearchBuf);
    if (basicFields.empty() && !hasVisibleId) {
        return;
    }
    // ID counts as 1; plus the selected fields in this group.
    const size_t selectedInGroup =
        1 + static_cast<size_t>(std::count_if(basicFields.begin(), basicFields.end(), [&](const TrackerField* f) {
            return f && selectedFieldSet.count(f->Id);
        }));
    const size_t total = basicFields.size() + 1;
    const std::string label =
        std::string("Basic fields (") + std::to_string(selectedInGroup) + "/" + std::to_string(total) + ")###grp_basic";
    if (!ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    bool idChecked = true;
    ImGui::BeginDisabled();
    ImGui::Checkbox("##ViewField_id", &idChecked);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("ID (locked)");
    const char* basicOrder[] = {"summary", "assignee", "priority", "status", "created", "updated"};
    for (const char* fid : basicOrder) {
        auto it = std::find_if(basicFields.begin(), basicFields.end(),
                               [&](const TrackerField* f) { return f && f->Id == fid; });
        if (it == basicFields.end() || !*it) {
            continue;
        }
        const TrackerField* field = *it;
        bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
        const std::string checkboxId = "##ViewField_" + field->Id;
        if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
            if (checked) {
                selectedFieldSet.insert(field->Id);
            } else {
                selectedFieldSet.erase(field->Id);
            }
        }
        ImGui::SameLine();
        ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
    }
}

// Apply a field-id selection back onto the draft's ordered Columns (column-view-save-
// simplification — replaces ReconcileEditingColumnOrder's std::unordered_set-iteration
// append, which produced hash-order, non-deterministic column placement for a newly checked
// field). A field dropped from `selected` loses its column (its position is simply gone, not
// reused); a field newly present in `selected` is appended at the END, deterministic for a
// single toggle. "id" is never touched — it isn't a field selection at all. Multiple fields
// added in the same frame (Select all visible) are still appended in whatever order
// `selected` (an unordered_set) iterates them in this run — not a per-run coin flip, just not
// alphabetical/insertion order; acceptable since the property this actually needs to hold is
// "the EXISTING columns never reshuffle," which this preserves exactly.
void ApplyFieldSelectionToDraftColumns(const std::unordered_set<std::string>& selected, ViewDefinition& draft) {
    draft.Columns.erase(std::remove_if(draft.Columns.begin(), draft.Columns.end(),
                                       [&](const ViewColumn& c) {
                                           if (c.Key == "id" || c.Key.compare(0, 6, "field:") != 0) {
                                               return false;
                                           }
                                           return selected.find(c.Key.substr(6)) == selected.end();
                                       }),
                        draft.Columns.end());
    std::unordered_set<std::string> present;
    present.reserve(draft.Columns.size());
    for (const auto& col : draft.Columns) {
        present.insert(col.Key);
    }
    for (const auto& fieldId : selected) {
        const std::string key = "field:" + fieldId;
        if (present.insert(key).second) {
            draft.Columns.push_back({key, 0.0f});
        }
    }
}

} // namespace

void SmatchetUI::drawViewsFieldsTab(ViewsDashboardDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;

    if (ImGui::BeginTabItem("Fields")) {
        d.viewsActiveTab = Tab_Fields;
        ImGui::Spacing();

        if (d.fieldCatalogLoading) {
            ImGui::TextDisabled("Loading available fields...");
        }

        // Per-frame LOCAL working set, derived from d.viewDraft.Columns (the source of
        // truth) — the toggle handlers / select-all / clear mutate it, and
        // ApplyFieldSelectionToDraftColumns writes the result back onto the draft once, at
        // the end of this function (column-view-save-simplification: no separately-stored
        // #views-field-uncheck selection set to keep in sync with a second column-order list).
        std::unordered_set<std::string> selectedFieldSet;
        for (const auto& col : d.viewDraft.Columns) {
            if (col.Key.compare(0, 6, "field:") == 0) {
                selectedFieldSet.insert(col.Key.substr(6));
            }
        }
        const auto& availableFields = app.GetAvailableFields();

        // Single pane: the column-order list lives in the Columns tab.
        const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();

        ImGui::BeginChild("ViewsFieldsAvailable", ImVec2(0, listHeight), true);
        {
            ImGui::TextUnformatted("Available");
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu)", availableFields.size());
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##ViewFieldSearch", "Search field id or name", d.fieldSearchBuf,
                                     sizeof(d.fieldSearchBuf));

            const CategorizedFields categorized = CategorizeAvailableFields(availableFields, d.fieldSearchBuf);
            const std::vector<const TrackerField*>& visibleFields = categorized.visible;
            const std::vector<const TrackerField*>& systemFields = categorized.system;
            const std::vector<const TrackerField*>& customFields = categorized.custom;
            const std::vector<const TrackerField*>& basicFields = categorized.basic;

            const bool disableEditing = d.fieldCatalogLoading;
            if (disableEditing) {
                ImGui::BeginDisabled();
            }
            ImGui::TextDisabled("Selected: %zu", selectedFieldSet.size());
            ImGui::SameLine();
            ImGui::TextDisabled("Visible: %zu", visibleFields.size());
            if (ImGui::SmallButton("Select all visible")) {
                for (const TrackerField* field : visibleFields) {
                    if (field) {
                        selectedFieldSet.insert(field->Id);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear visible")) {
                for (const TrackerField* field : visibleFields) {
                    if (field) {
                        selectedFieldSet.erase(field->Id);
                    }
                }
            }

            ImGui::Spacing();
            ImGui::BeginChild("##AvailableScroll", ImVec2(0, 0), false);

            if (availableFields.empty()) {
                ImGui::TextDisabled("No field catalog loaded yet.");
            } else if (d.fieldCatalogLoading) {
                ImGui::TextDisabled("Refreshing field catalog...");
            } else if (visibleFields.empty()) {
                ImGui::TextDisabled("No fields match current search.");
            } else {
                DrawViewsBasicFieldsGroup(d, basicFields, selectedFieldSet);
                DrawViewsFieldGroup("System fields", systemFields, selectedFieldSet);
                DrawViewsFieldGroup("Custom fields", customFields, selectedFieldSet);
            }

            ImGui::EndChild();
            if (disableEditing) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();

        // Write the (possibly toggled) selection back onto the draft's Columns — the Columns
        // tab reads d.viewDraft.Columns directly, so this is what keeps it correct when the
        // user switches tabs, without a separate reconcile pass over two independent lists.
        ApplyFieldSelectionToDraftColumns(selectedFieldSet, d.viewDraft);

        ImGui::EndTabItem();
    }
}

void SmatchetUI::drawViewsColumnsTab(ViewsDashboardDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;

    if (ImGui::BeginTabItem("Columns")) {
        d.viewsActiveTab = Tab_Columns;
        ImGui::Spacing();
        ImGui::TextUnformatted("Column order");
        ImGui::SameLine();
        ImGui::TextDisabled("— drag the handle or use Alt+↑/↓ on a focused row");

        // Per-frame LOCAL key-order view of d.viewDraft.Columns (the source of truth) — reuses
        // HandleRowReorder unchanged (it operates on a vector<string>&); ReorderViewColumns
        // writes the result back onto the draft, preserving each column's width.
        std::vector<std::string> columnKeys;
        columnKeys.reserve(d.viewDraft.Columns.size());
        for (const auto& col : d.viewDraft.Columns) {
            columnKeys.push_back(col.Key);
        }

        const auto& availableFields = app.GetAvailableFields();
        const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##ColumnOrderScroll", ImVec2(0, listHeight), true);
        bool reordered = false;
        for (int i = 0; i < static_cast<int>(columnKeys.size()); ++i) {
            const std::string key = columnKeys[static_cast<size_t>(i)];
            ImGui::PushID(i);
            ImGui::BeginGroup();
            SmatchetViewsDashboardUiDetail::DrawDragHandle("##h", i, "VIEWS_COLUMNS_ROW");
            char rowBuf[64];
            std::snprintf(rowBuf, sizeof(rowBuf), "%d.", i + 1);
            ImGui::TextUnformatted(rowBuf);
            ImGui::SameLine();
            const bool selected = (d.viewsKeyboardReorderRow == i);
            const std::string label = PrettyColumnLabel(key, availableFields);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
                d.viewsKeyboardReorderRow = i;
            }
            const float width = EffectiveColumnWidth(d.viewDraft, key);
            if (width > 0.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %.0fpx", width);
            }
            ImGui::EndGroup();
            // BeginDragDropTarget inside HandleRowReorder binds to the
            // group's full-row rect — entire row (handle + label + width
            // hint) accepts drops, including over another row's handle.
            if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, columnKeys, &d.viewsKeyboardReorderRow,
                                                                 "VIEWS_COLUMNS_ROW")) {
                reordered = true;
            }
            ImGui::PopID();
        }
        SmatchetViewsDashboardUiDetail::TickDragDropAutoScroll();
        ImGui::EndChild();
        if (reordered) {
            ReorderViewColumns(columnKeys, d.viewDraft);
        }

        ImGui::TextDisabled("Tip: Add or remove columns from the Fields tab. Resize columns directly in the grid.");

        ImGui::EndTabItem();
    }
}

void SmatchetUI::drawViewsSortTab(ViewsDashboardDrawCtx& ctx) {
    UiDrawSession& d = ctx.d;

    if (ImGui::BeginTabItem("Sort")) {
        d.viewsActiveTab = Tab_Sort;
        ImGui::Spacing();
        ImGui::TextUnformatted("Sort order");
        ImGui::SameLine();
        ImGui::TextDisabled("— drag to reorder, click direction to toggle");

        // We mutate the active view's SortSpecs in-place; bump revision after.
        ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
        const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0f;

        ImGui::BeginChild("##SortScroll", ImVec2(0, listHeight), true);
        if (!mutableActive || mutableActive->SortSpecs.empty()) {
            ImGui::TextDisabled("(no sort keys) — click \"+ Add sort key\" below.");
        }
        if (mutableActive) {
            drawViewsSortRows(ctx, mutableActive);
        }
        SmatchetViewsDashboardUiDetail::TickDragDropAutoScroll();
        ImGui::EndChild();

        drawViewsAddSortKeyPopup(ctx, mutableActive);

        ImGui::EndTabItem();
    }
}

// Drag-reorderable per-key sort rows (handle + label + direction toggle + delete). Split out of
// drawViewsSortTab under the function-size cap; behaviour-identical.
void SmatchetUI::drawViewsSortRows(ViewsDashboardDrawCtx& ctx, ViewDefinition* mutableActive) {
    UiDrawSession& d = ctx.d;
    const auto& availableFields = ctx.app.GetAvailableFields();

    // Promote SortSpecs into a parallel column-key list so we can reuse the
    // string-vector drag helper, then write any reorder back at end.
    std::vector<std::string> keyOrder;
    keyOrder.reserve(mutableActive->SortSpecs.size());
    for (const auto& spec : mutableActive->SortSpecs) {
        keyOrder.push_back(spec.ColumnKey);
    }
    bool reordered = false;
    for (int i = 0; i < static_cast<int>(keyOrder.size()); ++i) {
        const std::string key = keyOrder[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginGroup();
        SmatchetViewsDashboardUiDetail::DrawDragHandle("##h", i, "VIEWS_SORT_ROW");
        const bool selected = (d.viewsKeyboardReorderRow == i);
        const std::string label = PrettyColumnLabel(key, availableFields);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
            d.viewsKeyboardReorderRow = i;
        }
        ImGui::SameLine();
        int dir = mutableActive->SortSpecs[static_cast<size_t>(i)].Direction;
        const char* dirLabel = (dir == 1) ? "Asc" : (dir == 2 ? "Desc" : "—");
        if (ImGui::SmallButton(dirLabel)) {
            dir = (dir + 1) % 3; // cycle —, Asc, Desc
            mutableActive->SortSpecs[static_cast<size_t>(i)].Direction = dir;
            ViewState.BumpRevision();
            d.viewLayoutSaveAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        }
        ImGui::SameLine();
        bool erased = false;
        if (ImGui::SmallButton("X")) {
            mutableActive->SortSpecs.erase(mutableActive->SortSpecs.begin() + i);
            keyOrder.erase(keyOrder.begin() + i);
            ViewState.BumpRevision();
            d.viewLayoutSaveAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            erased = true;
        }
        ImGui::EndGroup();
        if (erased) {
            ImGui::PopID();
            break;
        }
        // BeginDragDropTarget inside HandleRowReorder binds to the
        // group's full-row rect — entire Sort row (handle + label
        // + direction button + X) accepts drops, including over
        // another row's handle.
        if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, keyOrder, &d.viewsKeyboardReorderRow,
                                                             "VIEWS_SORT_ROW")) {
            reordered = true;
        }
        ImGui::PopID();
    }
    if (reordered) {
        // Rebuild SortSpecs in the new order.
        std::vector<ViewSortSpec> rebuilt;
        rebuilt.reserve(keyOrder.size());
        for (const auto& k : keyOrder) {
            auto it = std::find_if(mutableActive->SortSpecs.begin(), mutableActive->SortSpecs.end(),
                                   [&](const ViewSortSpec& s) { return s.ColumnKey == k; });
            if (it != mutableActive->SortSpecs.end()) {
                rebuilt.push_back(*it);
            }
        }
        mutableActive->SortSpecs = std::move(rebuilt);
        ViewState.BumpRevision();
        d.viewLayoutSaveAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    }
}

// "+ Add sort key" popup picker, scoped to the current column order (skips already-used keys). Split
// out of drawViewsSortTab under the function-size cap; behaviour-identical.
void SmatchetUI::drawViewsAddSortKeyPopup(ViewsDashboardDrawCtx& ctx, ViewDefinition* mutableActive) {
    UiDrawSession& d = ctx.d;
    const auto& availableFields = ctx.app.GetAvailableFields();

    // + Add sort key (popup picker scoped to current column order).
    if (ImGui::Button("+ Add sort key")) {
        ImGui::OpenPopup("##AddSortKeyPopup");
    }
    if (ImGui::BeginPopup("##AddSortKeyPopup")) {
        if (mutableActive) {
            for (const auto& col : d.viewDraft.Columns) {
                const std::string& key = col.Key;
                const bool alreadyUsed = std::any_of(mutableActive->SortSpecs.begin(), mutableActive->SortSpecs.end(),
                                                     [&](const ViewSortSpec& s) { return s.ColumnKey == key; });
                if (alreadyUsed) {
                    continue;
                }
                const std::string label = PrettyColumnLabel(key, availableFields);
                if (ImGui::Selectable(label.c_str())) {
                    ViewSortSpec spec;
                    spec.ColumnKey = key;
                    spec.Direction = 1; // Asc by default
                    mutableActive->SortSpecs.push_back(spec);
                    ViewState.BumpRevision();
                    d.viewLayoutSaveAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
                    ImGui::CloseCurrentPopup();
                }
            }
        } else {
            ImGui::TextDisabled("No active view.");
        }
        ImGui::EndPopup();
    }
}

void SmatchetUI::drawViewsModals(ViewsDashboardDrawCtx& ctx) {
    // ctx.app no longer needed here: the delete-confirm handler latches instead of
    // mutating (applyPendingViewDelete owns the post-delete SyncWithCurrentView).
    UiDrawSession& d = ctx.d;
    const ViewDefinition* activeView = ctx.activeView;

    // -------- Discard-confirm modal (pending activate) --------
    if (d.viewsShowDiscardConfirm) {
        ImGui::OpenPopup("Discard changes?");
        d.viewsShowDiscardConfirm = false;
    }
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("You have unsaved changes on this view.");
        ImGui::Spacing();
        if (ImGui::Button("Save & switch")) {
            ctx.applyAndSync();
            if (!d.viewsPendingActivateId.empty()) {
                ctx.activateView(d.viewsPendingActivateId);
                d.viewsPendingActivateId.clear();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard & switch")) {
            if (!d.viewsPendingActivateId.empty()) {
                ctx.activateView(d.viewsPendingActivateId);
                d.viewsPendingActivateId.clear();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            d.viewsPendingActivateId.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // -------- Delete-view confirm modal --------
    if (d.viewsShowDeleteConfirm) {
        ImGui::OpenPopup("Delete view?");
        d.viewsShowDeleteConfirm = false;
    }
    if (ImGui::BeginPopupModal("Delete view?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // DR13b: the confirm + latch target d.viewsPendingDeleteId (the right-clicked view), which
        // may NOT be the active view. Resolve its name from the store for the prompt + toast; an
        // empty target falls back to the active view (back-compat for callers that only set the
        // show flag).
        const std::string targetId = d.viewsPendingDeleteId.empty() ? activeView->Id : d.viewsPendingDeleteId;
        const std::string targetName =
            SmatchetViewsDashboardUiDetail::FindViewName(ctx.store, targetId, activeView->Name);
        ImGui::Text("Delete view \"%s\"? This cannot be undone.", targetName.c_str());
        ImGui::Spacing();
        SmatchetPushDestructiveButtonColors();
        if (ImGui::Button("Delete")) {
            // Defer the erase to top-of-next-frame: Views::Delete erases from store.Views — same
            // pointer-invalidation class as the create latch. Copy the name now, mutate next frame.
            // viewsPendingDeleteId carries the target through to applyPendingViewDelete.
            d.viewsPendingDeleteActive = true;
            d.viewsPendingDeleteToastName = targetName;
            ImGui::CloseCurrentPopup();
        }
        SmatchetPopDestructiveButtonColors();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            d.viewsPendingDeleteId.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SmatchetUI::drawViewsDashboardWindow(AppController& app, UiDrawSession& d, bool embedded) {
    // embedded (dual-ui slice 4): mobile Views page draws the body directly into the page
    // child; skip the show-gate + dock-window chrome. Desktop path is byte-identical below.
    if (!embedded && !d.showViewsDashboard) {
        return;
    }
    if (!embedded) {
        const bool bFocusViews = d.requestViewsDashboardFocus;
        prepareTopLevelWindow(d, "views", 880.0f, 600.0f, bFocusViews);
        const std::string backendName = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
        std::string viewsWinTitle =
            SmatchetLocalization::Format("window.views_backend", "Views - %s", backendName.c_str());
        viewsWinTitle += "###SmatchetViewsDashboard";
        SmatchetWindowExpand::BeginWindow(d, viewsWinTitle.c_str());
        ImGui::Begin(viewsWinTitle.c_str(), &d.showViewsDashboard);
        SmatchetWindowExpand::DrawToggle(d);
        repairTopLevelWindow(d, "views", 520.0f, 320.0f);
        if (bFocusViews) {
            ImGui::SetWindowFocus();
            d.requestViewsDashboardFocus = false;
        }
    }

    ViewState.EnsureLoaded(d.cfg);
    const ViewsStore& store = ViewState.GetStoreMutable();

    drawViewsConnectivityBanner(app, d);
    drawViewsJiraDomainPicker(app, d);

    const ViewDefinition* activeView = ViewState.GetActiveView();

    // [temp-debug] Instrument this second reload guard — the suspected "stealer" call site
    // reached via drawMobilePageContent(embedded=true) ahead of drawMobileDrawerViews each frame.
    std::fprintf(stderr, "[drawViewsDashboardWindow] embedded=%s activeView=%s d.viewDraftId='%s'\n",
                 embedded ? "true" : "false", activeView ? activeView->Id.c_str() : "<null>",
                 d.viewDraftId.c_str());

    // Reload the draft whenever the active view id changed underneath us — see the matching
    // comment in drawMobileDrawerViews for why a mere layout drift no longer force-reloads.
    if (activeView && d.viewDraftId != activeView->Id) {
        std::fprintf(stderr, "[drawViewsDashboardWindow] embedded=%s IDs DO NOT MATCH — calling LoadBuffersFromView\n",
                     embedded ? "true" : "false");
        LoadBuffersFromView(d, *activeView);
    }

    if (!activeView) {
        ImGui::TextDisabled("No views available.");
        if (!embedded) {
            ImGui::End();
        }
        return;
    }

    // Action helpers — closures so the tab bodies stay short; bodies live in the views* methods.
    auto applyAndSync = [this, &app, &d, activeView]() { viewsApplyAndSync(app, d, activeView); };
    auto discardChanges = [this, &d]() { viewsDiscardChanges(d); };
    auto activateView = [this, &app, &d](const std::string& id) { viewsActivateView(app, d, id); };
    auto requestActivate = [this, &app, &d, activeView](const std::string& id) {
        viewsRequestActivate(app, d, activeView, id);
    };
    auto createNewView = [this, &app, &d, activeView]() { viewsCreateNewView(app, d, activeView); };

    handleViewsDashboardShortcuts(app, d, activeView);

    // ============================================================ Layout: sidebar | splitter | editor.
    float sidebarWidth = d.cfg.ViewsSidebarWidth;
    if (sidebarWidth < 140.0f) {
        sidebarWidth = 220.0f;
    }
    const float windowWidth = ImGui::GetContentRegionAvail().x;
    const float maxSidebar = (std::max)(180.0f, windowWidth - 360.0f);
    if (sidebarWidth > maxSidebar) {
        sidebarWidth = maxSidebar;
    }

    ViewsDashboardDrawCtx ctx{
        app,         d, store, activeView, sidebarWidth, applyAndSync, discardChanges, createNewView, requestActivate,
        activateView};

    // -------- Sidebar --------
    drawViewsSidebar(ctx);

    // Splitter between sidebar and editor body.
    ImGui::SameLine(0.0f, 0.0f);
    SmatchetViewsDashboardUiDetail::DrawHorizontalSplitter("##ViewsSidebarSplitter", d, &sidebarWidth, 160.0f,
                                                           maxSidebar);
    d.cfg.ViewsSidebarWidth = sidebarWidth;
    ImGui::SameLine(0.0f, 0.0f);

    // -------- Editor pane --------
    ImGui::BeginChild("ViewsEditor", ImVec2(0, 0), false);

    // Editor header: title (inline-editable) + status strip + Apply button.
    drawViewsEditorHeader(ctx);

    // [temp-debug] Bisect where within this function's tab bar the draft's Columns count
    // changes — pinpointing whether drawViewsFieldsTab/ColumnsTab reintroduce a column the
    // reload guard just dropped.
    std::fprintf(stderr, "[drawViewsDashboardWindow] pre-tabbar embedded=%s activeTab=%d Columns=%zu\n",
                 embedded ? "true" : "false", static_cast<int>(d.viewsActiveTab), d.viewDraft.Columns.size());

    // Tab bar.
    if (ImGui::BeginTabBar("##ViewsEditorTabs", ImGuiTabBarFlags_None)) {
        drawViewsFilterTab(ctx);
        std::fprintf(stderr, "[drawViewsDashboardWindow] post-filter Columns=%zu\n", d.viewDraft.Columns.size());
        drawViewsFieldsTab(ctx);
        std::fprintf(stderr, "[drawViewsDashboardWindow] post-fields Columns=%zu\n", d.viewDraft.Columns.size());
        drawViewsColumnsTab(ctx);
        std::fprintf(stderr, "[drawViewsDashboardWindow] post-columns Columns=%zu\n", d.viewDraft.Columns.size());
        drawViewsSortTab(ctx);
        std::fprintf(stderr, "[drawViewsDashboardWindow] post-sort Columns=%zu\n", d.viewDraft.Columns.size());
        ImGui::EndTabBar();
    }

    ImGui::EndChild(); // ViewsEditor

    drawViewsModals(ctx);

    if (!embedded) {
        ImGui::End();
    }
}

// Tracker connectivity banner (error / warning strip above the editor). Split out of
// drawViewsDashboardWindow under the function-size cap; behaviour-identical.
void SmatchetUI::drawViewsConnectivityBanner(AppController& app, UiDrawSession& d) {
    const std::string* sessionCatalogNote = d.fieldCatalogWarning.empty() ? nullptr : &d.fieldCatalogWarning;
    const TrackerConnectivityBannerForUi jiraBanner = app.GetTrackerConnectivityBannerForUi(sessionCatalogNote);
    if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
    } else if (jiraBanner.Kind == TrackerConnectivityBannerForUi::Level::Warning) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.92f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", jiraBanner.Message.c_str());
        ImGui::PopStyleColor();
    }
    if (jiraBanner.Kind != TrackerConnectivityBannerForUi::Level::None) {
        ImGui::Separator();
    }
}

void SmatchetUI::drawViewsJiraDomainPicker(AppController& app, UiDrawSession& d) {
    if (ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType) != "Jira") {
        return;
    }
    smatchet::jira_backends::EnsureHydrated(d.cfg);
    if (d.cfg.JiraBackends.size() < 2) {
        return;
    }
    int current = 0;
    const std::string live = d.cfg.ActiveJiraDomain.empty() ? d.cfg.Domain : d.cfg.ActiveJiraDomain;
    for (std::size_t i = 0; i < d.cfg.JiraBackends.size(); ++i) {
        if (smatchet::jira_backends::HostsMatch(d.cfg.JiraBackends[i].Domain, live)) {
            current = static_cast<int>(i);
            break;
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(SmatchetLocalization::T("views.jira_domain", "Domain"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.0f);
    const std::string& preview = d.cfg.JiraBackends[static_cast<std::size_t>(current)].Domain;
    if (ImGui::BeginCombo("##ViewsJiraDomain", preview.c_str())) {
        for (std::size_t i = 0; i < d.cfg.JiraBackends.size(); ++i) {
            const bool selected = static_cast<int>(i) == current;
            const char* label = d.cfg.JiraBackends[i].Domain.c_str();
            if (ImGui::Selectable(label, selected) && !selected) {
                if (smatchet::jira_backends::SelectActive(d.cfg, d.cfg.JiraBackends[i].Domain)) {
                    // JiraClient cfg-less paths re-read the Load cache. A queued tracker write
                    // leaves that cache on the previous origin until the worker drains, so sync
                    // would talk to the old host. Blocking Save matches persist-before-sync.
                    ConfigManager::Save(d.cfg);
                    d.triggerCatalogRefetch = true;
                    app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s",
                          SmatchetLocalization::T(
                              "views.jira_domain.help",
                              "Which Jira site this view talks to. Extra sites are added in Preferences → Tracker."));
    }
}

// Apply the editing buffers onto the active view + sync the grid. Former applyAndSync closure body.
// Commit the draft onto the store (column-view-save-simplification). ViewState.Update
// normalizes it (regenerating Fields from Columns, filling default widths, pruning stale
// SortSpecs) — reload the draft from what ACTUALLY landed rather than assume the pre-commit
// draft matches, which is what let a prior version of this function forget to keep
// d.lastSyncedColumnOrder in sync and cause a spurious buffer reload the very next frame.
void SmatchetUI::viewsApplyAndSync(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    if (!activeView) {
        return;
    }
    // Apply only what the editor's own tabs actually edit (Name/Jql via the Filter tab,
    // column order + membership via the Fields/Columns tabs) onto the CURRENT live view,
    // not a wholesale draft overwrite. SortSpecs/HideParents/StoryGroupSort are owned by
    // the grid header (DrawSortByPopupBody writes them straight to the live view and
    // autosaves) and are never edited through d.viewDraft, so taking them from the draft
    // here would silently revert whatever autosaved into the live view since the draft was
    // last loaded — the draft only reloads on a view-id switch, not on every live layout
    // write (Cursor Bugbot finding). Column widths get the same treatment: order/membership
    // come from the draft, but each surviving key's width is re-read from the live view so a
    // grid-driven resize made while the editor was open isn't clobbered by the draft's
    // load-time width.
    ViewDefinition merged = *activeView;
    merged.Name = d.viewDraft.Name;
    merged.Jql = d.viewDraft.Jql;
    merged.Columns.clear();
    merged.Columns.reserve(d.viewDraft.Columns.size());
    for (const auto& col : d.viewDraft.Columns) {
        merged.Columns.push_back({col.Key, EffectiveColumnWidth(*activeView, col.Key)});
    }
    if (ViewState.UpdateActive(merged)) {
        const ViewDefinition* saved = ViewState.GetActiveView();
        if (saved) {
            d.cfg.JqlQuery = saved->Jql;
            d.cfg.SelectedFields = saved->Fields;
            SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            LoadBuffersFromView(d, *saved);
            SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.view_saved", "View saved"),
                                                  saved->Name, ToastType::Success, 1800);
        }
    }
}

// Reload the draft from the store, discarding any in-progress edit. Former discardChanges
// closure. No snapshot to restore (column-view-save-simplification) — the draft never
// touches the store until Apply & Sync, so there is nothing in the store to revert.
void SmatchetUI::viewsDiscardChanges(UiDrawSession& d) {
    const ViewDefinition* a = ViewState.GetActiveView();
    if (a) {
        LoadBuffersFromView(d, *a);
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.discarded_changes", "Discarded changes"),
                                              a->Name, ToastType::Info, 1500);
    }
}

// Activate a view by id + reload buffers/grid. Former activateView closure body.
void SmatchetUI::viewsActivateView(AppController& app, UiDrawSession& d, const std::string& id, bool kickSync) {
    if (ViewState.Activate(id)) {
        const ViewDefinition* nowActive = ViewState.GetActiveView();
        if (nowActive) {
            LoadBuffersFromView(d, *nowActive);
            d.cfg.JqlQuery = nowActive->Jql;
            d.cfg.SelectedFields = nowActive->Fields;
            if (kickSync) {
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            } else {
                // Pane focus switch onto an already-sync-live context (Slice 3): persist the
                // adopted identity + keep nav history, but skip the SyncWithBackend network
                // re-fetch — the pane's own GridLiveContext data is already fresh.
                // Pillar 2 (#2026): this is the `WriteConfigJson` half of the violation pair the
                // log shows on every pane show/hide (the `LoadPersistentViewsFromDisk` half is
                // ViewState.Activate's save, now also off-thread). Coalescing worker, not an
                // inline atomic whole-file rewrite on the frame thread.
                smatchet::config_save::EnqueueTrackerConfig(d.cfg);
                d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
            }
        }
    }
}

// Request activation, guarded by the unsaved-changes confirm. Former requestActivate closure body.
// Dirty is derived (column-view-save-simplification), not d.viewsDirty — that field is the
// GRID's own query-strip flag now, unrelated to this editor's draft.
void SmatchetUI::viewsRequestActivate(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                                      const std::string& id) {
    if (id == activeView->Id) {
        return;
    }
    if (ViewDraftDiffersFromSaved(d.viewDraft, *activeView, 0.5f)) {
        d.viewsPendingActivateId = id;
        d.viewsShowDiscardConfirm = true;
    } else {
        viewsActivateView(app, d, id);
    }
}

// Create a new view from the current editing buffers. Former createNewView closure body.
// DEFERRED (Pillar 3 crash fix): a mid-frame ViewState.Create reallocates store.Views and
// dangles every ViewDefinition* resolved this frame (this window's ctx.activeView, the
// captured action lambdas, each pane's activeViewForGrid). The payload is copied while
// *activeView is still valid; applyPendingViewCreate consumes the latch next frame.
void SmatchetUI::viewsCreateNewView(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    (void)app;
    (void)activeView;
    ViewDefinition created = d.viewDraft;
    created.Name = "New View";
    created.Id.clear();
    d.viewsPendingCreate = true;
    d.viewsPendingCreatePayload = std::move(created);
    d.viewsPendingCreateToastTitle = "View created";
    d.viewsPendingCreateToastMs = 1800;
    d.viewsPendingCreateAdoptCfg = false;
}

// Consume the one-frame deferred view-create latch (see UiDrawSession::viewsPendingCreate).
// Runs at the top of SmatchetUI::Draw, BEFORE any ViewDefinition* is resolved for the
// frame, so the store mutation can never invalidate a live pointer.
void SmatchetUI::applyPendingViewCreate(UiDrawSession& d) {
    if (!d.viewsPendingCreate) {
        return;
    }
    // Consume-once means ALL latch state — capture + reset the auxiliary fields
    // up front so no early return can leak them into a later latch cycle
    // (review M: the reset was previously split across the success path only).
    const bool adoptCfg = d.viewsPendingCreateAdoptCfg;
    const std::string toastTitle = d.viewsPendingCreateToastTitle;
    const int toastMs = d.viewsPendingCreateToastMs;
    d.viewsPendingCreateAdoptCfg = false;
    d.viewsPendingCreateToastTitle.clear();
    d.viewsPendingCreateToastMs = 1800;
    const ViewDefinition* nowActive = SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore(
        ViewState, d.viewsPendingCreate, d.viewsPendingCreatePayload);
    if (!nowActive) {
        return;
    }
    LoadBuffersFromView(d, *nowActive);
    if (adoptCfg) {
        d.cfg.JqlQuery = nowActive->Jql;
        d.cfg.SelectedFields = nowActive->Fields;
        ConfigManager::Save(d.cfg);
    }
    SmatchetToastManager::Instance().Push(toastTitle, nowActive->Name, ToastType::Success, toastMs);
}

// Consume the one-frame deferred view-DELETE latch (see
// UiDrawSession::viewsPendingDeleteActive). Runs immediately after the create
// latch at the top of SmatchetUI::Draw — same rationale: Views::DeleteActive
// erases from store.Views, so it must never run while frame-resolved
// ViewDefinition* are live.
void SmatchetUI::applyPendingViewDelete(AppController& app, UiDrawSession& d) {
    if (!d.viewsPendingDeleteActive) {
        return;
    }
    d.viewsPendingDeleteActive = false;
    const std::string deletedName = d.viewsPendingDeleteToastName;
    d.viewsPendingDeleteToastName.clear();
    const std::string targetId = d.viewsPendingDeleteId;
    d.viewsPendingDeleteId.clear();

    // DR13b: delete the explicitly-targeted view. An empty target (or a target that IS the active
    // view) uses the DeleteActive path so the editor + grid re-home onto the new active view.
    // Deleting a NON-active view leaves the current editing context and grid untouched.
    const ViewDefinition* activeBefore = ViewState.GetActiveView();
    const std::string activeIdBefore = activeBefore ? activeBefore->Id : std::string();
    const bool deletingActive = targetId.empty() || targetId == activeIdBefore;
    const bool ok = deletingActive ? ViewState.DeleteActive() : ViewState.Delete(targetId);
    if (!ok) {
        return;
    }
    if (deletingActive) {
        const ViewDefinition* nowActive = ViewState.GetActiveView();
        if (nowActive) {
            LoadBuffersFromView(d, *nowActive);
            d.cfg.JqlQuery = nowActive->Jql;
            d.cfg.SelectedFields = nowActive->Fields;
            SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
        }
    }
    SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.view_deleted", "View deleted"), deletedName,
                                          ToastType::Info, 1800);
}

// Window-level keyboard shortcuts: Ctrl+Enter = Apply, Ctrl+N = New. Split out of
// drawViewsDashboardWindow under the function-size cap; behaviour-identical.
void SmatchetUI::handleViewsDashboardShortcuts(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow)) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            viewsApplyAndSync(app, d, activeView);
        } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
            viewsCreateNewView(app, d, activeView);
        }
    }
}
