#include "TrackerLabelsEditor.h"
#include "AppController.h"
#include "FilterDropdown.h"
#include "RecentSelections.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"
#include "TrackerLabelsPure.h"
#include "TouchCellEditGesture.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using TrackerLabelsPure::ContainsLabelCaseInsensitive;
using TrackerLabelsPure::LabelsEqualCaseInsensitive;
using TrackerLabelsPure::ParseCsv;
using TrackerLabelsPure::SortAndUniqueLabels;

namespace {

std::vector<std::string> CollectLabelSuggestions(const AppController& app, const CachedTicket& ticket,
                                                 const std::string& currentValue) {
    std::vector<std::string> labels;
    const auto appendLabels = [&labels](const std::string& rawLabels) {
        for (const auto& part : ParseCsv(rawLabels)) {
            const std::string trimmed = TrimCopy(part);
            if (!trimmed.empty()) {
                labels.push_back(trimmed);
            }
        }
    };

    appendLabels(currentValue);
    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;
    for (const auto& candidate : tickets) {
        appendLabels(candidate.GetFieldValue("labels"));
    }
    appendLabels(ticket.GetFieldValue("labels"));
    return SortAndUniqueLabels(std::move(labels));
}

// Draws the open labels-combo body through the shared dropdown in tick-box mode: clear-all, filter,
// recent picks, and an 'Add "<text>"' row for a label that does not exist yet. `queue` applies the
// intended full label set (set-replace).
void DrawLabelsComboBody(const std::vector<std::string>& selectedLabels, const std::vector<std::string>& suggestions,
                         const std::string& recentsScope,
                         const std::function<void(const std::vector<std::string>&)>& queue) {
    std::vector<smatchet::dropdown::Item> items;
    items.reserve(suggestions.size());
    for (const auto& suggestion : suggestions) {
        smatchet::dropdown::Item item;
        item.Id = suggestion.c_str();
        item.Label = suggestion.c_str();
        item.Checked = ContainsLabelCaseInsensitive(selectedLabels, suggestion);
        items.push_back(item);
    }
    smatchet::dropdown::Options options =
        smatchet::dropdown::TickBoxes(recentsScope, SmatchetLocalization::T("field.clear_all", "<clear all>"));
    options.AllowCreate = true;
    options.EmptyText = SmatchetLocalization::T("field.no_labels", "(no labels discovered yet)");
    const smatchet::dropdown::Event ev = smatchet::dropdown::DrawBody(items, options);
    if (ev.Kind == smatchet::dropdown::EventKind::Cleared) {
        if (!selectedLabels.empty()) {
            queue({});
        }
        return;
    }
    if (ev.Kind != smatchet::dropdown::EventKind::Picked && ev.Kind != smatchet::dropdown::EventKind::Created) {
        return;
    }
    std::vector<std::string> updated = selectedLabels;
    if (!ContainsLabelCaseInsensitive(updated, ev.Id)) {
        updated.push_back(ev.Id);
    } else if (ev.Kind == smatchet::dropdown::EventKind::Picked) {
        updated.erase(
            std::remove_if(updated.begin(), updated.end(),
                           [&](const std::string& label) { return LabelsEqualCaseInsensitive(label, ev.Id); }),
            updated.end());
    } else {
        return; // "Add" of a label that is already set: nothing to change
    }
    queue(SortAndUniqueLabels(std::move(updated)));
}

} // namespace

namespace TrackerLabelsEditor {

bool IsLabelsField(const std::string& fieldId) { return ToLowerAsciiCopy(fieldId) == "labels"; }

void RenderLabelsFieldEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, const QueueLabelEditFn& queueEdit,
                             SpreadsheetState& state, bool singleClickToEdit) {
    const auto queue = [&queueEdit, &ticket, &field](const std::vector<std::string>& values) {
        queueEdit(ticket.id, field, values);
    };

    std::vector<std::string> selectedLabels = SortAndUniqueLabels(ParseCsv(currentValue));
    std::vector<std::string> suggestions = CollectLabelSuggestions(app, ticket, currentValue);
    std::copy_if(selectedLabels.begin(), selectedLabels.end(), std::back_inserter(suggestions),
                 [&](const auto& selectedLabel) { return !ContainsLabelCaseInsensitive(suggestions, selectedLabel); });
    suggestions = SortAndUniqueLabels(std::move(suggestions));

    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    if (preview.empty() && !currentValue.empty()) {
        // Fallback if ResolveDisplayValue returns empty for non-empty value (should not happen with robust backends)
    }
    const std::string comboId = "##LabelsMultiSelect_" + ticket.id + "_" + field.Id;
    const std::string editorKey = ticket.id + "::" + field.Id;
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    // Arm-then-popup: see TicketFieldEditor::RenderSingleSelectEditor for the contract.
    const char* previewCStr = preview.empty() ? "" : preview.c_str();
    const std::string selLabel = std::string(previewCStr) + "##LabelsPreview_" + ticket.id + "_" + field.Id;
    if (!SmatchetTouchEdit::ArmThenPopupCellGate(state.EditArmedKey, state.EditArmedJustOpened, editorKey,
                                                 comboId.c_str(), selLabel.c_str(), cellAvail, singleClickToEdit)) {
        return;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(),
                          ImGuiComboFlags_NoArrowButton | ImGuiComboFlags_HeightLargest)) {
        DrawLabelsComboBody(selectedLabels, suggestions,
                            smatchet::recents::MakeScopeKey(app.FocusedCacheBackendKey(), field.Id), queue);
        ImGui::EndCombo();
    }
}

} // namespace TrackerLabelsEditor
