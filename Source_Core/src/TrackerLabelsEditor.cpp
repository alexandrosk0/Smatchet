#include "TrackerLabelsEditor.h"
#include "AppController.h"
#include "StringUtil.h"
#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            const std::string trimmed = TrimCopy(current);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    const std::string trimmed = TrimCopy(current);
    if (!trimmed.empty()) {
        result.push_back(trimmed);
    }
    return result;
}



bool LabelsEqualCaseInsensitive(const std::string& a, const std::string& b) {
    return ToLowerAsciiCopy(a) == ToLowerAsciiCopy(b);
}

bool ContainsLabelCaseInsensitive(const std::vector<std::string>& values, const std::string& needle) {
    return std::any_of(values.begin(), values.end(), [&](const auto& value) {
        return LabelsEqualCaseInsensitive(value, needle);
    });
}

std::vector<std::string> SortAndUniqueLabels(std::vector<std::string> values) {
    std::vector<std::string> trimmedValues;
    trimmedValues.reserve(values.size());
    for (const auto& value : values) {
        const std::string trimmed = TrimCopy(value);
        if (!trimmed.empty()) {
            trimmedValues.push_back(trimmed);
        }
    }
    values = std::move(trimmedValues);
    std::sort(values.begin(), values.end(), [](const std::string& a, const std::string& b) {
        const std::string lowerA = ToLowerAsciiCopy(a);
        const std::string lowerB = ToLowerAsciiCopy(b);
        if (lowerA != lowerB) {
            return lowerA < lowerB;
        }
        return a < b;
    });
    values.erase(
        std::unique(values.begin(), values.end(),
                    [](const std::string& a, const std::string& b) { return LabelsEqualCaseInsensitive(a, b); }),
        values.end());
    return values;
}

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

bool LabelMatchesFilter(const std::string& label, const std::string& filterLower) {
    if (filterLower.empty()) {
        return true;
    }
    return ToLowerAsciiCopy(label).find(filterLower) != std::string::npos;
}

std::vector<std::string> FilterSuggestionsForDisplay(const std::vector<std::string>& suggestions,
                                                     const std::vector<std::string>& selectedLabels,
                                                     const std::string& filterLower) {
    std::vector<std::string> out;
    out.reserve(suggestions.size());
    std::copy_if(suggestions.begin(), suggestions.end(), std::back_inserter(out), [&](const auto& suggestion) {
        return ContainsLabelCaseInsensitive(selectedLabels, suggestion) || LabelMatchesFilter(suggestion, filterLower);
    });
    return out;
}

} // namespace

namespace TrackerLabelsEditor {

bool IsLabelsField(const std::string& fieldId) { return ToLowerAsciiCopy(fieldId) == "labels"; }

void RenderLabelsFieldEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, const QueueLabelEditFn& queueEdit) {
    const auto queue = [&queueEdit, &ticket, &field](const std::vector<std::string>& values) {
        queueEdit(ticket.id, field, values);
    };

    std::vector<std::string> selectedLabels = SortAndUniqueLabels(ParseCsv(currentValue));
    std::vector<std::string> suggestions = CollectLabelSuggestions(app, ticket, currentValue);
    std::copy_if(selectedLabels.begin(), selectedLabels.end(), std::back_inserter(suggestions), [&](const auto& selectedLabel) {
        return !ContainsLabelCaseInsensitive(suggestions, selectedLabel);
    });
    suggestions = SortAndUniqueLabels(std::move(suggestions));

    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    if (preview.empty() && !currentValue.empty()) {
        // Fallback if ResolveDisplayValue returns empty for non-empty value (should not happen with robust backends)
    }
    const std::string comboId = "##LabelsMultiSelect_" + ticket.id + "_" + field.Id;
    const std::string editorKey = ticket.id + "::" + field.Id;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
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
        ImGui::EndCombo();
    }
}

} // namespace TrackerLabelsEditor






