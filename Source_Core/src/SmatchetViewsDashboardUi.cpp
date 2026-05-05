#include "SmatchetUI.h"

#include "SmatchetViewsDashboardUi_detail.h"
#include "AppController.h"
#include "Views.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

void SmatchetUI::drawViewsDashboardWindow(AppController& app, UiDrawSession& d) {
    if (!d.showViewsDashboard) {
        return;
    }
    // With DockSpaceOverViewport, "Views" often shares a tab bar with "Smatchet - Active Project"
    // (drawn later). showViewsDashboard can stay true while the Views tab is hidden behind another;
    // Open Views / menu then appears to do nothing unless we explicitly focus this window.
    const bool bFocusViews = d.requestViewsDashboardFocus;
    if (bFocusViews) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    const std::string viewsWinTitle =
        std::string("Views - ") + ConfigManager::NormalizeViewsBackendKey(d.cfg.TrackerType);
    ImGui::Begin(viewsWinTitle.c_str(), &d.showViewsDashboard);
    if (bFocusViews) {
        ImGui::SetWindowFocus();
        d.requestViewsDashboardFocus = false;
    }

    ViewsStore& store = ViewState.GetStoreMutable();
    if (store.Views.empty()) {
        ViewState.EnsureLoaded(d.cfg);
    }

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

    auto loadBuffersFromView = [&](const ViewDefinition& view) {
        std::memset(d.fieldSearchBuf, 0, sizeof(d.fieldSearchBuf));
        d.jqlAcpApplyReplace = false;
        d.jqlAcpReplaceStart = -1;
        d.jqlAcpReplaceEnd = -1;
        d.jqlAcpReplaceText.clear();
        d.jqlAcpListSelected = 0;
        d.jqlAcpLastCursor = 0;
        d.jqlAcpLastSelectionStart = 0;
        d.jqlAcpLastSelectionEnd = 0;
        d.jqlAcpWantsJqlInputFocus = false;
        d.jqlAcpScrollToSelected = false;
        d.jqlAcpWantsCursorPos = -1;
        d.jqlAcpPendingMouseCaretAfterPick = false;
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
    };

    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (activeView) {
        if (d.editingViewId != activeView->Id) {
            loadBuffersFromView(*activeView);
        } else if (activeView->ColumnOrder != d.lastSyncedColumnOrder) {
            loadBuffersFromView(*activeView);
        }
    }

    if (activeView) {
        ImGui::Text("Active View");
        if (ImGui::BeginCombo("##ActiveViewCombo", activeView->Name.c_str())) {
            for (const auto& view : store.Views) {
                const bool isSelected = (view.Id == activeView->Id);
                if (ImGui::Selectable(view.Name.c_str(), isSelected)) {
                    if (ViewState.Activate(view.Id)) {
                        const ViewDefinition* nowActive = ViewState.GetActiveView();
                        if (nowActive) {
                            d.cfg.JqlQuery = nowActive->Jql;
                            d.cfg.SelectedFields = nowActive->Fields;
                            SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                        }
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool disableBackNav = !d.navHistory.CanGoBack();
        if (disableBackNav) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("<")) {
            const NavigationEntry* entry = d.navHistory.GoBack();
            if (entry) {
                d.cfg.JqlQuery = entry->Jql;
                SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, d.cfg.JqlQuery);
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), false);
            }
        }
        if (disableBackNav) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Run the previous query from navigation history.");
        }
        ImGui::SameLine();
        const bool disableForwardNav = !d.navHistory.CanGoForward();
        if (disableForwardNav) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(">")) {
            const NavigationEntry* entry = d.navHistory.GoForward();
            if (entry) {
                d.cfg.JqlQuery = entry->Jql;
                SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.viewJqlBuf, d.cfg.JqlQuery);
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), false);
            }
        }
        if (disableForwardNav) {
            ImGui::EndDisabled();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Run the next query from navigation history.");
        }

        ImGui::InputText("View Name", d.viewNameBuf, sizeof(d.viewNameBuf));
        SmatchetViewsDashboardUiDetail::DrawJqlQueryEditor(app, d, ViewState, *activeView);


        ImGui::Spacing();
        if (ImGui::Button("Apply View & Sync")) {
            ViewDefinition updated = *activeView;
            updated.Name = d.viewNameBuf;
            updated.Jql = d.viewJqlBuf;
            updated.Fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
            updated.ColumnOrder = d.editingColumnOrder;
            if (ViewState.UpdateActive(updated)) {
                d.cfg.JqlQuery = updated.Jql;
                d.cfg.SelectedFields = updated.Fields;
                SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Create New View")) {
            ViewDefinition created = *activeView;
            created.Name = std::string(d.viewNameBuf).empty() ? std::string("New View") : std::string(d.viewNameBuf);
            created.Jql = d.viewJqlBuf;
            created.Fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
            created.ColumnOrder = d.editingColumnOrder;
            created.Id.clear();
            ViewState.Create(created);
            const ViewDefinition* nowActive = ViewState.GetActiveView();
            if (nowActive) {
                loadBuffersFromView(*nowActive);
            }
        }
        ImGui::SameLine();
        const bool disableDeleteView = (store.Views.size() <= 1);
        if (disableDeleteView) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Delete View")) {
            if (ViewState.DeleteActive()) {
                const ViewDefinition* nowActive = ViewState.GetActiveView();
                if (nowActive) {
                    loadBuffersFromView(*nowActive);
                    d.cfg.JqlQuery = nowActive->Jql;
                    d.cfg.SelectedFields = nowActive->Fields;
                    SmatchetViewsDashboardUiDetail::SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                }
            }
        }
        if (disableDeleteView) {
            ImGui::EndDisabled();
        }

        if (d.fieldCatalogLoading) {
            ImGui::TextDisabled("Loading available fields...");
        }

        ImGui::Separator();
        ImGui::Text("Field Picker");
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputTextWithHint("##ViewFieldSearch", "Search by field id or name", d.fieldSearchBuf,
                                 sizeof(d.fieldSearchBuf));

        std::unordered_set<std::string> selectedFieldSet;
        for (const auto& fieldId : SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf)) {
            selectedFieldSet.insert(fieldId);
        }

        const auto& availableFields = app.GetAvailableFields();
        std::vector<const TrackerField*> visibleFields;
        std::vector<const TrackerField*> systemFields;
        std::vector<const TrackerField*> customFields;
        std::vector<const TrackerField*> basicFields;

        auto isBasicFieldId = [](const std::string& id) {
            return id == "summary" || id == "assignee" || id == "priority" || id == "status" || id == "created" ||
                   id == "updated";
        };

        for (const auto& field : availableFields) {
            if (!SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive(field.Id, d.fieldSearchBuf) &&
                !SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive(field.Name, d.fieldSearchBuf)) {
                continue;
            }
            visibleFields.push_back(&field);
            if (field.IsCustom) {
                customFields.push_back(&field);
            } else if (isBasicFieldId(field.Id)) {
                basicFields.push_back(&field);
            } else {
                systemFields.push_back(&field);
            }
        }

        const auto fieldSortLess = [](const TrackerField* lhs, const TrackerField* rhs) {
            if (!lhs || !rhs)
                return lhs != nullptr;
            if (lhs->Name != rhs->Name)
                return lhs->Name < rhs->Name;
            return lhs->Id < rhs->Id;
        };
        std::sort(systemFields.begin(), systemFields.end(), fieldSortLess);
        std::sort(customFields.begin(), customFields.end(), fieldSortLess);

        const auto syncSelectedFieldsBuffer = [&]() {
            const std::vector<std::string> updated = SmatchetViewsDashboardUiDetail::ToSortedVector(selectedFieldSet);
            const std::string csv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(updated);
            SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, csv);
        };

        const bool disableFieldCatalogEditing = d.fieldCatalogLoading;
        if (disableFieldCatalogEditing) {
            ImGui::BeginDisabled();
        }

        ImGui::TextDisabled("Selected: %zu", selectedFieldSet.size());
        ImGui::SameLine();
        ImGui::TextDisabled("Visible: %zu", visibleFields.size());
        ImGui::SameLine();
        if (ImGui::Button("Select All Visible")) {
            for (const TrackerField* field : visibleFields)
                if (field)
                    selectedFieldSet.insert(field->Id);
            syncSelectedFieldsBuffer();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Visible")) {
            for (const TrackerField* field : visibleFields)
                if (field)
                    selectedFieldSet.erase(field->Id);
            syncSelectedFieldsBuffer();
        }

        const auto renderFieldGroup = [&](const char* groupName, const std::vector<const TrackerField*>& fields) {
            if (fields.empty())
                return;
            const std::string label = std::string(groupName) + " (" + std::to_string(fields.size()) + ")";
            if (!ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                return;
            for (const TrackerField* field : fields) {
                bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                const std::string checkboxId = "##ViewField_" + field->Id;
                if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                    if (checked)
                        selectedFieldSet.insert(field->Id);
                    else
                        selectedFieldSet.erase(field->Id);
                    syncSelectedFieldsBuffer();
                }
                ImGui::SameLine();
                ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
            }
        };

        ImGui::BeginChild("ViewFieldsList", ImVec2(0, 220), true);
        if (availableFields.empty()) {
            ImGui::TextDisabled("No field catalog loaded yet.");
        } else if (visibleFields.empty()) {
            ImGui::TextDisabled("No fields match current search.");
        } else {
            // Basic Fields group: ID (always shown) + core Jira fields.
            {
                const char* groupName = "Basic Fields";
                const bool hasVisibleId = SmatchetViewsDashboardUiDetail::ContainsCaseInsensitive("id", d.fieldSearchBuf);
                if (!basicFields.empty() || hasVisibleId) {
                    const std::size_t count = basicFields.size() + 1; // +1 for ID
                    const std::string label = std::string(groupName) + " (" + std::to_string(count) + ")";
                    if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        // ID: always selected, not toggleable.
                        bool idChecked = true;
                        ImGui::Checkbox("##ViewField_id", &idChecked);
                        ImGui::SameLine();
                        ImGui::Text("ID (id, always selected)");

                        auto renderBasicById = [&](const char* fid) {
                            auto it = std::find_if(basicFields.begin(), basicFields.end(),
                                                   [&](const TrackerField* f) { return f && f->Id == fid; });
                            if (it == basicFields.end() || !*it)
                                return;
                            const TrackerField* field = *it;
                            bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                            const std::string checkboxId = "##ViewField_" + field->Id;
                            if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                                if (checked)
                                    selectedFieldSet.insert(field->Id);
                                else
                                    selectedFieldSet.erase(field->Id);
                                syncSelectedFieldsBuffer();
                            }
                            ImGui::SameLine();
                            ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                        };

                        renderBasicById("summary");
                        renderBasicById("assignee");
                        renderBasicById("priority");
                        renderBasicById("status");
                        renderBasicById("created");
                        renderBasicById("updated");
                    }
                }
            }

            renderFieldGroup("System Fields", systemFields);
            renderFieldGroup("Custom Fields", customFields);
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Text("Column Order");
        ImGui::SameLine();
        ImGui::TextDisabled("(right-click a column to remove it from the view)");
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
            if (std::find(d.editingColumnOrder.begin(), d.editingColumnOrder.end(), key) ==
                d.editingColumnOrder.end()) {
                d.editingColumnOrder.push_back(key);
            }
        }

        ImGui::BeginChild("ColumnOrderList", ImVec2(0, 120), true);
        for (int i = 0; i < static_cast<int>(d.editingColumnOrder.size()); ++i) {
            const std::string& key = d.editingColumnOrder[static_cast<size_t>(i)];
            const bool selected = (d.selectedColumnOrderIndex == i);
            if (ImGui::Selectable(key.c_str(), selected)) {
                d.selectedColumnOrderIndex = i;
            }
            if (ImGui::BeginPopupContextItem()) {
                if (key != "id" && ImGui::MenuItem("Remove column from view")) {
                    d.editingColumnOrder.erase(d.editingColumnOrder.begin() + i);
                    if (d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size())) {
                        d.selectedColumnOrderIndex = static_cast<int>(d.editingColumnOrder.size()) - 1;
                    }
                    if (key.rfind("field:", 0) == 0) {
                        const std::string fieldId = key.substr(6);
                        std::vector<std::string> fields = SmatchetViewsDashboardUiDetail::ParseCsv(d.selectedFieldsBuf);
                        fields.erase(std::remove(fields.begin(), fields.end(), fieldId), fields.end());
                        const std::string csv = SmatchetViewsDashboardUiDetail::JoinCsvLocal(fields);
                        SmatchetViewsDashboardUiDetail::CopyStringToBuffer(d.selectedFieldsBuf, csv);
                        selectedFieldSet.clear();
                        for (const auto& fid : fields) {
                            selectedFieldSet.insert(fid);
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        const bool disableColumnMoveButtons =
            d.selectedColumnOrderIndex < 0 ||
            d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size());
        if (disableColumnMoveButtons) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Move Up") && d.selectedColumnOrderIndex > 0) {
            std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                      d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex - 1)]);
            --d.selectedColumnOrderIndex;
        }
        ImGui::SameLine();
        if (ImGui::Button("Move Down") && d.selectedColumnOrderIndex >= 0 &&
            d.selectedColumnOrderIndex < static_cast<int>(d.editingColumnOrder.size()) - 1) {
            std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                      d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex + 1)]);
            ++d.selectedColumnOrderIndex;
        }
        if (disableColumnMoveButtons) {
            ImGui::EndDisabled();
        }

        if (disableFieldCatalogEditing) {
            ImGui::EndDisabled();
        }
    } else {
        ImGui::TextDisabled("No views available.");
    }

    ImGui::End();
}
