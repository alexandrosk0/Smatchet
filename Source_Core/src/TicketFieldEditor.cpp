#include "TicketFieldEditor.h"
#include "TicketFieldEditor_detail.h"

#include "AppController.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerLabelsEditor.h"
#include "CppSyntaxHighlight.h"
#include "SmatchetFieldIconRender.h"
#include "SmatchetFieldRender.h"
#include "SmatchetThemedTextEditorPalette.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerFieldValueParser.h"
#include "TrackerFieldPayload.h"
#include "DictationInsertionRouter.h"
#include "MarkdownConvert.h"
#include "MarkdownPreviewRender.h"
#include "TicketFieldEditorLongTextPure.h"
#include "TicketFieldEditorDescriptionPure.h"
#include "TextEditor.h"
#include "Logger.h"
#include "JiraClient.h"
#include "CompactDateFormat.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
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
    const bool fullRange = data->BufTextLen > 0 && data->SelectionStart == 0 && data->SelectionEnd == data->BufTextLen;
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
    if (!wrapper)
        return 0;

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

bool DrawDurationFieldWithSuggestions(const char* label, char* buf, size_t bufSize, ImGuiInputTextFlags flags = 0,
                                      ImGuiInputTextCallback callback = nullptr, void* callbackUserData = nullptr,
                                      bool* outManuallyEdited = nullptr, bool forceOpenPopup = false) {
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
    if (ImGui::InputText("##duration_input", buf, bufSize,
                         flags | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                         DurationInputTextCallback, &wrapperData)) {
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
                      const std::string& currentValue, SpreadsheetState& state,
                      std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled, float availWidth,
                      bool singleClickToEdit) {
    // Widget-cell unique ID is provided by the CellIdScope pushed in RenderFieldCell
    // (ticket.id + field.Id on the ImGui ID stack). Literal short labels below stay collision-free.

    // ADF / long-text fields (Jira description/environment, Plane description, custom textarea/wiki-renderer
    // fields) are too constrained by the inline 512-byte single-line InputText. Route them through the modal
    // editor singleton: the cell stays in display mode, the modal owns the lifecycle. v1: plain-text round-trip.
    // v2 (see RICH_TEXT_EDITING_V2_PLAN.md) layers Markdown <-> ADF/HTML fidelity on top.
    if (TrackerFieldPayload::FieldUsesAdfDocument(field)) {
        if (state.IsEditingField(ticket.id, field.Id)) {
            if (state.EditJustStarted) {
                const std::string& label = !field.Name.empty() ? field.Name : field.Id;
                const std::string richValue = ticket.GetFieldRichValue(field.Id);
                OpenLongTextEditor(app, ticket.id, field, label, currentValue, richValue);
                // Do NOT call ImGui::OpenPopup here — we are inside a table cell.
                // ImGui requires OpenPopup and BeginPopupModal at the same window depth.
                // RenderLongTextModal (called after EndTable) sees JustOpened=true and
                // calls OpenPopup from the stable top-level location.
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
                "##textedit_duration", state.EditBuffer, sizeof(state.EditBuffer), ImGuiInputTextFlags_CallbackAlways,
                InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser), nullptr, editJustStarted);
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            EditCbUser cbUser{&state};
            submitted = ImGui::InputText("##textedit", state.EditBuffer, sizeof(state.EditBuffer),
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
    const bool hasNewlineInValue =
        std::any_of(valueForDisplay.begin(), valueForDisplay.end(), [](char c) { return c == '\n' || c == '\r'; });
    std::string singleLine = valueForDisplay;
    auto nlIt = std::find_if(singleLine.begin(), singleLine.end(), [](char c) { return c == '\n' || c == '\r'; });
    if (nlIt != singleLine.end()) {
        singleLine.erase(nlIt, singleLine.end());
    }
    const std::string& display = singleLine;
    const float regionAvail = (availWidth > 0.0f) ? availWidth : ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable(display.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (singleClickToEdit || ImGui::IsMouseDoubleClicked(0)) {
            state.StartEditingField(ticket.id, field.Id, currentValue);
        }
    }
    const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    const bool horizontallyClipped = (regionAvail > 0.0f && textSize.x > regionAvail + 1.0f);
    // GitHub body + Jira description carry markdown — render preview tooltip on
    // hover even when the cell fits in one line (no clip / no newline). Other
    // text fields keep the clip-gate so plain values don't tooltip noisily.
    const bool isDescriptionLike = IsDescriptionLikeFieldId(field.Id);
    if (tooltipsEnabled && (hasNewlineInValue || horizontallyClipped || isDescriptionLike) && ImGui::IsItemHovered()) {
        const std::string* rawTip = IsTrackerDateOrDateTimeField(field.Id, &field) ? &currentValue : nullptr;
        // Convert ADF rich value → markdown so paragraph breaks (\n\n) render as
        // separate paragraphs. Plain text currentValue uses single \n (soft break in
        // markdown) which would join paragraphs into one line.
        std::string descMd;
        if (isDescriptionLike) {
            const std::string richVal = ticket.GetFieldRichValue(field.Id);
            if (!richVal.empty()) {
                try {
                    descMd = MarkdownConvert::AdfToMarkdown(nlohmann::json::parse(richVal));
                } catch (const std::exception& e) {
                    LOG_DEBUG("ADF tooltip convert failed for field '%s': %s", field.Id.c_str(), e.what());
                } catch (...) {
                    LOG_DEBUG("ADF tooltip convert failed for field '%s': unknown error", field.Id.c_str());
                }
            }
            if (descMd.empty()) {
                descMd = currentValue;
            }
        }
        const std::string& tipSource =
            isDescriptionLike ? descMd : ((rawTip && !rawTip->empty()) ? *rawTip : valueForDisplay);
        if (!tipSource.empty()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            if (isDescriptionLike) {
                MarkdownPreviewRender::Options opts;
                opts.mode = MarkdownPreviewRender::Mode::Tooltip;
                opts.clickableLinks = false;
                opts.wrapWidth = ImGui::GetFontSize() * 48.0f;
                MarkdownPreviewRender::Render(tipSource, opts);
            } else {
                ImGui::TextUnformatted(tipSource.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
}

void RenderSingleSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                              const std::string& currentValue, SpreadsheetState& state,
                              std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled,
                              bool singleClickToEdit) {
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    SmatchetLoadedIconTexture overlayIcon{};
    std::string overlayLoadErr;
    const bool haveOverlayIcon =
        SmatchetFieldIconRender::TryGetInlineFieldIconTexture(app, field, currentValue, overlayIcon, overlayLoadErr);
    (void)overlayLoadErr;

    // Hot path: ResolveOptionId + ResolveDisplayValue both parse / linear-scan against the raw JSON
    // value blob. They're only needed when the combo opens (currentId for the selected-row check)
    // or when the visible preview text actually shows (haveOverlayIcon = false). For a 100-row
    // priority column they fired ~100×/frame and the result was discarded — see PR #41.
    std::string preview;
    const char* previewCStr;
    if (haveOverlayIcon) {
        previewCStr = " ";
    } else {
        preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
        previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
    }
    // Combo cells always render as a flat Selectable preview (no BeginCombo's blue framed
    // background). Click threshold depends on `singleClickToEdit`: single-click mode arms on
    // any click; double-click mode arms only on double-click. Once armed, the next frame
    // force-opens the combo popup via OpenPopupEx + IsPopupOpen probe; on dismiss the arm
    // clears and the cell falls back to the Selectable preview.
    const std::string editorKey = ticket.id + "::" + field.Id;
    bool armed = (state.EditArmedKey == editorKey);
    if (armed) {
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, ImGui::GetID("##singleselect"));
        if (state.EditArmedJustOpened) {
            ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
            state.EditArmedJustOpened = false;
        } else if (!ImGui::IsPopupOpen(popupId, 0)) {
            // User dismissed the combo on a prior frame; release arm.
            state.EditArmedKey.clear();
            armed = false;
        }
    }
    if (!armed) {
        const ImVec2 selSize(cellAvail > 0.0f ? cellAvail : 0.0f, 0.0f);
        if (ImGui::Selectable(previewCStr, false, ImGuiSelectableFlags_AllowDoubleClick, selSize)) {
            if (singleClickToEdit || ImGui::IsMouseDoubleClicked(0)) {
                state.EditArmedKey = editorKey;
                state.EditArmedJustOpened = true;
            }
        }
        const ImVec2 selMin = ImGui::GetItemRectMin();
        const ImVec2 selMax = ImGui::GetItemRectMax();
        if (haveOverlayIcon && overlayIcon.Texture != nullptr && overlayIcon.Width > 0 && overlayIcon.Height > 0) {
            const float maxEdge = ImGui::GetFrameHeight();
            const float iw = static_cast<float>(overlayIcon.Width);
            const float ih = static_cast<float>(overlayIcon.Height);
            const float scale = maxEdge / (std::max)(iw, ih);
            const float dw = iw * scale;
            const float dh = ih * scale;
            const float rowH = selMax.y - selMin.y;
            const ImVec2 overlayP0(selMin.x + 4.0f, selMin.y + ((rowH - dh) * 0.5f));
            const ImVec2 overlayP1(overlayP0.x + dw, overlayP0.y + dh);
            ImGui::GetWindowDrawList()->AddImage(overlayIcon.Texture->GetTexRef(), overlayP0, overlayP1,
                                                 ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            if (haveOverlayIcon && preview.empty()) {
                preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
                previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
            }
            const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
            const bool previewClipped = (cellAvail > 0.0f && psz.x > cellAvail + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(previewCStr);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        return;
    }
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(cellAvail);
    const bool comboOpened = ImGui::BeginCombo("##singleselect", previewCStr, ImGuiComboFlags_NoArrowButton);
    const ImVec2 comboMin = ImGui::GetItemRectMin();
    const ImVec2 comboMax = ImGui::GetItemRectMax();
    if (comboOpened) {
        const bool justOpened = (state.SingleSelectActiveKey != editorKey);
        if (justOpened) {
            state.SingleSelectActiveKey = editorKey;
            state.SingleSelectSearchBuf[0] = '\0';
        }

        const std::string currentId = ResolveOptionId(field, currentValue);
        const bool selectedNone = currentId.empty();
        if (ImGui::Selectable("<clear>", selectedNone)) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();

        // Quick-filter input — auto-focused on combo open so the user can start typing
        // immediately. Enter on a single match commits that option and closes the combo.
        if (justOpened) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool submitOnEnter =
            ImGui::InputTextWithHint("##SingleSelectSearch", "Filter options", state.SingleSelectSearchBuf,
                                     sizeof(state.SingleSelectSearchBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();

        const std::string filterLower = ToLowerAsciiCopy(TrimCopy(state.SingleSelectSearchBuf));
        const TrackerFieldOption* firstMatch = nullptr;
        int matchCount = 0;
        bool drewAny = false;
        for (const auto& option : field.AllowedValueOptions) {
            const std::string optionId = option.Id.empty() ? option.Value : option.Id;
            const bool matchesFilter = filterLower.empty() ||
                                       ToLowerAsciiCopy(option.Value).find(filterLower) != std::string::npos ||
                                       ToLowerAsciiCopy(option.SecondaryValue).find(filterLower) != std::string::npos ||
                                       ToLowerAsciiCopy(optionId).find(filterLower) != std::string::npos;
            if (!matchesFilter) {
                continue;
            }
            if (firstMatch == nullptr) {
                firstMatch = &option;
            }
            ++matchCount;
            drewAny = true;
            const bool isSelected = (option.Id == currentId);
            ImGui::PushID(optionId.c_str());
            if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
                QueueEdit(ticket.id, field, {option.Id}, pendingEdits);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        if (!drewAny) {
            ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
        }
        // Enter commits when filter narrows to a single match; if multiple match, commit the
        // top match (least-surprise: matches typeahead behaviour in most pickers).
        if (submitOnEnter && firstMatch != nullptr) {
            QueueEdit(ticket.id, field, {firstMatch->Id}, pendingEdits);
            ImGui::CloseCurrentPopup();
        }
        (void)matchCount;
        ImGui::EndCombo();
    }

    if (haveOverlayIcon && overlayIcon.Texture != nullptr && overlayIcon.Width > 0 && overlayIcon.Height > 0) {
        const float maxEdge = ImGui::GetFrameHeight();
        const float iw = static_cast<float>(overlayIcon.Width);
        const float ih = static_cast<float>(overlayIcon.Height);
        const float scale = maxEdge / (std::max)(iw, ih);
        const float dw = iw * scale;
        const float dh = ih * scale;
        const float rowH = comboMax.y - comboMin.y;
        const ImVec2 overlayP0(comboMin.x + 4.0f, comboMin.y + ((rowH - dh) * 0.5f));
        const ImVec2 overlayP1(overlayP0.x + dw, overlayP0.y + dh);
        ImGui::GetWindowDrawList()->AddImage(overlayIcon.Texture->GetTexRef(), overlayP0, overlayP1, ImVec2(0.0f, 0.0f),
                                             ImVec2(1.0f, 1.0f));
    }
    // Tooltip only fires when the combo is actually hovered — defer the clip-test text measurement
    // (and resolve the preview lazily for the icon-overlay branch where preview was skipped).
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        if (haveOverlayIcon && preview.empty()) {
            preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
            previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
        }
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

void RenderMultiSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, SpreadsheetState& state,
                             std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled,
                             bool singleClickToEdit) {
    std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(field, currentValue);
    std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    // Arm-then-popup: see RenderSingleSelectEditor. Always Selectable preview; click threshold
    // gated by singleClickToEdit (any-click vs double-click).
    const std::string editorKey = ticket.id + "::" + field.Id;
    bool armed = (state.EditArmedKey == editorKey);
    if (armed) {
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, ImGui::GetID("##multiselect"));
        if (state.EditArmedJustOpened) {
            ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
            state.EditArmedJustOpened = false;
        } else if (!ImGui::IsPopupOpen(popupId, 0)) {
            state.EditArmedKey.clear();
            armed = false;
        }
    }
    if (!armed) {
        const char* previewCStr = preview.empty() ? "" : preview.c_str();
        const ImVec2 selSize(cellAvail > 0.0f ? cellAvail : 0.0f, 0.0f);
        if (ImGui::Selectable(previewCStr, false, ImGuiSelectableFlags_AllowDoubleClick, selSize)) {
            if (singleClickToEdit || ImGui::IsMouseDoubleClicked(0)) {
                state.EditArmedKey = editorKey;
                state.EditArmedJustOpened = true;
            }
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
            const bool previewClipped = (cellAvail > 0.0f && psz.x > cellAvail + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(previewCStr);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        return;
    }
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##multiselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (state.MultiSelectActiveKey != editorKey) {
            state.MultiSelectActiveKey = editorKey;
            state.MultiSelectSearchBuf[0] = '\0';
        }

        if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits);
        }
        ImGui::Separator();

        const std::string searchHint = field.Id == "components" ? "Search components" : "Search options";
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##MultiSelectSearch", searchHint.c_str(), state.MultiSelectSearchBuf,
                                 sizeof(state.MultiSelectSearchBuf));
        ImGui::Separator();

        const std::string filterLower = ToLowerAsciiCopy(TrimCopy(state.MultiSelectSearchBuf));
        bool drewAny = false;
        for (const auto& option : field.AllowedValueOptions) {
            const std::string optionId = option.Id.empty() ? option.Value : option.Id;
            bool checked = (selectedSet.find(optionId) != selectedSet.end());
            const bool matchesFilter = filterLower.empty() ||
                                       ToLowerAsciiCopy(option.Value).find(filterLower) != std::string::npos ||
                                       ToLowerAsciiCopy(option.SecondaryValue).find(filterLower) != std::string::npos ||
                                       ToLowerAsciiCopy(optionId).find(filterLower) != std::string::npos;
            if (!checked && !matchesFilter) {
                continue;
            }
            drewAny = true;
            // Per-option PushID disambiguates checkboxes whose visible labels could collide
            // (e.g. duplicate option.Value across customfields); pop matches at the end of body.
            ImGui::PushID(optionId.c_str());
            if (option.Disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Checkbox(option.Value.c_str(), &checked)) {
                if (checked) {
                    selectedSet.insert(optionId);
                } else {
                    selectedSet.erase(optionId);
                }
                std::vector<std::string> updated(selectedSet.begin(), selectedSet.end());
                std::sort(updated.begin(), updated.end());
                QueueEdit(ticket.id, field, updated, pendingEdits);
            }
            if (option.Disabled) {
                ImGui::EndDisabled();
            }
            if (!option.SecondaryValue.empty() && ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(option.SecondaryValue.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        if (!drewAny) {
            ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
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

void RenderCascadingSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                                 const std::string& currentValue, std::vector<PendingFieldEdit>& pendingEdits,
                                 bool tooltipsEnabled, SpreadsheetState& state, bool singleClickToEdit) {
    std::string parentId;
    std::string childId;
    TryResolveCascadingSelection(field, currentValue, parentId, childId);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);

    const float cellAvail = ImGui::GetContentRegionAvail().x;
    // Arm-then-popup: see RenderSingleSelectEditor.
    const std::string editorKey = ticket.id + "::" + field.Id;
    bool armed = (state.EditArmedKey == editorKey);
    if (armed) {
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, ImGui::GetID("##cascadeselect"));
        if (state.EditArmedJustOpened) {
            ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
            state.EditArmedJustOpened = false;
        } else if (!ImGui::IsPopupOpen(popupId, 0)) {
            state.EditArmedKey.clear();
            armed = false;
        }
    }
    if (!armed) {
        const char* previewCStr = preview.empty() ? "" : preview.c_str();
        const ImVec2 selSize(cellAvail > 0.0f ? cellAvail : 0.0f, 0.0f);
        if (ImGui::Selectable(previewCStr, false, ImGuiSelectableFlags_AllowDoubleClick, selSize)) {
            if (singleClickToEdit || ImGui::IsMouseDoubleClicked(0)) {
                state.EditArmedKey = editorKey;
                state.EditArmedJustOpened = true;
            }
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
            const bool previewClipped = (cellAvail > 0.0f && psz.x > cellAvail + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(previewCStr);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        return;
    }
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##cascadeselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
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

namespace {
/// RAII helper that pushes ticket.id + field-id onto the ImGui ID stack at cell entry, popping on
/// scope exit. Every widget inside RenderFieldCell (and the editors it dispatches to) inherits a
/// cell-unique ID without each call site re-allocating a "##TextCell_<ticket>_<field>" string.
/// Saves an estimated 4-6 std::string allocations per cell per frame across ~200 visible rows ×
/// ~30 columns.
struct CellIdScope {
    // Pushes ticket.id then fieldId — or the column index when fieldId is empty.
    // PushID("") hashes to the same value for every empty-FieldId column, which
    // would collapse synthetic / errored-catalog rows onto a single ImGui ID
    // and leak edit-state and popups between cells. The columnIndex fallback
    // keeps each cell distinct.
    CellIdScope(const char* ticketId, const char* fieldId, int columnIndex) {
        ImGui::PushID(ticketId);
        if (fieldId != nullptr && fieldId[0] != '\0') {
            ImGui::PushID(fieldId);
        } else {
            ImGui::PushID(columnIndex);
        }
    }
    ~CellIdScope() {
        ImGui::PopID();
        ImGui::PopID();
    }
    CellIdScope(const CellIdScope&) = delete;
    CellIdScope& operator=(const CellIdScope&) = delete;
};
} // namespace

void TicketFieldEditor::RenderFieldCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                        int columnIndex, const TrackerField* field, const std::string& currentValue,
                                        float availWidth, bool tooltipsEnabled, bool allowEdits,
                                        SpreadsheetState& state, std::vector<PendingFieldEdit>& pendingEdits,
                                        TrackerGridFieldAsyncState& trackerGridAsync,
                                        const std::string& dateFormatOption, int thresholdDays,
                                        bool singleClickToEdit) {
    SMATCHET_UI_PERF_SCOPE("RenderFieldCell");
    CellIdScope cellIds(ticket.id.c_str(), column.FieldId.c_str(), columnIndex);
    // Cached recorded-cmd-list dispatch: cache hit replays in ~5 µs / cell; miss invokes
    // the Lua provider on a recorder which builds an opcode list we cache. See
    // docs/design/applied/lua-recorded-cmd-list.md and AppController::TryRenderCachedLuaField.
    // Passing `allowEdits` through means draw:input_text can't bypass grid-level edit-
    // disabled states — read-only folds catalog + editmeta + allowEdits.
    const bool handledByLua =
        app.TryRenderCachedLuaField(column.FieldId, ticket, currentValue, availWidth, field, allowEdits);
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
            display =
                DisplayValueForTrackerDateField(column.FieldId, field, currentValue, dateFormatOption, thresholdDays);
        } else {
            display = app.ResolveDisplayValue(column.FieldId, field, currentValue);
        }
        if (disabled && display.empty()) {
            display = "-";
        }
        const bool isDescriptionField = IsDescriptionLikeFieldId(column.FieldId);
        if (isDescriptionField) {
            // Lazy: parse ADF → markdown only on actual hover, not per-cell per-frame.
            RenderClippedFieldText(display, availWidth, false, disabled, nullptr, false, &column.FieldId);
            if (tooltipsEnabled && ImGui::IsItemHovered()) {
                const std::string richVal = ticket.GetFieldRichValue(column.FieldId);
                std::string md;
                if (!richVal.empty()) {
                    try {
                        md = MarkdownConvert::AdfToMarkdown(nlohmann::json::parse(richVal));
                    } catch (const std::exception& e) {
                        LOG_DEBUG("ADF tooltip convert failed for field '%s': %s", column.FieldId.c_str(), e.what());
                    } catch (...) {
                        LOG_DEBUG("ADF tooltip convert failed for field '%s': unknown error", column.FieldId.c_str());
                    }
                }
                if (md.empty()) {
                    md = currentValue;
                }
                if (!md.empty()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                    MarkdownPreviewRender::Options opts;
                    opts.mode = MarkdownPreviewRender::Mode::Tooltip;
                    opts.clickableLinks = false;
                    opts.wrapWidth = ImGui::GetFontSize() * 48.0f;
                    MarkdownPreviewRender::Render(md, opts);
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
        } else {
            const std::string* tip = column.IsDateLike ? &currentValue : nullptr;
            RenderClippedFieldText(display, availWidth, tooltipsEnabled, disabled, tip, false, &column.FieldId);
        }
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
            TrackerGridFieldDisplay::RenderVotesField(app, ticket.id, currentValue, availWidth, tooltipsEnabled,
                                                      trackerGridAsync);
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
        // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // invisible background when normal
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

        if (ImGui::Button(buttonText.c_str(), ImVec2(availWidth > 0.0f ? availWidth : -FLT_MIN, 0.0f))) {
            s_ActiveWorklogState.IssueId = ticket.id;
            s_ActiveWorklogState.TimeSpent[0] = '\0';
            s_ActiveWorklogState.OriginalEstimate = ticket.GetFieldValue("timeoriginalestimate");
            s_ActiveWorklogState.TotalTimeSpent = ticket.GetFieldValue("timespent");
            s_ActiveWorklogState.TotalTimeRemaining = ticket.GetFieldValue("timeestimate");
            std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(),
                         sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
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
            },
            state, singleClickToEdit);
        return;
    case TicketGridColumn::RenderPlan::Cascading:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderCascadingSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled, state,
                                    singleClickToEdit);
        return;
    case TicketGridColumn::RenderPlan::MultiSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderMultiSelectEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled,
                                singleClickToEdit);
        return;
    case TicketGridColumn::RenderPlan::SingleSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderSingleSelectEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled,
                                 singleClickToEdit);
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
            dateFormatOption, thresholdDays, singleClickToEdit);
        return;
    case TicketGridColumn::RenderPlan::TextEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return;
        }
        RenderTextEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled, availWidth,
                         singleClickToEdit);
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
            if (loggedLabel.empty())
                loggedLabel = "0m";
            loggedLabel += " logged";

            std::string remainingLabel = FormatWorkDurationFromSeconds(displayRemSec);
            if (remainingLabel.empty())
                remainingLabel = "0m";
            remainingLabel += " remaining";

            ImGui::TextUnformatted(loggedLabel.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(remainingLabel.c_str()).x);
            ImGui::TextUnformatted(remainingLabel.c_str());

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.12f, 0.45f, 0.88f, 1.00f)); // Jira blue
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 14.0f), "");
            ImGui::PopStyleColor();

            if (!s_ActiveWorklogState.OriginalEstimate.empty()) {
                ImGui::TextDisabled("The original estimate for this work item was %s.",
                                    s_ActiveWorklogState.OriginalEstimate.c_str());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Inputs
            ImGui::Text("Time spent *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeSpent", s_ActiveWorklogState.TimeSpent,
                                                 sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr, nullptr, nullptr,
                                                 false)) {
                if (!s_ActiveWorklogState.TimeRemainingManuallyEdited) {
                    long long spentVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
                    long long remVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
                    if (spentVal > 0) {
                        long long newRem = (std::max)(0LL, remVal - spentVal);
                        std::string formattedRem = FormatWorkDurationFromSeconds(newRem);
                        std::strncpy(s_ActiveWorklogState.TimeRemaining, formattedRem.c_str(),
                                     sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    } else {
                        std::strncpy(s_ActiveWorklogState.TimeRemaining,
                                     s_ActiveWorklogState.TotalTimeRemaining.c_str(),
                                     sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                        s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
                    }
                }
            }
            ImGui::TextDisabled("Use the format: 2w 4d 6h 45m (w=weeks, d=days, h=hours, m=minutes)");

            ImGui::Spacing();
            ImGui::Text("Time remaining");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (DrawDurationFieldWithSuggestions("##WorklogTimeRemaining", s_ActiveWorklogState.TimeRemaining,
                                                 sizeof(s_ActiveWorklogState.TimeRemaining), 0, nullptr, nullptr,
                                                 &s_ActiveWorklogState.TimeRemainingManuallyEdited, false)) {
                // value changed!
            }

            ImGui::Spacing();
            ImGui::Text("Date started *");
            ImGui::SetNextItemWidth(-FLT_MIN);
            TrackerDateTimeFieldEditor::RenderGenericDatePicker("##WorklogDateStarted",
                                                                s_ActiveWorklogState.DateStarted, true);

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
                            std::strncpy(s_ActiveWorklogState.WorkDescription, item.c_str(),
                                         sizeof(s_ActiveWorklogState.WorkDescription) - 1);
                            s_ActiveWorklogState.WorkDescription[sizeof(s_ActiveWorklogState.WorkDescription) - 1] =
                                '\0';
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::InputTextMultiline("##WorklogDesc", s_ActiveWorklogState.WorkDescription,
                                      sizeof(s_ActiveWorklogState.WorkDescription),
                                      ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));

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
                        if (app.SubmitWorklog(s_ActiveWorklogState.IssueId, s_ActiveWorklogState.TimeSpent,
                                              s_ActiveWorklogState.TimeRemaining, adjEst,
                                              s_ActiveWorklogState.WorkDescription, s_ActiveWorklogState.DateStarted,
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
