#include "TicketFieldEditor.h"
#include "UiPerfMonitor.h"

#include "AppController.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "SmatchetFieldIconRender.h"
#include "SmatchetFieldRender.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldValueParser.h"
#include "TrackerFieldPayload.h"
#include "MarkdownConvert.h"
#include "JiraClient.h"
#include "CompactDateFormat.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <sstream>

namespace {

using namespace TrackerFieldValueUtils;

std::string GetCurrentJiraDateTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    std::tm tmUtc{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &tt);
    gmtime_s(&tmUtc, &tt);
#else
    localtime_r(&tt, &tmLocal);
    gmtime_r(&tt, &tmUtc);
#endif

    int localMin = tmLocal.tm_hour * 60 + tmLocal.tm_min;
    int utcMin = tmUtc.tm_hour * 60 + tmUtc.tm_min;

    if (tmLocal.tm_yday > tmUtc.tm_yday || (tmLocal.tm_yday == 0 && tmUtc.tm_yday > 300)) {
        localMin += 24 * 60;
    } else if (tmLocal.tm_yday < tmUtc.tm_yday || (tmUtc.tm_yday == 0 && tmLocal.tm_yday > 300)) {
        utcMin += 24 * 60;
    }
    const int offsetSec = (localMin - utcMin) * 60;

    ParsedJiraDateTime p;
    p.Year = tmLocal.tm_year + 1900;
    p.Month = tmLocal.tm_mon + 1;
    p.Day = tmLocal.tm_mday;
    p.Hour = tmLocal.tm_hour;
    p.Minute = tmLocal.tm_min;
    p.Second = tmLocal.tm_sec;
    p.HasWallTime = true;
    p.OffsetSec = offsetSec;
    p.HasTimeZoneSuffix = true;
    p.TimeZoneWasZ = (offsetSec == 0);

    return FormatJiraDateOrDateTimeForApi(false, p);
}

struct ActiveWorklogDialogState {
    std::string IssueId;
    char TimeSpent[64] = "";
    char TimeRemaining[64] = "";
    std::string DateStarted;
    char WorkDescription[1024] = "";
    std::string OriginalEstimate;
    std::string TotalTimeSpent;
    std::string TotalTimeRemaining;
    bool Initialized = false;
    std::string ErrorMsg;
    bool JustOpened = false;
    bool TimeRemainingManuallyEdited = false;
};

static ActiveWorklogDialogState s_ActiveWorklogState;

/// Source format of the original rich payload (`OriginalRichValue`). Determines which converter
/// seeds the Markdown buffer on open and which target format is expected by the payload layer.
enum class LongTextRichKind { None, Adf, Html };

/// Singleton state for the long-text / ADF field modal editor. Decoupled from `SpreadsheetState`
/// so the modal survives the originating cell scrolling out of view: the cell triggers
/// `JustOpened`, then the top-level `RenderLongTextModal` owns the lifecycle.
struct ActiveLongTextEditorState {
    std::string IssueId;
    TrackerField Field;
    std::string FieldLabel;
    /// Stripped display text of the field at the moment the modal opened — for "did the user actually
    /// change anything?" detection on save.
    std::string OriginalStrippedValue;
    /// The Markdown that initially seeded the buffer (after converting from `OriginalRichValue`).
    /// Used so save can detect "no change" against the actual editor surface, not the stripped text.
    std::string OriginalMarkdown;
    /// Original rich payload (ADF JSON or HTML) at modal-open time. Empty when the cache had no
    /// rich value (legacy ticket pre-PR-B). v2 PR-E will pass this to the offline-replay merge.
    std::string OriginalRichValue;
    LongTextRichKind RichKind = LongTextRichKind::None;
    /// True when HtmlSubsetToMarkdown tripped the fallback (unknown tags) — modal shows a banner
    /// and falls back to editing the raw HTML directly.
    bool RawMode = false;
    /// ADF node types that AdfToMarkdown skipped because they aren't representable in our
    /// Markdown subset (panels, mentions, smart links, ...). Surface as a soft warning.
    std::vector<std::string> DroppedAdfNodeTypes;

    /// Generously sized for descriptions; truncates beyond this. v2.1 candidate: resize callback.
    static constexpr size_t kBufferSize = 64 * 1024;
    std::vector<char> Buffer;
    bool Active = false;
    bool JustOpened = false;
};

static ActiveLongTextEditorState s_ActiveLongTextState;

static constexpr const char* kLongTextModalPopupId = "EditLongTextModal";

/// Determine whether a stored rich payload looks like ADF JSON or HTML. Returns LongTextRichKind::None
/// when the input is empty or unrecognizable (caller falls back to the stripped text).
static LongTextRichKind ClassifyRichValue(const std::string& rich) {
    if (rich.empty()) return LongTextRichKind::None;
    // Cheap leading-whitespace skip.
    size_t i = 0;
    while (i < rich.size() && (rich[i] == ' ' || rich[i] == '\t' || rich[i] == '\n' || rich[i] == '\r')) ++i;
    if (i >= rich.size()) return LongTextRichKind::None;
    if (rich[i] == '{') {
        try {
            auto parsed = nlohmann::json::parse(rich, nullptr, false);
            if (parsed.is_object() && parsed.value("type", std::string()) == "doc") {
                return LongTextRichKind::Adf;
            }
        } catch (...) {
            // fall through
        }
    }
    if (rich[i] == '<') {
        return LongTextRichKind::Html;
    }
    return LongTextRichKind::None;
}

static void OpenLongTextEditor(const std::string& issueId, const TrackerField& field,
                               const std::string& label, const std::string& currentStrippedValue,
                               const std::string& currentRichValue) {
    s_ActiveLongTextState.IssueId = issueId;
    s_ActiveLongTextState.Field = field;
    s_ActiveLongTextState.FieldLabel = label.empty() ? field.Id : label;
    s_ActiveLongTextState.OriginalStrippedValue = currentStrippedValue;
    s_ActiveLongTextState.OriginalRichValue = currentRichValue;
    s_ActiveLongTextState.RichKind = ClassifyRichValue(currentRichValue);
    s_ActiveLongTextState.RawMode = false;
    s_ActiveLongTextState.DroppedAdfNodeTypes.clear();

    /// Compute the Markdown seed from the rich value when present; otherwise fall back to the
    /// stripped text (legacy tickets pre-PR-B, or fields where the backend never returned rich).
    std::string seed;
    switch (s_ActiveLongTextState.RichKind) {
        case LongTextRichKind::Adf: {
            try {
                const auto adf = nlohmann::json::parse(currentRichValue);
                seed = MarkdownConvert::AdfToMarkdown(adf, &s_ActiveLongTextState.DroppedAdfNodeTypes);
            } catch (...) {
                seed = currentStrippedValue;
            }
            break;
        }
        case LongTextRichKind::Html: {
            bool fellBack = false;
            seed = MarkdownConvert::HtmlSubsetToMarkdown(currentRichValue, &fellBack);
            if (fellBack) {
                s_ActiveLongTextState.RawMode = true;
                seed = currentRichValue; // edit the raw HTML directly so nothing is lost.
            }
            break;
        }
        case LongTextRichKind::None:
            seed = currentStrippedValue;
            break;
    }
    s_ActiveLongTextState.OriginalMarkdown = seed;

    s_ActiveLongTextState.Buffer.assign(ActiveLongTextEditorState::kBufferSize, '\0');
    const size_t copyLen = (std::min)(seed.size(), ActiveLongTextEditorState::kBufferSize - 1);
    std::memcpy(s_ActiveLongTextState.Buffer.data(), seed.data(), copyLen);
    s_ActiveLongTextState.Buffer[copyLen] = '\0';
    s_ActiveLongTextState.Active = true;
    s_ActiveLongTextState.JustOpened = true;
}

static void CloseLongTextEditor() {
    s_ActiveLongTextState = ActiveLongTextEditorState{};
}

struct EditCbUser {
    SpreadsheetState* state;
};

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
    return 0;
}

struct DurationCallbackWrapperData {
    ImGuiInputTextCallback OriginalCallback;
    void* OriginalUserData;
    bool* NeedRepositionAndFocus;
};

static int DurationInputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* wrapper = static_cast<DurationCallbackWrapperData*>(data->UserData);
    if (!wrapper) return 0;

    if (wrapper->NeedRepositionAndFocus && *(wrapper->NeedRepositionAndFocus)) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
            data->CursorPos = data->BufTextLen;
            data->SelectionStart = data->BufTextLen;
            data->SelectionEnd = data->BufTextLen;
            *(wrapper->NeedRepositionAndFocus) = false;
        }
    }

    if (wrapper->OriginalCallback) {
        data->UserData = wrapper->OriginalUserData;
        int res = wrapper->OriginalCallback(data);
        data->UserData = wrapper;
        return res;
    }
    return 0;
}

bool DrawDurationFieldWithSuggestions(const char* label, char* buf, size_t bufSize,
                                      ImGuiInputTextFlags flags = 0,
                                      ImGuiInputTextCallback callback = nullptr,
                                      void* callbackUserData = nullptr,
                                      bool* outManuallyEdited = nullptr,
                                      bool forceOpenPopup = false) {
    bool submitted = false;
    ImGui::PushID(label);

    // Resolve per-widget state using ImGuiStorage
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID needFocusKey = ImGui::GetID("##needRepositionAndFocus");
    ImGuiID lastActiveIdKey = ImGui::GetID("##lastActiveId");

    bool needRepositionAndFocus = storage->GetInt(needFocusKey, 0) != 0;
    ImGuiID lastActiveId = static_cast<ImGuiID>(storage->GetInt(lastActiveIdKey, 0));
    ImGuiID selectedFromPopupKey = ImGui::GetID("##valueSelectedFromPopup");
    bool valueSelectedFromPopup = storage->GetInt(selectedFromPopupKey, 0) != 0;

    float totalWidth = ImGui::GetContentRegionAvail().x;
    float inputWidth = totalWidth - 26.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::SetNextItemWidth(inputWidth);

    if (forceOpenPopup) {
        ImGui::OpenPopup("duration_suggestions");
    }

    if (needRepositionAndFocus) {
        ImGui::SetKeyboardFocusHere();
    }

    DurationCallbackWrapperData wrapperData;
    wrapperData.OriginalCallback = callback;
    wrapperData.OriginalUserData = callbackUserData;
    wrapperData.NeedRepositionAndFocus = &needRepositionAndFocus;

    bool deactivated = false;
    if (ImGui::InputText("##duration_input", buf, bufSize, flags | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways, DurationInputTextCallback, &wrapperData)) {
        submitted = true;
        if (outManuallyEdited) {
            *outManuallyEdited = true;
        }
    }
    deactivated = ImGui::IsItemDeactivatedAfterEdit();
    if (!deactivated && ImGui::IsItemDeactivated() && valueSelectedFromPopup) {
        deactivated = true;
        valueSelectedFromPopup = false;
    }

    ImGuiID currentId = ImGui::GetID("##duration_input");

    bool shouldOpen = false;
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        shouldOpen = true;
    }
    if (ImGui::IsItemActive()) {
        if (lastActiveId != currentId) {
            lastActiveId = currentId;
            if (buf[0] == '\0') {
                shouldOpen = true;
            }
        }
    } else {
        if (lastActiveId == currentId) {
            lastActiveId = 0;
        }
    }
    if (shouldOpen) {
        ImGui::OpenPopup("duration_suggestions");
    }

    ImGui::SameLine();
    bool buttonClicked = ImGui::Button("▼", ImVec2(24.0f, 0.0f));
    bool buttonHovered = ImGui::IsItemHovered();
    if (buttonClicked) {
        ImGui::OpenPopup("duration_suggestions");
    }
    ImGui::PopStyleVar();

    bool popupIsOpen = ImGui::IsPopupOpen("duration_suggestions");

    // Suggestions popup
    if (ImGui::BeginPopup("duration_suggestions")) {
        std::vector<std::string> suggestions = LoadDurationSuggestions();

        for (size_t i = 0; i < suggestions.size(); ++i) {
            const auto& item = suggestions[i];
            if (ImGui::Selectable(item.c_str())) {
                std::strncpy(buf, item.c_str(), bufSize - 1);
                buf[bufSize - 1] = '\0';
                if (outManuallyEdited) {
                    *outManuallyEdited = true;
                }
                needRepositionAndFocus = true;
                valueSelectedFromPopup = true;
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // Resolve per-widget state using ImGuiStorage
    ImGuiID popupOpenKey = ImGui::GetID("##popupWasOpen");
    bool popupWasOpen = storage->GetInt(popupOpenKey, 0) != 0;

    bool popupJustClosed = popupWasOpen && !popupIsOpen;

    bool finalDeactivated = false;
    if (deactivated) {
        if (!buttonHovered && !popupIsOpen) {
            finalDeactivated = true;
        }
    }
    if (popupJustClosed && !needRepositionAndFocus) {
        finalDeactivated = true;
    }

    storage->SetInt(popupOpenKey, popupIsOpen ? 1 : 0);
    storage->SetInt(selectedFromPopupKey, valueSelectedFromPopup ? 1 : 0);
    // Store updated state back to ImGuiStorage
    storage->SetInt(needFocusKey, needRepositionAndFocus ? 1 : 0);
    storage->SetInt(lastActiveIdKey, static_cast<int>(lastActiveId));

    ImGui::PopID();
    return submitted || finalDeactivated;
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

    // ADF / long-text fields (Jira description/environment, Plane description, custom textarea/wiki-renderer
    // fields) are too constrained by the inline 512-byte single-line InputText. Route them through the modal
    // editor singleton: the cell stays in display mode, the modal owns the lifecycle. v1: plain-text round-trip.
    // v2 (see RICH_TEXT_EDITING_V2_PLAN.md) layers Markdown <-> ADF/HTML fidelity on top.
    if (TrackerFieldPayload::FieldUsesAdfDocument(field)) {
        if (state.IsEditingField(ticket.id, field.Id)) {
            if (state.EditJustStarted) {
                const std::string& label = !field.Name.empty() ? field.Name : field.Id;
                const std::string richValue = ticket.GetFieldRichValue(field.Id);
                OpenLongTextEditor(ticket.id, field, label, currentValue, richValue);
                ImGui::OpenPopup(kLongTextModalPopupId);
            }
            // Hand the lifecycle to the modal singleton; the cell falls through to the read-only preview below.
            state.ClearEditing();
        }
        // fall through to the display branch
    } else if (state.IsEditingField(ticket.id, field.Id)) {
        const bool editJustStarted = state.EditJustStarted;
        const bool isDuration = IsTimeDurationField(field.Id);
        bool submitted = false;

        if (isDuration) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            EditCbUser cbUser{&state};
            submitted = DrawDurationFieldWithSuggestions(
                itemId.c_str(), state.EditBuffer, sizeof(state.EditBuffer),
                ImGuiInputTextFlags_CallbackAlways,
                InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser),
                nullptr, editJustStarted
            );
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            EditCbUser cbUser{&state};
            submitted = ImGui::InputText(itemId.c_str(), state.EditBuffer, sizeof(state.EditBuffer),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                         InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser));
        }

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
    const bool hasNewlineInValue = std::any_of(valueForDisplay.begin(), valueForDisplay.end(),
                                               [](char c) { return c == '\n' || c == '\r'; });
    std::string singleLine = valueForDisplay;
    auto nlIt = std::find_if(singleLine.begin(), singleLine.end(), [](char c) { return c == '\n' || c == '\r'; });
    if (nlIt != singleLine.end()) {
        singleLine.erase(nlIt, singleLine.end());
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
        if (disabled && display.empty()) {
            display = "-";
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
    case TicketGridColumn::RenderPlan::SpecialTimeSpent: {
        std::string buttonText = currentValue.empty() ? "Log work" : currentValue;
        std::string buttonId = "##TimeSpentBtn_" + ticket.id;

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0)); // invisible background when normal
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

        if (ImGui::Button((buttonText + buttonId).c_str(), ImVec2(availWidth > 0.0f ? availWidth : -FLT_MIN, 0.0f))) {
            s_ActiveWorklogState.IssueId = ticket.id;
            s_ActiveWorklogState.TimeSpent[0] = '\0';
            s_ActiveWorklogState.OriginalEstimate = ticket.GetFieldValue("timeoriginalestimate");
            s_ActiveWorklogState.TotalTimeSpent = ticket.GetFieldValue("timespent");
            s_ActiveWorklogState.TotalTimeRemaining = ticket.GetFieldValue("timeestimate");
            std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
            s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';

            s_ActiveWorklogState.DateStarted = GetCurrentJiraDateTimeString();

            s_ActiveWorklogState.WorkDescription[0] = '\0';
            s_ActiveWorklogState.ErrorMsg.clear();
            s_ActiveWorklogState.Initialized = true;
            s_ActiveWorklogState.JustOpened = true;
            s_ActiveWorklogState.TimeRemainingManuallyEdited = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (currentValue.empty()) {
                ImGui::TextUnformatted("No work logged yet. Click to log work.");
            } else {
                ImGui::Text("Total Time Spent: %s\nClick to log work / edit estimates.", currentValue.c_str());
            }
            ImGui::EndTooltip();
        }
        break;
    }
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

    if (s_ActiveWorklogState.Initialized && s_ActiveWorklogState.IssueId == ticket.id) {
        ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_Always);
        if (s_ActiveWorklogState.JustOpened) {
            ImGui::OpenPopup("TimeTrackingPopup");
            s_ActiveWorklogState.JustOpened = false;
        }

        if (ImGui::BeginPopupModal("TimeTrackingPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Time tracking: %s", ticket.id.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            // Logged & Remaining progress bar
            long long spentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeSpent);
            long long remSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
            long long newSpentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);

            long long displaySpentSec = spentSec + newSpentSec;
            long long displayRemSec = remSec;
            if (newSpentSec > 0 && !s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                displayRemSec = (std::max)(0LL, remSec - newSpentSec);
            } else if (s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                displayRemSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeRemaining);
            }

            long long totalSec = displaySpentSec + displayRemSec;
            float fraction = 0.0f;
            if (totalSec > 0) {
                fraction = (float)displaySpentSec / (float)totalSec;
            }

            std::string loggedLabel = FormatWorkDurationFromSeconds(displaySpentSec);
            if (loggedLabel.empty()) loggedLabel = "0m";
            loggedLabel += " logged";

            std::string remainingLabel = FormatWorkDurationFromSeconds(displayRemSec);
            if (remainingLabel.empty()) remainingLabel = "0m";
            remainingLabel += " remaining";

            ImGui::TextUnformatted(loggedLabel.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(remainingLabel.c_str()).x);
            ImGui::TextUnformatted(remainingLabel.c_str());

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.12f, 0.45f, 0.88f, 1.00f)); // Jira blue
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 14.0f), "");
            ImGui::PopStyleColor();

            if (!s_ActiveWorklogState.OriginalEstimate.empty()) {
                ImGui::TextDisabled("The original estimate for this work item was %s.", s_ActiveWorklogState.OriginalEstimate.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Inputs
            ImGui::Text("Time spent *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeSpent", s_ActiveWorklogState.TimeSpent, sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr, nullptr, nullptr, false)) {
                if (!s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                    long long spentVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
                    long long remVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
                    if (spentVal > 0) {
                        long long newRem = (std::max)(0LL, remVal - spentVal);
                        std::string formattedRem = FormatWorkDurationFromSeconds(newRem);
                        std::strncpy(s_ActiveWorklogState.TimeRemaining, formattedRem.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    } else {
                        std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(), sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    }
                }
            }
            ImGui::TextDisabled("Use the format: 2w 4d 6h 45m (w=weeks, d=days, h=hours, m=minutes)");

            ImGui::Spacing();
            ImGui::Text("Time remaining");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeRemaining", s_ActiveWorklogState.TimeRemaining, sizeof(s_ActiveWorklogState.TimeRemaining), 0, nullptr, nullptr, &s_ActiveWorklogState.TimeRemainingManuallyEdited, false)) {
                // value changed!
            }

            ImGui::Spacing();
            ImGui::Text("Date started *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            TrackerDateTimeFieldEditor::RenderGenericDatePicker("##WorklogDateStarted", s_ActiveWorklogState.DateStarted, true);

            ImGui::Spacing();
            ImGui::Text("Work description");
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
            if (ImGui::Button("Templates ▼", ImVec2(90.0f, 0.0f))) {
                ImGui::OpenPopup("comment_templates_popup");
            }
            if (ImGui::BeginPopup("comment_templates_popup")) {
                std::vector<std::string> templates = LoadCommentTemplates();
                if (templates.empty()) {
                    ImGui::TextDisabled("No templates configured in Preferences.");
                } else {
                    for (const auto& item : templates) {
                        if (ImGui::Selectable(item.c_str())) {
                            std::strncpy(s_ActiveWorklogState.WorkDescription, item.c_str(), sizeof(s_ActiveWorklogState.WorkDescription) - 1);
                            s_ActiveWorklogState.WorkDescription[sizeof(s_ActiveWorklogState.WorkDescription) - 1] = '\0';
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::InputTextMultiline("##WorklogDesc", s_ActiveWorklogState.WorkDescription, sizeof(s_ActiveWorklogState.WorkDescription), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));

            if (!s_ActiveWorklogState.ErrorMsg.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_ActiveWorklogState.ErrorMsg.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Buttons
            if (ImGui::Button("Save", ImVec2(80, 0))) {
                s_ActiveWorklogState.ErrorMsg.clear();
                long long sVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
                if (sVal <= 0) {
                    s_ActiveWorklogState.ErrorMsg = "Invalid Time spent format. Please use e.g. 2h 30m.";
                } else {
                    ParsedJiraDateTime dummyDt;
                    if (!TryParseJiraDateTime(s_ActiveWorklogState.DateStarted, dummyDt)) {
                        s_ActiveWorklogState.ErrorMsg = "Invalid Date started format.";
                    } else {
                        std::string adjEst = s_ActiveWorklogState.TimeRemainingManuallyEdited ? "new" : "auto";
                        std::string outErr;
                        if (app.SubmitWorklog(s_ActiveWorklogState.IssueId,
                                              s_ActiveWorklogState.TimeSpent,
                                              s_ActiveWorklogState.TimeRemaining,
                                              adjEst,
                                              s_ActiveWorklogState.WorkDescription,
                                              s_ActiveWorklogState.DateStarted,
                                              outErr)) {
                            ImGui::CloseCurrentPopup();
                            s_ActiveWorklogState.Initialized = false;
                        } else {
                            s_ActiveWorklogState.ErrorMsg = "Failed: " + outErr;
                        }
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                ImGui::CloseCurrentPopup();
                s_ActiveWorklogState.Initialized = false;
            }

            ImGui::EndPopup();
        } else {
            s_ActiveWorklogState.Initialized = false;
        }
    }
}

void TicketFieldEditor::RenderLongTextModal(std::vector<PendingFieldEdit>& pendingEdits) {
    if (!s_ActiveLongTextState.Active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (s_ActiveLongTextState.JustOpened) {
        const ImVec2 modalSize(viewport->Size.x * 0.6f, viewport->Size.y * 0.65f);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    }

    if (ImGui::BeginPopupModal(kLongTextModalPopupId, nullptr,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Edit %s — %s", s_ActiveLongTextState.FieldLabel.c_str(),
                    s_ActiveLongTextState.IssueId.c_str());

        // Format-fidelity banners. RawMode is the strongest signal — fall back to editing
        // the source HTML directly so we don't destroy unrecognized markup. DroppedAdfNodeTypes
        // is informational: the rendered Markdown is missing some original constructs but the
        // user can still edit and save the rest cleanly.
        if (s_ActiveLongTextState.RawMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextWrapped("Editing raw HTML — the source contains tags this build doesn't yet "
                               "translate to Markdown. Save will store your edits verbatim.");
            ImGui::PopStyleColor();
        } else if (!s_ActiveLongTextState.DroppedAdfNodeTypes.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
            std::string list;
            for (size_t i = 0; i < s_ActiveLongTextState.DroppedAdfNodeTypes.size() && i < 5; ++i) {
                if (!list.empty()) list += ", ";
                list += s_ActiveLongTextState.DroppedAdfNodeTypes[i];
            }
            ImGui::TextWrapped("Note: this document contains constructs not in our Markdown subset (%s) — "
                               "saving will keep what you edit but those nodes are not shown.",
                               list.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // Reserve room for the footer (status line + button row).
        const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 inputSize(-FLT_MIN, ImGui::GetContentRegionAvail().y - footerH);

        if (s_ActiveLongTextState.JustOpened) {
            ImGui::SetKeyboardFocusHere();
            s_ActiveLongTextState.JustOpened = false;
        }

        ImGui::InputTextMultiline("##LongTextEditorBuf",
                                  s_ActiveLongTextState.Buffer.data(),
                                  s_ActiveLongTextState.Buffer.size(),
                                  inputSize,
                                  ImGuiInputTextFlags_AllowTabInput);

        // Ctrl+Enter saves; Esc cancels. Both work even when the textarea is focused.
        const bool ctrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        const bool ctrlEnter = ctrlDown && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        const bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

        const char* footerHint = s_ActiveLongTextState.RawMode
            ? "Ctrl+Enter to save · Esc to cancel · raw HTML mode (Markdown disabled)"
            : "Ctrl+Enter to save · Esc to cancel · Markdown — **bold**, *em*, # heading, - list, ```code```";
        ImGui::TextDisabled("%s", footerHint);

        bool save = ctrlEnter;
        bool cancel = escPressed;
        if (ImGui::Button("Save", ImVec2(100, 0))) {
            save = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            cancel = true;
        }

        if (save) {
            const std::string newValue(s_ActiveLongTextState.Buffer.data());
            // Diff against the seeded markdown (or the raw HTML in RawMode) so we don't queue a
            // null edit that would still re-emit through the payload converter and reshape
            // formatting that hasn't actually changed.
            const std::string& seed = s_ActiveLongTextState.RawMode
                                          ? s_ActiveLongTextState.OriginalRichValue
                                          : s_ActiveLongTextState.OriginalMarkdown;
            if (newValue != seed) {
                PendingFieldEdit edit;
                edit.IssueId = s_ActiveLongTextState.IssueId;
                edit.Field = s_ActiveLongTextState.Field;
                edit.Values = {newValue};
                edit.Preformatted = s_ActiveLongTextState.RawMode;
                pendingEdits.push_back(std::move(edit));
            }
            ImGui::CloseCurrentPopup();
            CloseLongTextEditor();
        } else if (cancel) {
            ImGui::CloseCurrentPopup();
            CloseLongTextEditor();
        }

        ImGui::EndPopup();
    } else {
        // Popup was dismissed without our intervention (shouldn't normally happen for a modal,
        // but stay self-consistent if it does).
        CloseLongTextEditor();
    }
}



