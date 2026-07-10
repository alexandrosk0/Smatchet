#include "TrackerLabelsEditor.h"
#include "AppController.h"
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
using TrackerLabelsPure::FilterSuggestionsForDisplay;
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

// Draw the open labels-combo body: the search box, the suggestion checkboxes,
// and the add-draft row. Owns the inner PushID/PopID pair. `queue` applies the
// intended full label set (set-replace).
void DrawLabelsComboBody(const std::string& editorKey, const CachedTicket& ticket, const TrackerField& field,
                         const std::vector<std::string>& selectedLabels, const std::vector<std::string>& suggestions,
                         const std::function<void(const std::vector<std::string>&)>& queue) {
    static std::string activeEditorKey;
    static char draftBuf[128] = "";
    static char searchBuf[128] = "";
    ImGui::PushID(editorKey.c_str());
    if (activeEditorKey != editorKey) {
        activeEditorKey = editorKey;
        draftBuf[0] = '\0';
        searchBuf[0] = '\0';
    }

    if (ImGui::Selectable("<clear all>", selectedLabels.empty())) {
        queue({});
    }
    ImGui::Separator();

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##LabelSearch", "Search labels", searchBuf, sizeof(searchBuf));
    ImGui::Separator();

    const std::string filterLower = ToLowerAsciiCopy(TrimCopy(searchBuf));
    const std::vector<std::string> visibleSuggestions =
        FilterSuggestionsForDisplay(suggestions, selectedLabels, filterLower);

    if (visibleSuggestions.empty()) {
        if (suggestions.empty()) {
            ImGui::TextDisabled("(no labels discovered yet)");
        } else {
            ImGui::TextDisabled("(no matching labels)");
        }
    }

    for (const auto& suggestion : visibleSuggestions) {
        bool checked = ContainsLabelCaseInsensitive(selectedLabels, suggestion);
        const std::string optionWidget = suggestion + "##Label_" + ticket.id + "_" + field.Id + "_" + suggestion;
        if (ImGui::Checkbox(optionWidget.c_str(), &checked)) {
            std::vector<std::string> updated = selectedLabels;
            if (checked && !ContainsLabelCaseInsensitive(updated, suggestion)) {
                updated.push_back(suggestion);
            } else {
                updated.erase(std::remove_if(updated.begin(), updated.end(),
                                             [&](const std::string& label) {
                                                 return LabelsEqualCaseInsensitive(label, suggestion);
                                             }),
                              updated.end());
            }
            updated = SortAndUniqueLabels(std::move(updated));
            queue(updated);
        }
    }

    ImGui::Separator();
    const bool submitted = ImGui::InputTextWithHint("##NewLabelDraft", "Add label", draftBuf, sizeof(draftBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const std::string trimmedDraft = TrimCopy(draftBuf);
    const bool canAddDraft = !trimmedDraft.empty() && !ContainsLabelCaseInsensitive(selectedLabels, trimmedDraft);
    if (!canAddDraft) {
        ImGui::BeginDisabled();
    }
    const bool addClicked = ImGui::Button("Add");
    if (!canAddDraft) {
        ImGui::EndDisabled();
    }
    if ((submitted || addClicked) && canAddDraft) {
        std::vector<std::string> updated = selectedLabels;
        updated.push_back(trimmedDraft);
        updated = SortAndUniqueLabels(std::move(updated));
        queue(updated);
        draftBuf[0] = '\0';
    }
    ImGui::PopID();
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
    if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        DrawLabelsComboBody(editorKey, ticket, field, selectedLabels, suggestions, queue);
        ImGui::EndCombo();
    }
}

} // namespace TrackerLabelsEditor
