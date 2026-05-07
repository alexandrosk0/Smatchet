#include "TicketFieldEditor.h"
#include "UiPerfMonitor.h"

#include "AppController.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "SmatchetFieldIconRender.h"
#include "SmatchetFieldRender.h"
#include "TrackerFieldValueUtils.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace TrackerFieldValueUtils;

// #region agent log
struct EditCbUser {
    SpreadsheetState* state;
    const char* fieldId;
};

static void AgentLogTicketField(const char* hypothesisId, const char* location, int a, int b, int c, int d) {
    FILE* f = std::fopen("c:/Dev/Smatchet/debug-e99704.log", "a");
    if (!f) {
        return;
    }
    const long long ts = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    std::fprintf(f,
                 "{\"sessionId\":\"e99704\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"tick\",\"data\":{"
                 "\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d},\"timestamp\":%lld}\n",
                 hypothesisId, location, a, b, c, d, ts);
    std::fclose(f);
}
// #endregion agent log

/** Collapse ImGui's initial select-all when a grid text cell opens. `EventActivated` can arrive the frame after
 *  `EditJustStarted` is cleared; use `PendingGridInputTextDeselect` until we see activation or full-range selection. */
static int InputTextCallback_ClearSelectOnEditOpen(ImGuiInputTextCallbackData* data) {
    auto* u = static_cast<EditCbUser*>(data->UserData);
    if (!u || !u->state) {
        return 0;
    }
    SpreadsheetState* st = u->state;
    if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways) {
        return 0;
    }
    const bool isSummary = u->fieldId && std::strcmp(u->fieldId, "summary") == 0;
    if (isSummary && (data->EventActivated || st->EditJustStarted || st->PendingGridInputTextDeselect)) {
        static int s_summaryCbLogBudget;
        if (s_summaryCbLogBudget < 80) {
            ++s_summaryCbLogBudget;
            AgentLogTicketField("B", "TicketFieldEditor:InputText_cb_always", data->EventActivated ? 1 : 0,
                                st->PendingGridInputTextDeselect ? 1 : 0, data->SelectionStart, data->SelectionEnd);
        }
    }
    if (!st->PendingGridInputTextDeselect) {
        return 0;
    }
    const bool fullRange =
        data->BufTextLen > 0 && data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen;
    if (!data->EventActivated && !fullRange) {
        return 0;
    }
    const int end = data->BufTextLen;
    data->SetSelection(end, end);
    st->PendingGridInputTextDeselect = false;
    if (isSummary) {
        AgentLogTicketField("C", "TicketFieldEditor:InputText_cb_after_set", data->CursorPos, data->SelectionStart,
                            data->SelectionEnd, end);
        AgentLogTicketField("F", "TicketFieldEditor:collapse_applied", data->EventActivated ? 1 : 0, fullRange ? 1 : 0,
                            0, 0);
    }
    return 0;
}

void QueueEdit(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
               std::vector<PendingFieldEdit>& pendingEdits) {
    PendingFieldEdit edit;
    edit.IssueId = issueId;
    edit.Field = field;
    edit.Values = values;
    pendingEdits.push_back(std::move(edit));
}

std::string EncodeCascadingSelection(const std::string& parentId, const std::string& childId) {
    return parentId + "\x1f" + childId;
}

void RenderTextEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                      const std::string& currentValue,
                      SpreadsheetState& state, std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled,
                      float availWidth) {
    const std::string itemId = "##TextCell_" + ticket.id + "_" + field.Id;
    if (state.IsEditingField(ticket.id, field.Id)) {
        const bool editJustStarted = state.EditJustStarted;
        if (field.Id == "summary") {
            static int s_enterEditLogBudget;
            if (s_enterEditLogBudget < 30) {
                ++s_enterEditLogBudget;
                AgentLogTicketField("E", "TicketFieldEditor:enter_edit", editJustStarted ? 1 : 0, 0, 0, 0);
            }
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (editJustStarted) {
            ImGui::SetKeyboardFocusHere();
        }
        EditCbUser cbUser{&state, field.Id.c_str()};
        const bool submitted =
            ImGui::InputText(itemId.c_str(), state.EditBuffer, sizeof(state.EditBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                             InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            state.ClearEditing();
        } else if (submitted || (!editJustStarted && ImGui::IsItemDeactivatedAfterEdit())) {
            QueueEdit(ticket.id, field, {state.EditBuffer}, pendingEdits);
            state.ClearEditing();
        } else if (editJustStarted) {
            state.EditJustStarted = false;
        }
        return;
    }

    const std::string valueForDisplay = app.ResolveDisplayValue(field.Id, &field, currentValue);
    bool hasNewlineInValue = false;
    for (size_t i = 0; i < valueForDisplay.size(); ++i) {
        if (valueForDisplay[i] == '\n' || valueForDisplay[i] == '\r') {
            hasNewlineInValue = true;
            break;
        }
    }
    std::string singleLine = valueForDisplay;
    for (size_t i = 0; i < singleLine.size(); ++i) {
        if (singleLine[i] == '\n' || singleLine[i] == '\r') {
            singleLine.erase(i);
            break;
        }
    }
    const std::string& display = singleLine;
    const float regionAvail = (availWidth > 0.0f) ? availWidth : ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable((display + itemId).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
            state.StartEditingField(ticket.id, field.Id, currentValue);
        }
    }
    const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    const bool horizontallyClipped = (regionAvail > 0.0f && textSize.x > regionAvail + 1.0f);
    if (tooltipsEnabled && (hasNewlineInValue || horizontallyClipped) && ImGui::IsItemHovered()) {
        const std::string* rawTip = IsTrackerDateOrDateTimeField(field.Id, &field) ? &currentValue : nullptr;
        const std::string& tipSource = (rawTip && !rawTip->empty()) ? *rawTip : valueForDisplay;
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tipSource.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void RenderSingleSelectEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                              const std::string& currentValue, std::vector<PendingFieldEdit>& pendingEdits,
                              bool tooltipsEnabled) {
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    SmatchetLoadedIconTexture overlayIcon{};
    std::string overlayLoadErr;
    const bool haveOverlayIcon =
        SmatchetFieldIconRender::TryGetInlineFieldIconTexture(app, field, currentValue, overlayIcon, overlayLoadErr);
    (void)overlayLoadErr;

    const std::string currentId = ResolveOptionId(field, currentValue);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    const std::string comboId = "##SingleSelect_" + ticket.id + "_" + field.Id;
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    const char* previewCStr =
        haveOverlayIcon ? " " : (preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str());
    ImGui::SetNextItemWidth(cellAvail);
    const bool comboOpened = ImGui::BeginCombo(comboId.c_str(), previewCStr, ImGuiComboFlags_NoArrowButton);
    const ImVec2 comboMin = ImGui::GetItemRectMin();
    const ImVec2 comboMax = ImGui::GetItemRectMax();
    if (comboOpened) {
        const bool selectedNone = currentId.empty();
        if (ImGui::Selectable("<clear>", selectedNone)) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }

        for (const auto& option : field.AllowedValueOptions) {
            const bool isSelected = (option.Id == currentId);
            if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
                QueueEdit(ticket.id, field, {option.Id}, pendingEdits);
            }
        }
        ImGui::EndCombo();
    }

    ImVec2 overlayP0(0.0f, 0.0f);
    ImVec2 overlayP1(0.0f, 0.0f);
    if (haveOverlayIcon && overlayIcon.Texture != nullptr && overlayIcon.Width > 0 && overlayIcon.Height > 0) {
        const float maxEdge = ImGui::GetFrameHeight();
        const float iw = static_cast<float>(overlayIcon.Width);
        const float ih = static_cast<float>(overlayIcon.Height);
        const float scale = maxEdge / (std::max)(iw, ih);
        const float dw = iw * scale;
        const float dh = ih * scale;
        const float rowH = comboMax.y - comboMin.y;
        overlayP0 = ImVec2(comboMin.x + 4.0f, comboMin.y + ((rowH - dh) * 0.5f));
        overlayP1 = ImVec2(overlayP0.x + dw, overlayP0.y + dh);
        ImGui::GetWindowDrawList()->AddImage(overlayIcon.Texture->GetTexRef(), overlayP0, overlayP1, ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f));
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(previewCStr);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void RenderMultiSelectEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue,
                             std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled) {
    std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(field, currentValue);
    std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    const std::string comboId = "##MultiSelect_" + ticket.id + "_" + field.Id;
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();

        for (const auto& option : field.AllowedValueOptions) {
            bool checked = (selectedSet.find(option.Id) != selectedSet.end());
            const std::string optionWidget = option.Value + "##Opt_" + ticket.id + "_" + field.Id + "_" + option.Id;
            if (ImGui::Checkbox(optionWidget.c_str(), &checked)) {
                if (checked) {
                    selectedSet.insert(option.Id);
                } else {
                    selectedSet.erase(option.Id);
                }
                std::vector<std::string> updated(selectedSet.begin(), selectedSet.end());
                std::sort(updated.begin(), updated.end());
                QueueEdit(ticket.id, field, updated, pendingEdits);
            }
        }
        ImGui::EndCombo();
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void RenderCascadingSelectEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                                 const std::string& currentValue,
                                 std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled) {
    std::string parentId;
    std::string childId;
    TryResolveCascadingSelection(field, currentValue, parentId, childId);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);

    const std::string comboId = "##CascadeSelect_" + ticket.id + "_" + field.Id;
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("<clear>", parentId.empty() && childId.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();
        for (const auto& parent : field.AllowedValueOptions) {
            if (parent.Children.empty()) {
                const bool selected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable(parent.Value.c_str(), selected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                }
                continue;
            }

            if (ImGui::BeginMenu(parent.Value.c_str())) {
                const bool parentOnlySelected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable("<parent only>", parentOnlySelected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                }
                ImGui::Separator();
                for (const auto& child : parent.Children) {
                    const bool selected = (parent.Id == parentId && child.Id == childId);
                    if (ImGui::Selectable(child.Value.c_str(), selected)) {
                        QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, child.Id)}, pendingEdits);
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndCombo();
    }
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
        const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
        if (previewClipped) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(preview.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

} // namespace

void TicketFieldEditor::RenderFieldCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                        const TrackerField* field, const std::string& currentValue, float availWidth,
                                        bool tooltipsEnabled, bool allowEdits, SpreadsheetState& state,
                                        std::vector<PendingFieldEdit>& pendingEdits,
                                        TrackerGridFieldAsyncState& trackerGridAsync,
                                        const std::string& dateFormatOption, int thresholdDays) {
    SMATCHET_UI_PERF_SCOPE("RenderFieldCell");
    const bool handledByLua = app.TryLuaFieldDisplay(column.FieldId, ticket, currentValue, availWidth, field);
    if (column.FieldId == "summary") {
        static int s_summaryCellLogBudget;
        if (s_summaryCellLogBudget < 50) {
            ++s_summaryCellLogBudget;
            AgentLogTicketField("A", "TicketFieldEditor:RenderFieldCell_summary", handledByLua ? 1 : 0,
                                static_cast<int>(column.Plan), 0, 0);
        }
    }
    if (handledByLua) {
        return;
    }

    if (SmatchetFieldIconRender::TryDrawFieldValueIcon(app, column.FieldId, field, currentValue, availWidth,
                                                       tooltipsEnabled, allowEdits)) {
        return;
    }

    auto renderPlainText = [&](bool disabled) {
        std::string display;
        if (column.IsDateLike) {
            display = DisplayValueForTrackerDateField(column.FieldId, field, currentValue,
                                                     dateFormatOption, thresholdDays);
        } else {
            display = app.ResolveDisplayValue(column.FieldId, field, currentValue);
        }
        const std::string* tip = column.IsDateLike ? &currentValue : nullptr;
        RenderClippedFieldText(display, availWidth, tooltipsEnabled, disabled, tip);
    };

    switch (column.Plan) {
    case TicketGridColumn::RenderPlan::SpecialAttachment:
        if (field == nullptr || IsAttachmentFieldId(field->Id)) {
            TrackerGridFieldDisplay::RenderAttachmentsField(app, currentValue, availWidth, tooltipsEnabled);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialWatchers:
        if (field == nullptr || TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWatchersField(app, ticket.id, currentValue, availWidth, tooltipsEnabled,
                                                      trackerGridAsync);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialVotes:
        if (field == nullptr || TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderVotesField(app, ticket.id, currentValue, availWidth, tooltipsEnabled, trackerGridAsync);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialWorklog:
        if (field == nullptr || TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWorklogField(currentValue, availWidth, tooltipsEnabled);
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialProgress:
        if (TrackerGridFieldDisplay::TryRenderProgressJsonField(currentValue, availWidth)) {
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::SpecialIssueRestriction:
        if (TrackerGridFieldDisplay::TryRenderIssueRestrictionField(currentValue, availWidth, tooltipsEnabled)) {
            return;
        }
        break;
    case TicketGridColumn::RenderPlan::PlainText:
        renderPlainText(column.CatalogReadOnly);
        return;
    case TicketGridColumn::RenderPlan::Labels:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        TrackerLabelsEditor::RenderLabelsFieldEditor(
            app, ticket, *field, currentValue,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits);
            });
        return;
    case TicketGridColumn::RenderPlan::Cascading:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderCascadingSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::MultiSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderMultiSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::SingleSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderSingleSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
        return;
    case TicketGridColumn::RenderPlan::DateTimeEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        TrackerDateTimeFieldEditor::RenderDateTimeFieldEditor(
            ticket, *field, currentValue, state,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits);
            },
            dateFormatOption, thresholdDays);
        return;
    case TicketGridColumn::RenderPlan::TextEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderTextEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled, availWidth);
        return;
    }
}







