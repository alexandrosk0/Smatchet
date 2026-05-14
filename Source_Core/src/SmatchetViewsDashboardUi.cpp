#include "SmatchetUI.h"

#include "SmatchetViewsDashboardUi_detail.h"
#include "AppController.h"
#include "Views.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// One enum value per editor tab. Persisted in UiDrawSession::viewsActiveTab.
enum ViewsEditorTab : int {
    Tab_Filter = 0,
    Tab_Fields = 1,
    Tab_Columns = 2,
    Tab_Sort = 3,
};

bool IsBasicFieldId(const std::string& id) {
    return id == "summary" || id == "assignee" || id == "priority" || id == "status" || id == "created" ||
           id == "updated";
}

// Load all edit buffers from a saved view. Resets dirty + autocomplete state.
void LoadBuffersFromView(UiDrawSession& d, const ViewDefinition& view) {
    std::memset(d.fieldSearchBuf, 0, sizeof(d.fieldSearchBuf));
    d.jqlAcpApplyReplace = false;
    d.jqlAcpReplaceStart = -1;
    d.jqlAcpReplaceEnd = -1;
    d.jqlAcpReplaceText.clear();
    d.jqlAcpListSelected = -1;
    d.jqlAcpLastCursor = 0;
    d.jqlAcpLastSelectionStart = 0;
    d.jqlAcpLastSelectionEnd = 0;
    d.jqlAcpWantsJqlInputFocus = false;
    d.jqlAcpScrollToSelected = false;
    d.jqlAcpCaretSnapFramesRemaining = 0;
    d.jqlWantsApplyFromEnter = false;
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewNameBuf, view.Name);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, view.Jql);
    const std::string selectedFieldsCsv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(view.Fields);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, selectedFieldsCsv);
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

ViewDefinition BuildUpdatedView(const ViewDefinition& base, const UiDrawSession& d) {
    ViewDefinition updated = base;
    updated.Name = d.viewNameBuf;
    updated.Jql = d.viewJqlBuf;
    updated.Fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
    updated.ColumnOrder = d.editingColumnOrder;
    return updated;
}

// Reconcile editing column-order list against currently-selected fields. Drops
// entries for removed fields; appends entries for newly-checked fields.
void ReconcileEditingColumnOrder(UiDrawSession& d) {
    const std::vector<std::string> currentFields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
    std::unordered_set<std::string> validKeys = {"id"};
    for (const auto& f : currentFields) {
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

void SyncSelectedFieldsBuffer(UiDrawSession& d, const std::unordered_set<std::string>& selectedFieldSet) {
    const std::vector<std::string> sortedFields = SmatchetViewsDashboardUiDetail::ToSortedVector(selectedFieldSet);
    const std::string csv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(sortedFields);
    SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, csv);
}

// Friendly label for a column key — "id" stays as "ID"; "field:foo" resolves via the catalog.
std::string PrettyColumnLabel(const std::string& key, const std::vector<TrackerField>& fields) {
    if (key == "id") {
        return "ID";
    }
    if (key.rfind("field:", 0) == 0) {
        const std::string fieldId = key.substr(6);
        for (const auto& f : fields) {
            if (f.Id == fieldId) {
                return f.Name + " (" + f.Id + ")";
            }
        }
        return fieldId;
    }
    return key;
}

} // namespace

void SmatchetUI::drawViewsDashboardWindow(AppController& app, UiDrawSession& d) {
    if (!d.showViewsDashboard) {
        return;
    }
    const bool bFocusViews = d.requestViewsDashboardFocus;
    prepareTopLevelWindow(d, "views", 880.0f, 600.0f, bFocusViews);
    const std::string backendName = ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
    std::string viewsWinTitle = SmatchetLocalization::Format("window.views_backend", "Views - %s", backendName.c_str());
    viewsWinTitle += "###SmatchetViewsDashboard";
    ImGui::Begin(viewsWinTitle.c_str(), &d.showViewsDashboard);
    repairTopLevelWindow(d, "views", 520.0f, 320.0f);
    if (bFocusViews) {
        ImGui::SetWindowFocus();
        d.requestViewsDashboardFocus = false;
    }

    ViewState.EnsureLoaded(d.cfg);
    const ViewsStore& store = ViewState.GetStoreMutable();

    // Tracker connectivity banner — preserved from the legacy UI.
    {
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
        ImGui::End();
        return;
    }

    // Action helpers — closures so the tab bodies stay short.
    auto applyAndSync = [&]() {
        if (!activeView) {
            return;
        }
        ReconcileEditingColumnOrder(d);
        ViewDefinition updated = BuildUpdatedView(*activeView, d);
        if (ViewState.UpdateActive(updated)) {
            d.cfg.JqlQuery = updated.Jql;
            d.cfg.SelectedFields = updated.Fields;
            SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            d.viewsDirty = false;
            d.viewsHasOriginalSnapshot = false;
            d.pendingViewStateSave = false;
            SmatchetToastManager::Instance().Push("View saved", updated.Name, ToastType::Success, 1800);
        }
    };
    auto discardChanges = [&]() {
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
            SmatchetToastManager::Instance().Push("Discarded changes", a->Name, ToastType::Info, 1500);
        }
    };
    auto activateView = [&](const std::string& id) {
        if (ViewState.Activate(id)) {
            const ViewDefinition* nowActive = ViewState.GetActiveView();
            if (nowActive) {
                LoadBuffersFromView(d, *nowActive);
                d.cfg.JqlQuery = nowActive->Jql;
                d.cfg.SelectedFields = nowActive->Fields;
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            }
        }
    };
    auto requestActivate = [&](const std::string& id) {
        if (id == activeView->Id) {
            return;
        }
        if (d.viewsDirty) {
            d.viewsPendingActivateId = id;
            d.viewsShowDiscardConfirm = true;
        } else {
            activateView(id);
        }
    };
    auto createNewView = [&]() {
        ReconcileEditingColumnOrder(d);
        ViewDefinition created = BuildUpdatedView(*activeView, d);
        created.Name = "New View";
        created.Id.clear();
        ViewState.Create(created);
        const ViewDefinition* nowActive = ViewState.GetActiveView();
        if (nowActive) {
            LoadBuffersFromView(d, *nowActive);
            SmatchetToastManager::Instance().Push("View created", nowActive->Name, ToastType::Success, 1800);
        }
    };

    // Global keyboard shortcuts (window-level): Ctrl+Enter = Apply, Ctrl+N = New.
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow)) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                applyAndSync();
            } else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) {
                createNewView();
            }
        }
    }

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

    // -------- Sidebar --------
    ImGui::BeginChild("ViewsSidebar", ImVec2(sidebarWidth, 0), true);
    {
        ImGui::TextUnformatted("Views");
        ImGui::SameLine();
        const float btnW = ImGui::CalcTextSize("+ New").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - btnW);
        if (ImGui::SmallButton("+ New")) {
            createNewView();
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
                requestActivate(view.Id);
            }
            // Hover-revealed context menu for rename / duplicate / delete.
            if (ImGui::BeginPopupContextItem("##ViewRowMenu")) {
                if (ImGui::MenuItem("Rename...")) {
                    requestActivate(view.Id);
                    d.viewsTitleEditing = true;
                }
                if (ImGui::MenuItem("Duplicate")) {
                    ViewDefinition dup = view;
                    dup.Id.clear();
                    dup.Name = view.Name + " (copy)";
                    ViewState.Create(dup);
                    const ViewDefinition* nowActive = ViewState.GetActiveView();
                    if (nowActive) {
                        LoadBuffersFromView(d, *nowActive);
                        SmatchetToastManager::Instance().Push("View duplicated", nowActive->Name, ToastType::Success,
                                                              1500);
                    }
                }
                ImGui::Separator();
                const bool canDelete = store.Views.size() > 1;
                if (!canDelete) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::MenuItem("Delete view...")) {
                    requestActivate(view.Id);
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

    // Splitter between sidebar and editor body.
    ImGui::SameLine(0.0f, 0.0f);
    SmatchetViewsDashboardUiDetail::DrawHorizontalSplitter("##ViewsSidebarSplitter", d, &sidebarWidth, 160.0f,
                                                           maxSidebar);
    d.cfg.ViewsSidebarWidth = sidebarWidth;
    ImGui::SameLine(0.0f, 0.0f);

    // -------- Editor pane --------
    ImGui::BeginChild("ViewsEditor", ImVec2(0, 0), false);

    // Editor header: title (inline-editable) + status strip + Apply button.
    {
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

        // Force row break so the action cluster lands below the title row.
        ImGui::NewLine();
        ImGui::NewLine();
        
        // Action buttons on a separate row below the title for a more compact header.
        const bool disableDiscard = !d.viewsDirty;
        if (disableDiscard) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Discard")) {
            discardChanges();
        }
        if (disableDiscard) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Apply & Sync")) {
            applyAndSync();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Apply changes and sync issues (Ctrl+Enter).");
        }
        ImGui::SameLine();
        const bool disableDelete = (store.Views.size() <= 1);
        if (disableDelete) {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("Delete view")) {
            d.viewsShowDeleteConfirm = true;
        }
        ImGui::PopStyleColor(3);
        if (disableDelete) {
            ImGui::EndDisabled();
        }

        // Sub-row: status / sync hints.
        ImGui::TextDisabled("Active view  ·  press Ctrl+Enter to apply  ·  Ctrl+N to create a new view");
        ImGui::Separator();
    }

    // Tab bar.
    if (ImGui::BeginTabBar("##ViewsEditorTabs", ImGuiTabBarFlags_None)) {
        // -------- Filter tab --------
        if (ImGui::BeginTabItem("Filter")) {
            d.viewsActiveTab = Tab_Filter;
            ImGui::Spacing();
            ImGui::TextUnformatted("Name");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##ViewNameInput", d.viewNameBuf, sizeof(d.viewNameBuf))) {
                d.viewsDirty = true;
            }

            ImGui::Spacing();
            const bool isPlane = d.cfg.TrackerType == "Plane";
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(isPlane ? "Plane filter" : "JQL");
            ImGui::SameLine();
            const std::string currentJql(d.viewJqlBuf);
            const bool disableOpenJql = currentJql.empty() || d.cfg.Domain.empty() || isPlane;
            if (disableOpenJql) {
                ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton(isPlane ? "Open##Query" : "Open in browser")) {
                if (!isPlane) {
                    app.OpenUrl(app.BuildJqlSearchUrl(d.cfg, currentJql));
                }
            }
            if (disableOpenJql) {
                ImGui::EndDisabled();
            }

            const int beforeLen = static_cast<int>(std::strlen(d.viewJqlBuf));
            SmatchetViewsDashboardUiDetail::DrawJqlQueryEditorEmbedded(app, d);
            if (static_cast<int>(std::strlen(d.viewJqlBuf)) != beforeLen) {
                d.viewsDirty = true;
            }
            ImGui::TextDisabled(
                isPlane ? "field:value AND field:value  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list"
                        : "JQL tokens + catalog  ·  Up/Down list  ·  Enter/Tab pick  ·  Esc close list");

            if (d.jqlWantsApplyFromEnter) {
                d.jqlWantsApplyFromEnter = false;
                applyAndSync();
            }

            ImGui::EndTabItem();
        }

        // -------- Fields tab --------
        if (ImGui::BeginTabItem("Fields")) {
            d.viewsActiveTab = Tab_Fields;
            ImGui::Spacing();

            if (d.fieldCatalogLoading) {
                ImGui::TextDisabled("Loading available fields...");
            }

            // Build catalog snapshot used by both panes.
            std::unordered_set<std::string> selectedFieldSet;
            for (const auto& fieldId : SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf)) {
                selectedFieldSet.insert(fieldId);
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

                std::vector<const TrackerField*> visibleFields;
                std::vector<const TrackerField*> systemFields;
                std::vector<const TrackerField*> customFields;
                std::vector<const TrackerField*> basicFields;
                for (const auto& field : availableFields) {
                    if (!SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive(field.Id, d.fieldSearchBuf) &&
                        !SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive(field.Name, d.fieldSearchBuf)) {
                        continue;
                    }
                    visibleFields.push_back(&field);
                    if (field.IsCustom) {
                        customFields.push_back(&field);
                    } else if (IsBasicFieldId(field.Id)) {
                        basicFields.push_back(&field);
                    } else {
                        systemFields.push_back(&field);
                    }
                }
                const auto fieldSortLess = [](const TrackerField* lhs, const TrackerField* rhs) {
                    if (!lhs || !rhs) {
                        return lhs != nullptr;
                    }
                    if (lhs->Name != rhs->Name) {
                        return lhs->Name < rhs->Name;
                    }
                    return lhs->Id < rhs->Id;
                };
                std::sort(systemFields.begin(), systemFields.end(), fieldSortLess);
                std::sort(customFields.begin(), customFields.end(), fieldSortLess);

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
                    SyncSelectedFieldsBuffer(d, selectedFieldSet);
                    d.viewsDirty = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear visible")) {
                    for (const TrackerField* field : visibleFields) {
                        if (field) {
                            selectedFieldSet.erase(field->Id);
                        }
                    }
                    SyncSelectedFieldsBuffer(d, selectedFieldSet);
                    d.viewsDirty = true;
                }

                ImGui::Spacing();
                ImGui::BeginChild("##AvailableScroll", ImVec2(0, 0), false);

                const auto renderFieldGroup = [&](const char* groupName,
                                                  const std::vector<const TrackerField*>& fields) {
                    if (fields.empty()) {
                        return;
                    }
                    size_t selectedInGroup = 0;
                    for (const TrackerField* f : fields) {
                        if (f && selectedFieldSet.count(f->Id)) {
                            ++selectedInGroup;
                        }
                    }
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
                            SyncSelectedFieldsBuffer(d, selectedFieldSet);
                            d.viewsDirty = true;
                        }
                        ImGui::SameLine();
                        ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                    }
                };

                if (availableFields.empty()) {
                    ImGui::TextDisabled("No field catalog loaded yet.");
                } else if (visibleFields.empty()) {
                    ImGui::TextDisabled("No fields match current search.");
                } else {
                    // Basic group: ID (always selected, locked) + the six core Jira fields.
                    const bool hasVisibleId =
                        SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive("id", d.fieldSearchBuf);
                    if (!basicFields.empty() || hasVisibleId) {
                        size_t selectedInGroup = 1; // ID counts
                        for (const TrackerField* f : basicFields) {
                            if (f && selectedFieldSet.count(f->Id)) {
                                ++selectedInGroup;
                            }
                        }
                        const size_t total = basicFields.size() + 1;
                        const std::string label = std::string("Basic fields (") + std::to_string(selectedInGroup) +
                                                  "/" + std::to_string(total) + ")###grp_basic";
                        if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            bool idChecked = true;
                            ImGui::BeginDisabled();
                            ImGui::Checkbox("##ViewField_id", &idChecked);
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            ImGui::TextDisabled("ID (locked)");
                            const char* basicOrder[] = {"summary", "assignee", "priority",
                                                        "status",  "created",  "updated"};
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
                                    SyncSelectedFieldsBuffer(d, selectedFieldSet);
                                    d.viewsDirty = true;
                                }
                                ImGui::SameLine();
                                ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                            }
                        }
                    }
                    renderFieldGroup("System fields", systemFields);
                    renderFieldGroup("Custom fields", customFields);
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

        // -------- Columns tab --------
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
                // Read-only width hint pulled from the active view's saved widths.
                auto wit = activeView->ColumnWidths.find(key);
                if (wit != activeView->ColumnWidths.end() && wit->second > 0.0f) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("  %.0fpx", wit->second);
                }
                if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, d.editingColumnOrder,
                                                                     &d.viewsKeyboardReorderRow, "VIEWS_COLUMNS_ROW")) {
                    d.viewsDirty = true;
                }
                ImGui::PopID();
            }
            SmatchetViewsDashboardUiDetail::TickDragDropAutoScroll();
            ImGui::EndChild();

            ImGui::TextDisabled("Tip: Add or remove columns from the Fields tab. Resize columns directly in the grid.");

            ImGui::EndTabItem();
        }

        // -------- Sort tab --------
        if (ImGui::BeginTabItem("Sort")) {
            d.viewsActiveTab = Tab_Sort;
            ImGui::Spacing();
            ImGui::TextUnformatted("Sort order");
            ImGui::SameLine();
            ImGui::TextDisabled("— drag to reorder, click direction to toggle");

            // We mutate the active view's SortSpecs in-place; bump revision after.
            ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
            const auto& availableFields = app.GetAvailableFields();
            const float listHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.0f;

            ImGui::BeginChild("##SortScroll", ImVec2(0, listHeight), true);
            if (!mutableActive || mutableActive->SortSpecs.empty()) {
                ImGui::TextDisabled("(no sort keys) — click \"+ Add sort key\" below.");
            }
            if (mutableActive) {
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
                    SmatchetViewsDashboardUiDetail::DrawDragHandle("##h", i, "VIEWS_SORT_ROW");
                    const bool selected = (d.viewsKeyboardReorderRow == i);
                    const std::string label = PrettyColumnLabel(key, availableFields);
                    if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
                        d.viewsKeyboardReorderRow = i;
                    }
                    if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, keyOrder, &d.viewsKeyboardReorderRow,
                                                                         "VIEWS_SORT_ROW")) {
                        reordered = true;
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
                    if (ImGui::SmallButton("X")) {
                        SmatchetViewsDashboardUiDetail::SnapshotActiveViewIfNeeded(d, *mutableActive);
                        mutableActive->SortSpecs.erase(mutableActive->SortSpecs.begin() + i);
                        keyOrder.erase(keyOrder.begin() + i);
                        ViewState.BumpRevision();
                        d.viewsDirty = true;
                        ImGui::PopID();
                        break;
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
            SmatchetViewsDashboardUiDetail::TickDragDropAutoScroll();
            ImGui::EndChild();

            // + Add sort key (popup picker scoped to current column order).
            if (ImGui::Button("+ Add sort key")) {
                ImGui::OpenPopup("##AddSortKeyPopup");
            }
            if (ImGui::BeginPopup("##AddSortKeyPopup")) {
                if (mutableActive) {
                    for (const auto& key : d.editingColumnOrder) {
                        const bool alreadyUsed =
                            std::any_of(mutableActive->SortSpecs.begin(), mutableActive->SortSpecs.end(),
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

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild(); // ViewsEditor

    // -------- Discard-confirm modal (pending activate) --------
    if (d.viewsShowDiscardConfirm) {
        ImGui::OpenPopup("Discard changes?");
        d.viewsShowDiscardConfirm = false;
    }
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("You have unsaved changes on this view.");
        ImGui::Spacing();
        if (ImGui::Button("Save & switch")) {
            applyAndSync();
            if (!d.viewsPendingActivateId.empty()) {
                activateView(d.viewsPendingActivateId);
                d.viewsPendingActivateId.clear();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard & switch")) {
            if (!d.viewsPendingActivateId.empty()) {
                activateView(d.viewsPendingActivateId);
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
        ImGui::Text("Delete view \"%s\"? This cannot be undone.", activeView->Name.c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("Delete")) {
            const std::string deletedName = activeView->Name;
            if (ViewState.DeleteActive()) {
                const ViewDefinition* nowActive = ViewState.GetActiveView();
                if (nowActive) {
                    LoadBuffersFromView(d, *nowActive);
                    d.cfg.JqlQuery = nowActive->Jql;
                    d.cfg.SelectedFields = nowActive->Fields;
                    SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                }
                SmatchetToastManager::Instance().Push("View deleted", deletedName, ToastType::Info, 1800);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
