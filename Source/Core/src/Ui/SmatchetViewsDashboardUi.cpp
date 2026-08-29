// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "SmatchetUI.h"

#include "SmatchetViewsDashboardUi_detail.h"
#include "SmatchetAutocompleteUi.h"
#include "AppController.h"
#include "Views.h"
#include "ConfigManager.h"
#include "ConfigSaveWorker.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"
#include "SmatchetToast.h"
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

// Load all edit buffers from a saved view. Resets dirty + autocomplete state.
void LoadBuffersFromView(UiDrawSession& d, const ViewDefinition& view) {
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
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewNameBuf, view.Name);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlEditor.buf, view.Jql);
    // Seed the authoritative selection set straight from the saved view
    // (#views-field-uncheck) — never via a truncating CSV buffer.
    d.selectedFieldSet.clear();
    for (const auto& fieldId : view.Fields) {
        d.selectedFieldSet.insert(fieldId);
    }
    d.editingColumnOrder = view.ColumnOrder;
    if (d.editingColumnOrder.empty()) {
        d.editingColumnOrder = {"id"};
        for (const auto& fieldId : view.Fields) {
            d.editingColumnOrder.push_back("field:" + fieldId);
        }
    }
    d.selectedColumnOrderIndex = -1;
    d.editingViewId = view.Id;
    d.lastSyncedColumnOrder = view.ColumnOrder;
    d.viewsDirty = false;
    d.viewsHasOriginalSnapshot = false;
    d.viewsKeyboardReorderRow = -1;
    d.viewsTitleEditing = false;
}

ViewDefinition BuildUpdatedView(AppController& app, const ViewDefinition& base, const UiDrawSession& d) {
    ViewDefinition updated = base;
    updated.Name = d.viewNameBuf;
    // The editor buffer holds the readable name form on Jira (TrackerQueryAcp_ApplyUserNamesToBuffer);
    // the view of record keeps the id-canonical query, so reverse-map names back to account ids here.
    updated.Jql = TrackerQueryAcp_CanonicalQueryForApply(d.cfg.TrackerType, app.GetAvailableFields(),
                                                         app.GetAvailableUsers(), d.viewJqlEditor,
                                                         std::string(d.viewJqlEditor.buf));
    // Read the authoritative set (#views-field-uncheck), not the truncating buffer, so a
    // >1023-byte selection persists ALL fields on save instead of being clipped on disk.
    updated.Fields = SmatchetViewsDashboardUiDetail::ToSortedVector(d.selectedFieldSet);
    updated.ColumnOrder = d.editingColumnOrder;
    return updated;
}

// Reconcile editing column-order list against currently-selected fields. Drops
// entries for removed fields; appends entries for newly-checked fields.
void ReconcileEditingColumnOrder(UiDrawSession& d) {
    std::unordered_set<std::string> validKeys = {"id"};
    for (const auto& f : d.selectedFieldSet) {
        validKeys.insert("field:" + f);
    }
    d.editingColumnOrder.erase(
        std::remove_if(d.editingColumnOrder.begin(), d.editingColumnOrder.end(),
                       [&](const std::string& key) { return validKeys.find(key) == validKeys.end(); }),
        d.editingColumnOrder.end());
    for (const auto& key : validKeys) {
        if (std::find(d.editingColumnOrder.begin(), d.editingColumnOrder.end(), key) == d.editingColumnOrder.end()) {
            d.editingColumnOrder.push_back(key);
        }
    }
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
    ViewState.EnsureLoaded(d.cfg);
    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (!activeView) {
        ImGui::TextDisabled("No views available.");
        return;
    }
    if (d.editingViewId != activeView->Id) {
        LoadBuffersFromView(d, *activeView);
    } else if (activeView->ColumnOrder != d.lastSyncedColumnOrder) {
        LoadBuffersFromView(d, *activeView);
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

    // Title row.
    if (d.viewsTitleEditing) {
        ImGui::SetNextItemWidth(-260.0f);
        if (ImGui::InputText("##ViewTitle", d.viewNameBuf, sizeof(d.viewNameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            d.viewsTitleEditing = false;
            d.viewsDirty = true;
        }
        if (ImGui::IsItemDeactivated()) {
            d.viewsTitleEditing = false;
            if (std::string(d.viewNameBuf) != activeView->Name) {
                d.viewsDirty = true;
            }
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(activeView->Name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename")) {
            d.viewsTitleEditing = true;
        }
        if (d.viewsDirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.20f, 1.0f), "  unsaved");
        }
    }

    // Action buttons on a separate row below the title for a more compact header.
    const bool disableDiscard = !d.viewsDirty;
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
        if (ImGui::InputText("##ViewNameInput", d.viewNameBuf, sizeof(d.viewNameBuf))) {
            d.viewsDirty = true;
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
                app.OpenUrl(app.BuildJqlSearchUrl(
                    d.cfg, TrackerQueryAcp_CanonicalQueryForApply(d.cfg.TrackerType, app.GetAvailableFields(),
                                                                  app.GetAvailableUsers(), d.viewJqlEditor,
                                                                  currentJql)));
            }
        }
        if (disableOpenJql) {
            ImGui::EndDisabled();
        }

        char beforeJql[sizeof(d.viewJqlEditor.buf)];
        std::memcpy(beforeJql, d.viewJqlEditor.buf, sizeof(beforeJql));
        SmatchetViewsDashboardUiDetail::DrawJqlQueryEditorEmbedded(app, d, d.viewJqlEditor);
        // Compare content, not just length — a same-length edit ("abc" -> "xyz") still dirties (#6).
        // The cosmetic id->name rewrite consumes its flag here so it never marks the view dirty
        // (typing needs focus, the rewrite needs no focus — the two can't co-occur in a frame).
        const bool semanticRewrite = d.viewJqlEditor.jqlBufSemanticRewrite;
        d.viewJqlEditor.jqlBufSemanticRewrite = false;
        if (!semanticRewrite && std::strcmp(d.viewJqlEditor.buf, beforeJql) != 0) {
            d.viewsDirty = true;
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
void DrawViewsFieldGroup(UiDrawSession& d, const char* groupName, const std::vector<const TrackerField*>& fields,
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
            d.viewsDirty = true;
        }
        ImGui::SameLine();
        ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
    }
}

// The "Basic fields" group: the locked ID row plus the six core Jira fields in fixed order. Extracted
// from drawViewsFieldsTab during the over-100-line decomposition. Behaviour is identical.
void DrawViewsBasicFieldsGroup(UiDrawSession& d, const std::vector<const TrackerField*>& basicFields,
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
            d.viewsDirty = true;
        }
        ImGui::SameLine();
        ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
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

        // The authoritative selection set lives on the session (#views-field-uncheck): the toggle
        // handlers, select-all and clear mutate it in place. It is seeded from view.Fields in
        // LoadBuffersFromView, never re-derived from the truncating buffer per frame.
        std::unordered_set<std::string>& selectedFieldSet = d.selectedFieldSet;
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
                d.viewsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear visible")) {
                for (const TrackerField* field : visibleFields) {
                    if (field) {
                        selectedFieldSet.erase(field->Id);
                    }
                }
                d.viewsDirty = true;
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
                DrawViewsFieldGroup(d, "System fields", systemFields, selectedFieldSet);
                DrawViewsFieldGroup(d, "Custom fields", customFields, selectedFieldSet);
            }

            ImGui::EndChild();
            if (disableEditing) {
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();

        // Keep editingColumnOrder coherent with selection so the Columns tab is correct
        // when the user switches tabs.
        ReconcileEditingColumnOrder(d);

        ImGui::EndTabItem();
    }
}

void SmatchetUI::drawViewsColumnsTab(ViewsDashboardDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const ViewDefinition* activeView = ctx.activeView;

    if (ImGui::BeginTabItem("Columns")) {
        d.viewsActiveTab = Tab_Columns;
        ImGui::Spacing();
        ImGui::TextUnformatted("Column order");
        ImGui::SameLine();
        ImGui::TextDisabled("— drag the handle or use Alt+↑/↓ on a focused row");

        ReconcileEditingColumnOrder(d);

        const auto& availableFields = app.GetAvailableFields();
        const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##ColumnOrderScroll", ImVec2(0, listHeight), true);
        for (int i = 0; i < static_cast<int>(d.editingColumnOrder.size()); ++i) {
            const std::string key = d.editingColumnOrder[static_cast<size_t>(i)];
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
            auto wit = activeView->ColumnWidths.find(key);
            if (wit != activeView->ColumnWidths.end() && wit->second > 0.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %.0fpx", wit->second);
            }
            ImGui::EndGroup();
            // BeginDragDropTarget inside HandleRowReorder binds to the
            // group's full-row rect — entire row (handle + label + width
            // hint) accepts drops, including over another row's handle.
            if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, d.editingColumnOrder, &d.viewsKeyboardReorderRow,
                                                                 "VIEWS_COLUMNS_ROW")) {
                d.viewsDirty = true;
            }
            ImGui::PopID();
        }
        SmatchetViewsDashboardUiDetail::TickDragDropAutoScroll();
        ImGui::EndChild();

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
            SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
            dir = (dir + 1) % 3; // cycle —, Asc, Desc
            mutableActive->SortSpecs[static_cast<size_t>(i)].Direction = dir;
            ViewState.BumpRevision();
            d.viewsDirty = true;
        }
        ImGui::SameLine();
        bool erased = false;
        if (ImGui::SmallButton("X")) {
            SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
            mutableActive->SortSpecs.erase(mutableActive->SortSpecs.begin() + i);
            keyOrder.erase(keyOrder.begin() + i);
            ViewState.BumpRevision();
            d.viewsDirty = true;
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
        SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
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
        d.viewsDirty = true;
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
            for (const auto& key : d.editingColumnOrder) {
                const bool alreadyUsed = std::any_of(mutableActive->SortSpecs.begin(), mutableActive->SortSpecs.end(),
                                                     [&](const ViewSortSpec& s) { return s.ColumnKey == key; });
                if (alreadyUsed) {
                    continue;
                }
                const std::string label = PrettyColumnLabel(key, availableFields);
                if (ImGui::Selectable(label.c_str())) {
                    SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                    ViewSortSpec spec;
                    spec.ColumnKey = key;
                    spec.Direction = 1; // Asc by default
                    mutableActive->SortSpecs.push_back(spec);
                    ViewState.BumpRevision();
                    d.viewsDirty = true;
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

    const ViewDefinition* activeView = ViewState.GetActiveView();

    // Re-load buffers whenever the active view changes underneath us OR when the
    // saved column order drifts away from what we last synced (e.g. another window
    // mutated it).
    if (activeView) {
        if (d.editingViewId != activeView->Id) {
            LoadBuffersFromView(d, *activeView);
        } else if (activeView->ColumnOrder != d.lastSyncedColumnOrder) {
            LoadBuffersFromView(d, *activeView);
        }
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

    // Tab bar.
    if (ImGui::BeginTabBar("##ViewsEditorTabs", ImGuiTabBarFlags_None)) {
        drawViewsFilterTab(ctx);
        drawViewsFieldsTab(ctx);
        drawViewsColumnsTab(ctx);
        drawViewsSortTab(ctx);
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

// Apply the editing buffers onto the active view + sync the grid. Former applyAndSync closure body.
void SmatchetUI::viewsApplyAndSync(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    if (!activeView) {
        return;
    }
    ReconcileEditingColumnOrder(d);
    ViewDefinition updated = BuildUpdatedView(app, *activeView, d);
    if (ViewState.UpdateActive(updated)) {
        d.cfg.JqlQuery = updated.Jql;
        d.cfg.SelectedFields = updated.Fields;
        SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
        d.viewsDirty = false;
        d.viewsHasOriginalSnapshot = false;
        d.pendingViewStateSave = false;
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.view_saved", "View saved"), updated.Name,
                                              ToastType::Success, 1800);
    }
}

// Restore the active view from its pre-dirty snapshot + reload buffers. Former discardChanges closure.
void SmatchetUI::viewsDiscardChanges(UiDrawSession& d) {
    // Restore the in-memory view from the pre-dirty snapshot (covers grid-side
    // width / sort mutations that happened in-place) before reloading buffers.
    ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
    if (mutableActive && d.viewsHasOriginalSnapshot) {
        *mutableActive = d.viewsOriginalSnapshot;
        ViewState.BumpRevision();
    }
    const ViewDefinition* a = ViewState.GetActiveView();
    if (a) {
        LoadBuffersFromView(d, *a);
        d.viewsHasOriginalSnapshot = false;
        d.pendingViewStateSave = false;
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
void SmatchetUI::viewsRequestActivate(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                                      const std::string& id) {
    if (id == activeView->Id) {
        return;
    }
    if (d.viewsDirty) {
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
    ReconcileEditingColumnOrder(d);
    ViewDefinition created = BuildUpdatedView(app, *activeView, d);
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
