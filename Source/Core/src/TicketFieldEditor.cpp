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
#include "ProjectResolver.h"
#include "TrackerFieldPayload.h"
#include "DictationInsertionRouter.h"
#include "MarkdownConvert.h"
#include "MarkdownPreviewRender.h"
#include "TicketFieldEditorLongTextPure.h"
#include "TicketFieldEditorDescriptionPure.h"
#include "TicketFieldEditorOptionFilterPure.h"
#include "TicketFieldEditorCommitPolicyPure.h"
#include "TicketFieldEditorDurationPopupPure.h"
#include "Ui/TouchCellEditGesture.h"
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
using TicketFieldEditorCommitPolicyPure::InlineEditLoadedTruncated;
using TicketFieldEditorCommitPolicyPure::ShouldCommitInlineFieldEdit;
using TicketFieldEditorCommitPolicyPure::ShouldEndInlineEdit;

// Compile-time: is this the mobile/touch build? On mobile an inline field edit commits only on
// explicit submit (Enter / IME "Done"); focus-loss cancels with no PUT (WS4 item 16 stray-PUT fix,
// see TicketFieldEditorCommitPolicyPure.h). The NDK toolchain auto-defines __ANDROID__; Source/Core
// has no other mobile gate today.
#if defined(__ANDROID__)
constexpr bool kMobileInlineEditBuild = true;
#else
constexpr bool kMobileInlineEditBuild = false;
#endif

// P1.3 mobile interaction model (#1018 item 23): the cell-editor open gesture (long-press on the
// touch build, click / double-click on desktop) lives in Ui/TouchCellEditGesture.h so all five cell
// editors share one rule — the inline text cell here plus the SingleSelect / MultiSelect / Cascading
// combos, and the Labels / DateTime editors in their own TUs. Pulled in unqualified for the call
// sites below; desktop codegen stays byte-identical (kMobileTouchBuild is constexpr-false there).
using SmatchetTouchEdit::ArmThenPopupCellGate;
using SmatchetTouchEdit::ShouldOpenCellEditorOnGesture;

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

    // CPP_CODE_AUDIT.md #33 (year-boundary UTC-offset inverted): compare full civil dates
    // (year + day-of-year), not bare tm_yday. tm_yday resets to 0 every January 1st, so
    // comparing it alone conflates "local is a calendar day ahead of UTC" with "local and
    // UTC are in different years" — e.g. Dec 31 (tm_yday=364) vs the following Jan 1
    // (tm_yday=0) used to compare as local-ahead (364 > 0) even for a NEGATIVE-offset user
    // whose local time is actually BEHIND UTC across that boundary, producing a nonsense
    // ~48h-off offset (e.g. "+43:00") in the worklog DateStarted sent to Jira. Multiplying
    // tm_year by 366 (>= any possible tm_yday) keeps the combined ordinal strictly
    // increasing across a year boundary while local/UTC can differ by at most one day.
    const long localOrdinal = static_cast<long>(tmLocal.tm_year) * 366L + tmLocal.tm_yday;
    const long utcOrdinal = static_cast<long>(tmUtc.tm_year) * 366L + tmUtc.tm_yday;
    if (localOrdinal > utcOrdinal) {
        localMin += 24 * 60;
    } else if (localOrdinal < utcOrdinal) {
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

// Decide whether the duration-suggestions popup should open this frame (down-arrow while active,
// or first activation with an empty buffer). Updates lastActiveId tracking. Call right after the
// InputText so the IsItemActive queries refer to it.
bool ComputeDurationPopupShouldOpen(const char* buf, ImGuiID currentId, ImGuiID& lastActiveId) {
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
    return shouldOpen;
}

// Render the duration-suggestions selectable popup (BeginPopup/EndPopup whole). On a pick,
// copies the value into buf and arms the reposition/selected-from-popup flags.
void DrawDurationSuggestionsPopup(char* buf, size_t bufSize, bool* outManuallyEdited, bool& needRepositionAndFocus,
                                  bool& valueSelectedFromPopup, bool requestClose) {
    if (ImGui::BeginPopup("duration_suggestions")) {
        if (requestClose) {
            // Type-to-edit fired: the user is editing the text directly, so close the suggestions.
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
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
}

bool DrawDurationFieldWithSuggestions(const char* label, char* buf, size_t bufSize, ImGuiInputTextFlags flags = 0,
                                      ImGuiInputTextCallback callback = nullptr, void* callbackUserData = nullptr,
                                      bool* outManuallyEdited = nullptr, bool forceOpenPopup = false,
                                      bool* outExplicitSubmit = nullptr, bool typeToEditFocus = false) {
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
    ImGuiID popupOpenKey = ImGui::GetID("##popupWasOpen");
    bool popupWasOpen = storage->GetInt(popupOpenKey, 0) != 0;

    float totalWidth = ImGui::GetContentRegionAvail().x;
    float inputWidth = totalWidth - 26.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    ImGui::SetNextItemWidth(inputWidth);

    if (forceOpenPopup) {
        // New edit session for this cell: drop the per-cell flags a previous session may have left
        // armed (e.g. Escape after a suggestion pick) so a stale focus-return or a spurious
        // popupJustClosed cannot leak into this session's first frame.
        needRepositionAndFocus = false;
        valueSelectedFromPopup = false;
        popupWasOpen = false;
        ImGui::OpenPopup("duration_suggestions");
    }

    if (needRepositionAndFocus) {
        ImGui::SetKeyboardFocusHere();
    }

    // Type-to-edit (estimate-edit-ux): when this editor is the grid's active edit target but the
    // suggestions popup holds keyboard focus (popup focus clears the parent ActiveId), a printable
    // keystroke would be dropped. SetKeyboardFocusHere() alone cannot save it: it is a nav-move
    // applied NEXT frame, io.InputQueueCharacters is cleared at EndFrame, and on the activation
    // frame the nav-requested activation makes InputText skip insertion and clear the queue. So
    // splice the queued printable chars straight into buf now (the focus request is for focus
    // only) and arm needRepositionAndFocus so the existing CallbackAlways caret-to-end machinery
    // places the cursor once the input activates.
    bool typeToEditClosePopup = false;
    if (typeToEditFocus) {
        ImGuiIO& io = ImGui::GetIO();
        bool hasPrintableQueuedChar = false;
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            if (TicketFieldEditorDurationPopupPure::IsPrintableTypedChar(
                    static_cast<unsigned int>(io.InputQueueCharacters[i]))) {
                hasPrintableQueuedChar = true;
                break;
            }
        }
        if (TicketFieldEditorDurationPopupPure::ShouldPullFocusForTypedChar(
                ImGui::GetActiveID() != 0, needRepositionAndFocus, hasPrintableQueuedChar)) {
            if (TicketFieldEditorDurationPopupPure::SpliceTypedCharsIntoBuf(
                    io.InputQueueCharacters.Data, io.InputQueueCharacters.Size, buf, bufSize)) {
                if (outManuallyEdited) {
                    *outManuallyEdited = true;
                }
                needRepositionAndFocus = true;
                typeToEditClosePopup = true;       // user is typing into the field → close the suggestions dropdown
                io.InputQueueCharacters.resize(0); // consumed — nothing else may double-insert them
            }
            ImGui::SetKeyboardFocusHere();
        }
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
    // IsItemDeactivated (not ...AfterEdit): a focus-loss must END the edit session even when the
    // value was not modified — otherwise opening an existing duration value and clicking a DIFFERENT
    // cell without changing it leaves the editor stuck open (...AfterEdit fires only after a real
    // edit). The popupIsOpen guard on finalDeactivated below keeps the popup-open frame (where the
    // input deactivates as the popup takes focus) from being treated as an edit-end; the dirty check
    // in the caller keeps an unchanged value from PUTing.
    deactivated = ImGui::IsItemDeactivated();
    if (deactivated && valueSelectedFromPopup) {
        valueSelectedFromPopup = false;
    }
    // Captured here so the queries refer to the InputText (the ▼ button redefines "last item"
    // below). Used by the popup-close refocus-vs-commit decision after the popup is drawn.
    const bool inputActiveNow = ImGui::IsItemActive();
    const bool inputHoveredNow = ImGui::IsItemHovered();
    // IsItemHovered() reads false on the popup-close frame even when the mouse is over the input
    // (the closing popup suppresses parent-window hover that frame), so the popup-close decision
    // below additionally uses a GEOMETRIC mouse-over-rect test. Capture the input's rect now,
    // before the ▼ button redefines "last item".
    const ImVec2 inputRectMin = ImGui::GetItemRectMin();
    const ImVec2 inputRectMax = ImGui::GetItemRectMax();

    ImGuiID currentId = ImGui::GetID("##duration_input");

    if (ComputeDurationPopupShouldOpen(buf, currentId, lastActiveId)) {
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
    DrawDurationSuggestionsPopup(buf, bufSize, outManuallyEdited, needRepositionAndFocus, valueSelectedFromPopup,
                                 typeToEditClosePopup);

    bool popupJustClosed = popupWasOpen && !popupIsOpen;

    bool finalDeactivated = false;
    if (deactivated) {
        if (!buttonHovered && !popupIsOpen) {
            finalDeactivated = true;
        }
    }
    // A popup-close caused by a click landing back on the input itself is a refocus-for-editing
    // (empty buffer: keep editing — no spurious empty-value commit; existing value: caret into the
    // text), never a commit-deactivation. When the popup-close ate the click (input hovered but
    // not re-activated), arm the focus-return so the input regains keyboard focus next frame.
    // Geometric mouse-over-input test — reliable on the popup-close frame where IsItemHovered() is
    // suppressed by the closing popup. clip=false so the popup's clip rect can't hide the input.
    const bool mouseOverInput = inputHoveredNow || ImGui::IsMouseHoveringRect(inputRectMin, inputRectMax, false);
    if (TicketFieldEditorDurationPopupPure::ShouldFinalizeOnPopupClose(popupJustClosed, needRepositionAndFocus,
                                                                       inputActiveNow, mouseOverInput)) {
        finalDeactivated = true;
    } else if (TicketFieldEditorDurationPopupPure::ShouldRearmFocusOnPopupClose(popupJustClosed, needRepositionAndFocus,
                                                                                mouseOverInput, inputActiveNow)) {
        needRepositionAndFocus = true;
    }

    storage->SetInt(popupOpenKey, popupIsOpen ? 1 : 0);
    storage->SetInt(selectedFromPopupKey, valueSelectedFromPopup ? 1 : 0);
    // Store updated state back to ImGuiStorage
    storage->SetInt(needFocusKey, needRepositionAndFocus ? 1 : 0);
    storage->SetInt(lastActiveIdKey, static_cast<int>(lastActiveId));

    ImGui::PopID();
    // Report the explicit-Enter submit separately from the combined return so callers can
    // distinguish a true submit from a focus-loss deactivation (mobile commit policy, WS4 item 16).
    if (outExplicitSubmit) {
        *outExplicitSubmit = submitted;
    }
    return submitted || finalDeactivated;
}

void QueueEdit(const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
               std::vector<PendingFieldEdit>& pendingEdits, const std::string& originalValue) {
    PendingFieldEdit edit;
    edit.IssueId = issueId;
    edit.Field = field;
    edit.Values = values;
    // Scalar conflict base (ADR-0016): the pre-edit display value, captured at the single
    // QueueEdit choke point so every scalar grid/cell editor path records a base uniformly.
    // Rich (long-text modal) edits set OriginalRichValue instead and never reach here.
    edit.OriginalValue = originalValue;
    edit.HasOriginalValue = true; // a scalar base WAS captured here, even if it's blank (ADR-0016).
    pendingEdits.push_back(std::move(edit));
}

std::string EncodeCascadingSelection(const std::string& parentId, const std::string& childId) {
    return parentId + "\x1f" + childId;
}

using TicketFieldEditorOptionFilterPure::OptionMatchesFilter;

// Draws the inline field-value icon (status/priority sprite) centred-vertically inside the
// [rectMin, rectMax] cell rect at frame height. Shared verbatim by the single-select preview
// and combo paths, which previously inlined identical scale-and-blit blocks.
void DrawInlineFieldIconOverlay(const SmatchetLoadedIconTexture& icon, const ImVec2& rectMin, const ImVec2& rectMax) {
    if (icon.Texture == nullptr || icon.Width <= 0 || icon.Height <= 0) {
        return;
    }
    const float maxEdge = ImGui::GetFrameHeight();
    const float iw = static_cast<float>(icon.Width);
    const float ih = static_cast<float>(icon.Height);
    const float scale = maxEdge / (std::max)(iw, ih);
    const float dw = iw * scale;
    const float dh = ih * scale;
    const float rowH = rectMax.y - rectMin.y;
    const ImVec2 overlayP0(rectMin.x + 4.0f, rectMin.y + ((rowH - dh) * 0.5f));
    const ImVec2 overlayP1(overlayP0.x + dw, overlayP0.y + dh);
    ImGui::GetWindowDrawList()->AddImage(icon.Texture->GetTexRef(), overlayP0, overlayP1, ImVec2(0.0f, 0.0f),
                                         ImVec2(1.0f, 1.0f));
}

// Shows the standard hover tooltip for a select-cell preview when its text is clipped by the
// available cell width. Caller must invoke this immediately after the Selectable/combo so
// IsItemHovered() refers to it. Shared by the single/multi/cascading editors.
void DrawClippedPreviewTooltip(bool tooltipsEnabled, const char* previewCStr, float availWidth) {
    if (!tooltipsEnabled || !ImGui::IsItemHovered()) {
        return;
    }
    const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
    const bool previewClipped = (availWidth > 0.0f && psz.x > availWidth + 1.0f);
    if (!previewClipped) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
    ImGui::TextUnformatted(previewCStr);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// Draws the active inline single-line text editor for a non-ADF text cell and commits / cancels
// the edit. Extracted from RenderTextEditor's editing branch; behaviour byte-identical (duration
// fields route through the suggestion widget, plain fields through InputText). Caller has already
// verified state.IsEditingField(ticket.id, field.Id) for a non-ADF field.
void RenderTextInlineEdit(const CachedTicket& ticket, const TrackerField& field, SpreadsheetState& state,
                          std::vector<PendingFieldEdit>& pendingEdits) {
    const bool editJustStarted = state.EditJustStarted;
    const bool isDuration = IsTimeDurationField(field.Id);
    bool explicitSubmit = false;
    bool deactivated = false;

    if (isDuration) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (editJustStarted) {
            ImGui::SetKeyboardFocusHere();
        }
        EditCbUser cbUser{&state};
        const bool committedOrDeactivated = DrawDurationFieldWithSuggestions(
            "##textedit_duration", state.EditBuffer, sizeof(state.EditBuffer), ImGuiInputTextFlags_CallbackAlways,
            InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser), nullptr, editJustStarted,
            &explicitSubmit, /*typeToEditFocus=*/true);
        // The duration widget folds explicit-Enter and its internal focus-loss (finalDeactivated)
        // into one return and reports the Enter separately via outExplicitSubmit; the residual is
        // the focus-loss. The widget's last sub-item is the popup/button, so a top-level
        // IsItemDeactivated() here would not refer to the duration field — derive it.
        deactivated = committedOrDeactivated && !explicitSubmit;
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (editJustStarted) {
            ImGui::SetKeyboardFocusHere();
        }
        EditCbUser cbUser{&state};
        explicitSubmit = ImGui::InputText("##textedit", state.EditBuffer, sizeof(state.EditBuffer),
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                          InputTextCallback_ClearSelectOnEditOpen, static_cast<void*>(&cbUser));
        // IsItemDeactivated (not ...AfterEdit): a focus-loss ENDS the edit session even when the
        // value was not modified, so clicking another cell after merely refocusing closes the
        // editor. The dirty check below ensures an unchanged value still never PUTs.
        deactivated = !editJustStarted && ImGui::IsItemDeactivated();
    }

    const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
    const std::string originalValue = ticket.GetFieldValue(field.Id);
    // Dirty check against the buffer's start content (state.EditInitialValue) — the value as loaded
    // when the edit opened — NOT the live field value. They are the same today (both GetFieldValue),
    // but a background grid refresh / sync can mutate the field's stored value mid-edit; comparing
    // the buffer against the live value would then read an untouched edit as dirty and fire a
    // spurious PUT. EditInitialValue is captured once at edit-start, so the dirty verdict reflects
    // only what the USER typed, never an out-of-band store change.
    const bool valueChanged = state.EditInitialValue != std::string(state.EditBuffer);
    // DR23: if the stored value was longer than EditBuffer it was truncated on seed. Committing the
    // truncated copy would silently overwrite the field, destroying the untruncated tail — refuse the
    // commit (the value stays intact) and warn. The single-line inline cap stays; edit long values in
    // the long-text modal, which loads the full document.
    const bool loadedTruncated = InlineEditLoadedTruncated(originalValue.size(), sizeof(state.EditBuffer));
    if (loadedTruncated && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T("field_editor.inline_too_long",
                                                        "Value too long to edit inline — the change will not be "
                                                        "saved (it would truncate the field). Edit it elsewhere."));
    }
    if (escapePressed) {
        state.ClearEditing(); // cancel — never PUT
    } else if (!loadedTruncated &&
               ShouldCommitInlineFieldEdit(explicitSubmit, deactivated, kMobileInlineEditBuild, valueChanged)) {
        QueueEdit(ticket.id, field, {state.EditBuffer}, pendingEdits, originalValue);
        state.ClearEditing();
    } else if (ShouldEndInlineEdit(escapePressed, explicitSubmit, deactivated)) {
        state.ClearEditing(); // ended without a PUT (unchanged value, or mobile focus-loss)
    } else if (editJustStarted) {
        state.EditJustStarted = false;
    }
}

// Draws the hover preview tooltip for a display-mode text cell. Description-like fields render
// markdown (ADF rich value to markdown), callstack fields use the semantic tokenizer, and other
// fields fall back to plain wrapped text. Lifted verbatim from the text editor's tooltip block.
// The caller has already gated on tooltips-enabled plus clip / newline / description plus hover.
void DrawTextCellTooltip(const CachedTicket& ticket, const TrackerField& field, const std::string& currentValue,
                         const std::string& valueForDisplay, bool isDescriptionLike) {
    const std::string* rawTip = IsTrackerDateOrDateTimeField(field.Id, &field) ? &currentValue : nullptr;
    // Convert ADF rich value → markdown so paragraph breaks (\n\n) render as
    // separate paragraphs. Plain text currentValue uses single \n (soft break in
    // markdown) which would join paragraphs into one line.
    std::string descMd;
    if (isDescriptionLike) {
        descMd =
            TicketFieldEditorLongTextPure::RichValueToTooltipMarkdown(ticket.GetFieldRichValue(field.Id), currentValue);
    }
    const std::string& tipSource =
        isDescriptionLike ? descMd : ((rawTip && !rawTip->empty()) ? *rawTip : valueForDisplay);
    // Slice 7 of code-syntax-coloring-and-tooltips — when the field is
    // the configured callstack field, colour the tooltip via the semantic callstack tokenizer
    // (same as the cell + RenderClippedFieldText path). Without this branch the callstack
    // tooltip fell through to plain TextUnformatted while the cell was coloured.
    const bool isCallstack = IsCallstackFieldId(field.Id);
    if (tipSource.empty()) {
        return;
    }
    ImGui::BeginTooltip();
    if (isDescriptionLike) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        MarkdownPreviewRender::Options opts;
        opts.mode = MarkdownPreviewRender::Mode::Tooltip;
        opts.clickableLinks = false;
        opts.wrapWidth = ImGui::GetFontSize() * 48.0f;
        MarkdownPreviewRender::Render(tipSource, opts);
        ImGui::PopTextWrapPos();
    } else if (isCallstack) {
        // No wrap-pos: callstack source lines are per-line semantic tokens
        // (Module!Class::Method() [File:Line]); word-wrapping them mid-line
        // mangles the layout. Render each line full-width like the cell path.
        DrawColoredCallstackText(tipSource.c_str());
    } else {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tipSource.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTooltip();
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
        RenderTextInlineEdit(ticket, field, state, pendingEdits);
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
    const bool textCellClicked = ImGui::Selectable(display.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
    if (ShouldOpenCellEditorOnGesture(textCellClicked, singleClickToEdit)) {
        state.StartEditingField(ticket.id, field.Id, currentValue);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
    const bool horizontallyClipped = (regionAvail > 0.0f && textSize.x > regionAvail + 1.0f);
    // GitHub body + Jira description carry markdown — render preview tooltip on
    // hover even when the cell fits in one line (no clip / no newline). Other
    // text fields keep the clip-gate so plain values don't tooltip noisily.
    const bool isDescriptionLike = IsDescriptionLikeFieldId(field.Id);
    if (tooltipsEnabled && (hasNewlineInValue || horizontallyClipped || isDescriptionLike) && ImGui::IsItemHovered()) {
        DrawTextCellTooltip(ticket, field, currentValue, valueForDisplay, isDescriptionLike);
    }
}

// Draws the open single-select combo body: clear option, auto-focused filter input, and the
// filtered option list with typeahead enter-commit. Lifted from the single-select editor, whose
// caller owns the begin / end combo pair. Behaviour byte-identical to the inlined block.
void RenderSingleSelectComboBody(const CachedTicket& ticket, const TrackerField& field, const std::string& currentValue,
                                 SpreadsheetState& state, std::vector<PendingFieldEdit>& pendingEdits,
                                 const std::string& editorKey) {
    const bool justOpened = (state.SingleSelectActiveKey != editorKey);
    if (justOpened) {
        state.SingleSelectActiveKey = editorKey;
        state.SingleSelectSearchBuf[0] = '\0';
    }

    const std::string currentId = ResolveOptionId(field, currentValue);
    const bool selectedNone = currentId.empty();
    if (ImGui::Selectable("<clear>", selectedNone)) {
        QueueEdit(ticket.id, field, {}, pendingEdits, ticket.GetFieldValue(field.Id));
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
    bool drewAny = false;
    for (const auto& option : field.AllowedValueOptions) {
        if (!OptionMatchesFilter(option, filterLower)) {
            continue;
        }
        const std::string optionId = option.Id.empty() ? option.Value : option.Id;
        if (firstMatch == nullptr) {
            firstMatch = &option;
        }
        drewAny = true;
        const bool isSelected = (option.Id == currentId);
        ImGui::PushID(optionId.c_str());
        if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
            // CPP_CODE_AUDIT.md #33 (single-select combo clears the field for id-less
            // options): queue `optionId` (the same Id-or-Value fallback used for the
            // widget's own ImGui id above), not the raw `option.Id` — an id-less option
            // has `option.Id.empty()`, so queuing it directly sent {""} (a field clear)
            // instead of the option the user actually clicked.
            QueueEdit(ticket.id, field, {optionId}, pendingEdits, ticket.GetFieldValue(field.Id));
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
    }
    if (!drewAny) {
        ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
    }
    // Pressing enter commits when the filter narrows to a single match. When several still match,
    // the top one is committed as a least-surprise default that mirrors typeahead pickers.
    if (submitOnEnter && firstMatch != nullptr) {
        const std::string firstMatchId = firstMatch->Id.empty() ? firstMatch->Value : firstMatch->Id;
        QueueEdit(ticket.id, field, {firstMatchId}, pendingEdits, ticket.GetFieldValue(field.Id));
        ImGui::CloseCurrentPopup();
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
    // priority column they fired ~100×/frame and the result was discarded.
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
    if (!ArmThenPopupCellGate(state.EditArmedKey, state.EditArmedJustOpened, editorKey, "##singleselect", previewCStr,
                              cellAvail, singleClickToEdit)) {
        const ImVec2 selMin = ImGui::GetItemRectMin();
        const ImVec2 selMax = ImGui::GetItemRectMax();
        if (haveOverlayIcon) {
            DrawInlineFieldIconOverlay(overlayIcon, selMin, selMax);
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            if (haveOverlayIcon && preview.empty()) {
                preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
                previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
            }
            DrawClippedPreviewTooltip(tooltipsEnabled, previewCStr, cellAvail);
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
        RenderSingleSelectComboBody(ticket, field, currentValue, state, pendingEdits, editorKey);
        ImGui::EndCombo();
    }

    if (haveOverlayIcon) {
        DrawInlineFieldIconOverlay(overlayIcon, comboMin, comboMax);
    }
    // Tooltip only fires when the combo is actually hovered — defer the clip-test text measurement
    // (and resolve the preview lazily for the icon-overlay branch where preview was skipped).
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        if (haveOverlayIcon && preview.empty()) {
            preview = app.ResolveDisplayValue(field.Id, &field, currentValue);
            previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
        }
        DrawClippedPreviewTooltip(tooltipsEnabled, previewCStr, comboAvailBefore);
    }
}

// Draws the open multi-select combo body: clear-all, search input, and the per-option checkbox
// list (checked options are always shown even when filtered out). Extracted from
// RenderMultiSelectEditor; caller owns BeginCombo/EndCombo and the effective-option resolution.
// `selectedSet` is mutated as the user toggles checkboxes. Behaviour byte-identical.
void RenderMultiSelectComboBody(const CachedTicket& ticket, const TrackerField& field, SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits, const std::string& editorKey,
                                const std::vector<TrackerFieldOption>* opts, bool componentsLoaded,
                                std::unordered_set<std::string>& selectedSet) {
    if (state.MultiSelectActiveKey != editorKey) {
        state.MultiSelectActiveKey = editorKey;
        state.MultiSelectSearchBuf[0] = '\0';
    }

    if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
        QueueEdit(ticket.id, field, {}, pendingEdits, ticket.GetFieldValue(field.Id));
    }
    ImGui::Separator();

    const std::string searchHint = field.Id == "components" ? "Search components" : "Search options";
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##MultiSelectSearch", searchHint.c_str(), state.MultiSelectSearchBuf,
                             sizeof(state.MultiSelectSearchBuf));
    ImGui::Separator();

    const std::string filterLower = ToLowerAsciiCopy(TrimCopy(state.MultiSelectSearchBuf));
    bool drewAny = false;
    for (const auto& option : *opts) {
        const std::string optionId = option.Id.empty() ? option.Value : option.Id;
        bool checked = (selectedSet.find(optionId) != selectedSet.end());
        if (!checked && !OptionMatchesFilter(option, filterLower)) {
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
            QueueEdit(ticket.id, field, updated, pendingEdits, ticket.GetFieldValue(field.Id));
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
        if (field.Id == "components" && !componentsLoaded && filterLower.empty()) {
            // Per-project options have not loaded yet. The lazy fetch was kicked above and the
            // real options appear on a later frame once it lands. A successful fetch that
            // returned zero components marks the project loaded, so a genuinely empty project
            // shows the no-options text rather than spinning here forever.
            ImGui::TextDisabled("Loading components\xE2\x80\xA6");
        } else {
            ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
        }
    }
}

void RenderMultiSelectEditor(AppController& app, const CachedTicket& ticket, const TrackerField& field,
                             const std::string& currentValue, SpreadsheetState& state,
                             std::vector<PendingFieldEdit>& pendingEdits, bool tooltipsEnabled,
                             bool singleClickToEdit) {
    // For the grid components MultiSelect on cross-project views, options are scoped to THIS row's
    // own Jira project (resolved from the issue-key prefix). When the per-project map has not been
    // warmed for this project, kick a lazy fetch and use the (empty) per-project set anyway — never
    // fall back to field.AllowedValueOptions, which is a CROSS-PROJECT UNION from the scoped catalog
    // and would leak other projects' components into this row's dropdown. The list populates next
    // frame once the async fetch lands. Non-components fields keep the global AllowedValueOptions.
    const std::vector<TrackerFieldOption>* opts = &field.AllowedValueOptions;
    std::vector<TrackerFieldOption> perProject;
    bool componentsLoaded = true; // non-components fields are always "loaded"
    if (field.Id == "components") {
        const std::string projectKey = smatchet::ExtractIssueKeyPrefix(ticket.id);
        perProject = app.GetComponentOptionsForProject(projectKey);
        componentsLoaded = app.IsProjectComponentsLoaded(projectKey);
        if (!componentsLoaded) {
            // Non-blocking lazy fetch; mutates in-flight bookkeeping + spawns a worker.
            app.EnsureProjectComponentsLoaded(projectKey);
        }
        opts = &perProject;
    }
    // Resolve current selection ids against the effective option set (matters when a row's project
    // uses a per-project list distinct from the global catalog).
    TrackerField effectiveField = field;
    effectiveField.AllowedValueOptions = *opts;
    std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(effectiveField, currentValue);
    std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
    // Resolve the collapsed-cell preview against the same per-project option set as the dropdown
    // list (effectiveField). Passing the global &field here made cross-project component ids fall
    // through ResolveDisplayValueForSubmittedSelection's allowed-value scan and render the raw id.
    const std::string preview = app.ResolveDisplayValue(field.Id, &effectiveField, currentValue);
    const float cellAvail = ImGui::GetContentRegionAvail().x;
    // Arm-then-popup: see RenderSingleSelectEditor. Shared scaffold in ArmThenPopupCellGate.
    const std::string editorKey = ticket.id + "::" + field.Id;
    // Match RenderSingleSelectEditor: an empty preview becomes the EmptySelectPreviewLabel placeholder
    // so the collapsed Selectable keeps a tappable width on touch (an empty cell otherwise collapses
    // to a zero-width long-press target).
    const char* previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
    if (!ArmThenPopupCellGate(state.EditArmedKey, state.EditArmedJustOpened, editorKey, "##multiselect", previewCStr,
                              cellAvail, singleClickToEdit)) {
        DrawClippedPreviewTooltip(tooltipsEnabled, previewCStr, cellAvail);
        return;
    }
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##multiselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        RenderMultiSelectComboBody(ticket, field, state, pendingEdits, editorKey, opts, componentsLoaded, selectedSet);
        ImGui::EndCombo();
    }
    DrawClippedPreviewTooltip(tooltipsEnabled, preview.c_str(), comboAvailBefore);
}

void RenderCascadingSelectEditor(const AppController& app, const CachedTicket& ticket, const TrackerField& field,
                                 const std::string& currentValue, std::vector<PendingFieldEdit>& pendingEdits,
                                 bool tooltipsEnabled, SpreadsheetState& state, bool singleClickToEdit) {
    std::string parentId;
    std::string childId;
    TryResolveCascadingSelection(field, currentValue, parentId, childId);
    const std::string preview = app.ResolveDisplayValue(field.Id, &field, currentValue);

    const float cellAvail = ImGui::GetContentRegionAvail().x;
    // Arm-then-popup: see RenderSingleSelectEditor. Shared scaffold in ArmThenPopupCellGate.
    const std::string editorKey = ticket.id + "::" + field.Id;
    // Match RenderSingleSelectEditor: an empty preview becomes the EmptySelectPreviewLabel placeholder
    // so the collapsed Selectable keeps a tappable width on touch (an empty cell otherwise collapses
    // to a zero-width long-press target).
    const char* previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
    if (!ArmThenPopupCellGate(state.EditArmedKey, state.EditArmedJustOpened, editorKey, "##cascadeselect", previewCStr,
                              cellAvail, singleClickToEdit)) {
        DrawClippedPreviewTooltip(tooltipsEnabled, previewCStr, cellAvail);
        return;
    }
    // ID uniqueness comes from RenderFieldCell's CellIdScope (ticket.id + field.Id on stack).
    const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##cascadeselect", preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable("<clear>", parentId.empty() && childId.empty())) {
            QueueEdit(ticket.id, field, {}, pendingEdits, ticket.GetFieldValue(field.Id));
        }
        ImGui::Separator();
        for (const auto& parent : field.AllowedValueOptions) {
            if (parent.Children.empty()) {
                const bool selected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable(parent.Value.c_str(), selected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits,
                              ticket.GetFieldValue(field.Id));
                }
                continue;
            }

            if (ImGui::BeginMenu(parent.Value.c_str())) {
                const bool parentOnlySelected = (parent.Id == parentId && childId.empty());
                if (ImGui::Selectable("<parent only>", parentOnlySelected)) {
                    QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits,
                              ticket.GetFieldValue(field.Id));
                }
                ImGui::Separator();
                for (const auto& child : parent.Children) {
                    const bool selected = (parent.Id == parentId && child.Id == childId);
                    if (ImGui::Selectable(child.Value.c_str(), selected)) {
                        QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, child.Id)}, pendingEdits,
                                  ticket.GetFieldValue(field.Id));
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndCombo();
    }
    DrawClippedPreviewTooltip(tooltipsEnabled, preview.c_str(), comboAvailBefore);
}

// Draws the "Log work" / time-spent button for the SpecialTimeSpent column and, on click, primes
// the worklog dialog state. Extracted from RenderFieldCell's switch case; behaviour byte-identical.
void RenderTimeSpentButton(const CachedTicket& ticket, const std::string& currentValue, float availWidth,
                           bool tooltipsEnabled) {
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
}

// Computes the progress-bar logged/remaining seconds for the worklog dialog. Pure arithmetic on
// the parsed durations — extracted so the modal draw body stays layout-focused. Mirrors the
// auto-deduction (spent reduces remaining) unless the user manually edited the remaining field.
void ComputeWorklogProgressSeconds(long long& outDisplaySpentSec, long long& outDisplayRemSec) {
    const long long spentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeSpent);
    const long long remSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
    const long long newSpentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);

    outDisplaySpentSec = spentSec + newSpentSec;
    outDisplayRemSec = remSec;
    if (newSpentSec > 0 && !s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = (std::max)(0LL, remSec - newSpentSec);
    } else if (s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeRemaining);
    }
}

// Validates the worklog inputs and, on success, submits via AppController and dismisses the popup.
// Sets s_ActiveWorklogState.ErrorMsg on any validation/submit failure. Extracted from the modal's
// Save button handler; behaviour byte-identical.
void HandleWorklogSave(AppController& app) {
    s_ActiveWorklogState.ErrorMsg.clear();
    long long sVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
    if (sVal <= 0) {
        s_ActiveWorklogState.ErrorMsg = "Invalid Time spent format. Please use e.g. 2h 30m.";
        return;
    }
    ParsedJiraDateTime dummyDt;
    if (!TryParseJiraDateTime(s_ActiveWorklogState.DateStarted, dummyDt)) {
        s_ActiveWorklogState.ErrorMsg = "Invalid Date started format.";
        return;
    }
    std::string adjEst = s_ActiveWorklogState.TimeRemainingManuallyEdited ? "new" : "auto";
    std::string outErr;
    if (app.SubmitWorklog(s_ActiveWorklogState.IssueId, s_ActiveWorklogState.TimeSpent,
                          s_ActiveWorklogState.TimeRemaining, adjEst, s_ActiveWorklogState.WorkDescription,
                          s_ActiveWorklogState.DateStarted, outErr)) {
        ImGui::CloseCurrentPopup();
        s_ActiveWorklogState.Initialized = false;
    } else {
        s_ActiveWorklogState.ErrorMsg = "Failed: " + outErr;
    }
}

// Draws the time-tracking modal popup (logged/remaining bar, duration inputs, date picker, work
// description with templates, Save/Cancel). Owns its OpenPopup→BeginPopupModal lifecycle keyed on
// s_ActiveWorklogState. Extracted from the tail of RenderFieldCell; behaviour byte-identical.
// Logged/remaining labels + Jira-blue progress bar + original-estimate note.
void DrawWorklogProgressBar() {
    long long displaySpentSec = 0;
    long long displayRemSec = 0;
    ComputeWorklogProgressSeconds(displaySpentSec, displayRemSec);

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
}

// Time-spent + time-remaining duration inputs, including the auto-decrement of remaining.
void DrawWorklogTimeInputs() {
    ImGui::Text("Time spent *");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (DrawDurationFieldWithSuggestions("##WorklogTimeSpent", s_ActiveWorklogState.TimeSpent,
                                         sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr, nullptr, nullptr, false)) {
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
                std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(),
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
}

// Work-description label + templates popup + the multiline description input.
void DrawWorklogDescription() {
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
                    s_ActiveWorklogState.WorkDescription[sizeof(s_ActiveWorklogState.WorkDescription) - 1] = '\0';
                }
            }
        }
        ImGui::EndPopup();
    }
    ImGui::InputTextMultiline("##WorklogDesc", s_ActiveWorklogState.WorkDescription,
                              sizeof(s_ActiveWorklogState.WorkDescription),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
}

void RenderTimeTrackingModal(AppController& app, const CachedTicket& ticket) {
    if (!(s_ActiveWorklogState.Initialized && s_ActiveWorklogState.IssueId == ticket.id)) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_Always);
    if (s_ActiveWorklogState.JustOpened) {
        ImGui::OpenPopup("TimeTrackingPopup");
        s_ActiveWorklogState.JustOpened = false;
    }

    if (!ImGui::BeginPopupModal("TimeTrackingPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        s_ActiveWorklogState.Initialized = false;
        return;
    }

    ImGui::Text("Time tracking: %s", ticket.id.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    // Logged & Remaining progress bar
    DrawWorklogProgressBar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Inputs
    DrawWorklogTimeInputs();

    ImGui::Spacing();
    ImGui::Text("Date started *");
    ImGui::SetNextItemWidth(-FLT_MIN);
    TrackerDateTimeFieldEditor::RenderGenericDatePicker("##WorklogDateStarted", s_ActiveWorklogState.DateStarted, true);

    ImGui::Spacing();
    DrawWorklogDescription();

    if (!s_ActiveWorklogState.ErrorMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_ActiveWorklogState.ErrorMsg.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Buttons
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        HandleWorklogSave(app);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        ImGui::CloseCurrentPopup();
        s_ActiveWorklogState.Initialized = false;
    }

    ImGui::EndPopup();
}

} // namespace

#if defined(SMATCHET_BUILD_UI_TESTS)
// Test-only external-linkage forwarder for the bucket-E inline-edit commit/cancel test
// (tests/ui/duration_inline_edit_commit.test.cpp). RenderTextInlineEdit lives in the anonymous
// namespace above (its helpers — QueueEdit, DrawDurationFieldWithSuggestions, the Should* policy
// calls — are all anon-scoped), so the engine test cannot call it directly. Anon-namespace names
// are still visible at file scope in the rest of this TU, so this file-scope forwarder can reach
// it. Declared in TicketFieldEditor_detail.h; compiled only in the SMATCHET_BUILD_UI_TESTS build.
void SmatchetTest_RenderTextInlineEdit(const CachedTicket& ticket, const TrackerField& field, SpreadsheetState& state,
                                       std::vector<PendingFieldEdit>& pendingEdits) {
    RenderTextInlineEdit(ticket, field, state, pendingEdits);
}
#endif // SMATCHET_BUILD_UI_TESTS

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

// Dispatches the "special" column render plans (attachments, watchers, votes, worklog, progress,
// issue-restriction) to their grid-field-display renderers. Returns true when the cell was drawn.
// A false result means the plan's guard did not match, so the caller falls through to its original
// break path — the worklog modal tail — byte-identical to the inlined switch cases. The time-spent
// plan is intentionally NOT routed here, since it always draws and then falls through to the modal.
bool TryRenderSpecialColumnPlan(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                                const TrackerField* field, const std::string& currentValue, float availWidth,
                                bool tooltipsEnabled, TrackerGridFieldAsyncState& trackerGridAsync) {
    switch (column.Plan) {
    case TicketGridColumn::RenderPlan::SpecialAttachment:
        if (field == nullptr || IsAttachmentFieldId(field->Id)) {
            TrackerGridFieldDisplay::RenderAttachmentsField(app, currentValue, availWidth, tooltipsEnabled);
            return true;
        }
        return false;
    case TicketGridColumn::RenderPlan::SpecialWatchers:
        if (field == nullptr || TrackerGridFieldDisplay::IsWatchersColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWatchersField(app, ticket.id, currentValue, availWidth, tooltipsEnabled,
                                                         trackerGridAsync);
            return true;
        }
        return false;
    case TicketGridColumn::RenderPlan::SpecialVotes:
        if (field == nullptr || TrackerGridFieldDisplay::IsVotesColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderVotesField(app, ticket.id, currentValue, availWidth, tooltipsEnabled,
                                                      trackerGridAsync);
            return true;
        }
        return false;
    case TicketGridColumn::RenderPlan::SpecialWorklog:
        if (field == nullptr || TrackerGridFieldDisplay::IsWorklogColumnId(field->Id)) {
            TrackerGridFieldDisplay::RenderWorklogField(currentValue, availWidth, tooltipsEnabled);
            return true;
        }
        return false;
    case TicketGridColumn::RenderPlan::SpecialProgress:
        return TrackerGridFieldDisplay::TryRenderProgressJsonField(currentValue, availWidth);
    case TicketGridColumn::RenderPlan::SpecialIssueRestriction:
        return TrackerGridFieldDisplay::TryRenderIssueRestrictionField(currentValue, availWidth, tooltipsEnabled);
    default:
        return false;
    }
}

// Renders a read-only / display-mode cell value (the fall-through for every non-editable column
// plan, and the editable plans when allowEdits is false). Date-like columns format via the date
// helper; components cells resolve names against the row's own project options; description-like
// fields get a lazy markdown hover tooltip. Extracted verbatim from RenderFieldCell's
// renderPlainText lambda; behaviour byte-identical.
void RenderPlainTextCell(AppController& app, const CachedTicket& ticket, const TicketGridColumn& column,
                         const TrackerField* field, const std::string& currentValue, float availWidth,
                         bool tooltipsEnabled, const std::string& dateFormatOption, int thresholdDays, bool disabled) {
    std::string display;
    if (column.IsDateLike) {
        display = DisplayValueForTrackerDateField(column.FieldId, field, currentValue, dateFormatOption, thresholdDays);
    } else {
        // Components cells on cross-project views resolve their display name against this row's
        // own project options (same per-project pattern as RenderMultiSelectEditor). The global
        // components field has empty/wrong AllowedValueOptions cross-project, so without this the
        // collapsed cell rendered the raw numeric component id instead of its name.
        const TrackerField* displayField = field;
        TrackerField effectiveField;
        if (field != nullptr && column.FieldId == "components") {
            const std::string projectKey = smatchet::ExtractIssueKeyPrefix(ticket.id);
            std::vector<TrackerFieldOption> perProject = app.GetComponentOptionsForProject(projectKey);
            // Always scope to this row's project and never fall back to the global cross-project
            // union. When not yet loaded, kick a lazy fetch and use the empty per-project set so
            // selected-id names resolve on a later frame once the fetch lands (raw id shows
            // briefly). Gate on the loaded flag rather than an empty vector so a successful zero-
            // component fetch does not relaunch a worker every frame.
            if (!app.IsProjectComponentsLoaded(projectKey)) {
                app.EnsureProjectComponentsLoaded(projectKey);
            }
            effectiveField = *field;
            effectiveField.AllowedValueOptions = std::move(perProject);
            displayField = &effectiveField;
        }
        display = app.ResolveDisplayValue(column.FieldId, displayField, currentValue);
    }
    if (disabled && display.empty()) {
        display = "-";
    }
    const bool isDescriptionField = IsDescriptionLikeFieldId(column.FieldId);
    if (isDescriptionField) {
        // Lazy: parse ADF → markdown only on actual hover, not per-cell per-frame.
        RenderClippedFieldText(display, availWidth, false, disabled, nullptr, false, &column.FieldId);
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const std::string md = TicketFieldEditorLongTextPure::RichValueToTooltipMarkdown(
                ticket.GetFieldRichValue(column.FieldId), currentValue);
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
}
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
    // docs/plans/shipped/lua-recorded-cmd-list.md and AppController::TryRenderCachedLuaField.
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

    if (TryRenderSpecialColumnPlan(app, ticket, column, field, currentValue, availWidth, tooltipsEnabled,
                                   trackerGridAsync)) {
        return;
    }

    if (DispatchEditorByPlan(app, ticket, column, field, currentValue, availWidth, tooltipsEnabled, allowEdits, state,
                             pendingEdits, dateFormatOption, thresholdDays, singleClickToEdit)) {
        return;
    }

    RenderTimeTrackingModal(app, ticket);
}

bool TicketFieldEditor::DispatchEditorByPlan(AppController& app, const CachedTicket& ticket,
                                             const TicketGridColumn& column, const TrackerField* field,
                                             const std::string& currentValue, float availWidth, bool tooltipsEnabled,
                                             bool allowEdits, SpreadsheetState& state,
                                             std::vector<PendingFieldEdit>& pendingEdits,
                                             const std::string& dateFormatOption, int thresholdDays,
                                             bool singleClickToEdit) {
    auto renderPlainText = [&](bool disabled) {
        RenderPlainTextCell(app, ticket, column, field, currentValue, availWidth, tooltipsEnabled, dateFormatOption,
                            thresholdDays, disabled);
    };

    // A column can resolve its field to nullptr transiently when the active field catalog is
    // swapped out from under the grid — e.g. rapidly switching the focused pane between panes
    // bound to different backends (Jira<->GitHub): the column keeps its old RenderPlan but the
    // field id no longer exists in the just-activated backend's catalog. Every interactive
    // editor case below dereferences *field (RenderSingleSelectEditor -> TryGetInlineFieldIcon-
    // Texture read field.Id through the null pointer -> AV in std::hash). Fall back to a
    // read-only plain-text cell for that frame instead of crashing; the next frame resolves the
    // field against the new catalog. RenderPlainTextCell is null-field-safe.
    if (field == nullptr) {
        renderPlainText(true);
        return true;
    }

    switch (column.Plan) {
    case TicketGridColumn::RenderPlan::SpecialTimeSpent:
        RenderTimeSpentButton(ticket, currentValue, availWidth, tooltipsEnabled);
        return false;
    case TicketGridColumn::RenderPlan::PlainText:
        renderPlainText(column.CatalogReadOnly);
        return true;
    case TicketGridColumn::RenderPlan::Labels:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        TrackerLabelsEditor::RenderLabelsFieldEditor(
            app, ticket, *field, currentValue,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits, currentValue);
            },
            state, singleClickToEdit);
        return true;
    case TicketGridColumn::RenderPlan::Cascading:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        RenderCascadingSelectEditor(app, ticket, *field, currentValue, pendingEdits, tooltipsEnabled, state,
                                    singleClickToEdit);
        return true;
    case TicketGridColumn::RenderPlan::MultiSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        RenderMultiSelectEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled,
                                singleClickToEdit);
        return true;
    case TicketGridColumn::RenderPlan::SingleSelect:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        RenderSingleSelectEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled,
                                 singleClickToEdit);
        return true;
    case TicketGridColumn::RenderPlan::DateTimeEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        TrackerDateTimeFieldEditor::RenderDateTimeFieldEditor(
            ticket, *field, currentValue, state,
            [&](const std::string& issueId, const TrackerField& fld, const std::vector<std::string>& values) {
                QueueEdit(issueId, fld, values, pendingEdits, currentValue);
            },
            dateFormatOption, thresholdDays, singleClickToEdit);
        return true;
    case TicketGridColumn::RenderPlan::TextEditor:
        if (!allowEdits) {
            renderPlainText(true);
            return true;
        }
        RenderTextEditor(app, ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled, availWidth,
                         singleClickToEdit);
        return true;
    default:
        // The remaining special-column plans get dispatched earlier by
        // TryRenderSpecialColumnPlan and never reach this switch. Falling through
        // to the time-tracking modal preserves the prior unhandled-case behaviour.
        break;
    }
    return false;
}
