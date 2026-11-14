#include "FilterDropdown.h"

#include "FilterDropdownPure.h"
#include "RecentSelections.h"
#include "SmatchetLocalization.h"
#include "TicketFieldEditorDurationPopupPure.h"

#include "imgui.h"
#include "imgui_internal.h" // RenderCheckMark, PushItemFlag, the current window id

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

namespace smatchet {
namespace dropdown {

namespace {

constexpr float kMinWidthEm = 18.0f; // narrowest body, in font sizes: fits the footer hints
constexpr float kSmallTextScale = 0.85f;
constexpr int kMaxSplicedChars = 32;

// Everything one open dropdown remembers between frames. Slots are keyed by the popup window id and
// start fresh whenever that popup (re)appears.
struct BodyState {
    ImGuiID WindowId = 0;
    ImGuiID InputId = 0; // the filter field's id, known after its first draw
    int LastFrame = -1000;
    char Filter[128] = "";
    std::string AppliedFilterLower; // the filter the current highlight was chosen for
    int Highlight = -1;             // row index, -1 = none
    bool NeedDefaultHighlight = true;
    bool UserMovedHighlight = false;     // by key or mouse, since the last default
    int LastRowCount = -1;               // rows on the previous frame
    pure::RowIdentity HighlightIdentity; // what the highlighted row was at the end of the previous frame
    bool FocusInput = false;             // give the filter field keyboard focus this frame
    int FocusRequestFrame = -1000;       // last frame a focus request was made (it lands one frame later)
    bool CaretToEnd = false;             // on the filter's next active frame: caret to the end, no selection
    bool ScrollToHighlight = false;      // bring the highlighted row into view
    bool OpeningEnterHeld = false;       // the Enter that opened the popup is still down: not a pick
    bool RecentsLoaded = false;
    std::vector<std::string> Recents;  // newest first, for Options::RecentsScope
    std::vector<pure::ItemView> Views; // scratch, reused across frames
    std::vector<pure::Row> Rows;       // scratch, reused across frames
    std::string CreateLabel;           // scratch: the 'Add "<text>"' row text
};

BodyState& StateForWindow(ImGuiID windowId) {
    // Dropdowns do not nest more than a couple deep (a picker inside a modal inside a popup); the slot
    // used longest ago is recycled.
    static BodyState slots[4];
    BodyState* oldest = &slots[0];
    for (BodyState& slot : slots) {
        if (slot.WindowId == windowId) {
            return slot;
        }
        if (slot.LastFrame < oldest->LastFrame) {
            oldest = &slot;
        }
    }
    *oldest = BodyState();
    oldest->WindowId = windowId;
    return *oldest;
}

void ResetForOpen(BodyState& st) {
    st.Filter[0] = '\0';
    st.AppliedFilterLower.clear();
    st.Highlight = -1;
    st.NeedDefaultHighlight = true;
    st.UserMovedHighlight = false;
    st.LastRowCount = -1;
    st.HighlightIdentity = pure::RowIdentity();
    st.FocusInput = true;
    st.FocusRequestFrame = -1000;
    st.CaretToEnd = false;
    st.ScrollToHighlight = false;
    st.OpeningEnterHeld = false;
    st.RecentsLoaded = false;
    st.Recents.clear();
}

// A refocus through SetKeyboardFocusHere() activates the field with all its text selected, so the next
// typed character would replace the filter. This runs on the field's active frames and undoes that once.
int FilterInputCallback(ImGuiInputTextCallbackData* data) {
    BodyState* st = static_cast<BodyState*>(data->UserData);
    if (st != nullptr && st->CaretToEnd && data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        data->CursorPos = data->BufTextLen;
        data->SelectionStart = data->CursorPos;
        data->SelectionEnd = data->CursorPos;
        st->CaretToEnd = false;
    }
    return 0; // Up / Down arrive here too (CallbackHistory keeps them away from nav); DrawBody polls them.
}

void DrawMagnifier(ImDrawList* dl, ImVec2 center, float fontSize, ImU32 col) {
    const float r = fontSize * 0.24f;
    const ImVec2 c(center.x - r * 0.3f, center.y - r * 0.3f);
    dl->AddCircle(c, r, col, 0, 1.5f);
    dl->AddLine(ImVec2(c.x + r * 0.72f, c.y + r * 0.72f), ImVec2(c.x + r * 1.75f, c.y + r * 1.75f), col, 1.5f);
}

void DrawCross(ImDrawList* dl, ImVec2 center, float half, ImU32 col) {
    dl->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), col, 1.5f);
    dl->AddLine(ImVec2(center.x - half, center.y + half), ImVec2(center.x + half, center.y - half), col, 1.5f);
}

void DrawPlus(ImDrawList* dl, ImVec2 center, float half, ImU32 col) {
    dl->AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x + half, center.y), col, 1.5f);
    dl->AddLine(ImVec2(center.x, center.y - half), ImVec2(center.x, center.y + half), col, 1.5f);
}

// Characters typed while the popup has focus but the filter field does not (just after a row click, a
// scrollbar drag, or on the frame focus is on its way back) would be dropped: focus requested now only
// lands next frame, and the field discards the queue on the frame it activates. So they are put into the
// buffer here. Returns true if at least one landed.
bool SpliceQueuedCharsIntoFilter(BodyState& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.InputQueueCharacters.Size <= 0 || (io.KeyCtrl && !io.KeyAlt)) {
        return false;
    }
    // A filter never starts with a space: the Space that opened a keyboard-focused combo is not filter text.
    int first = 0;
    while (st.Filter[0] == '\0' && first < io.InputQueueCharacters.Size && io.InputQueueCharacters[first] == ' ') {
        ++first;
    }
    unsigned int queued[kMaxSplicedChars];
    const int n = (std::min)(io.InputQueueCharacters.Size - first, kMaxSplicedChars);
    for (int i = 0; i < n; ++i) {
        queued[i] = static_cast<unsigned int>(io.InputQueueCharacters[first + i]);
    }
    if (!TicketFieldEditorDurationPopupPure::SpliceTypedCharsIntoBuf(queued, n, st.Filter, sizeof(st.Filter))) {
        return false;
    }
    io.InputQueueCharacters.resize(0); // consumed: nothing else may insert them a second time
    return true;
}

// Empties the filter from code. An active text field keeps its own copy of the text and would write it
// back over the buffer, so the field is released first; focus returns to it on the next frame.
void ClearFilter(BodyState& st) {
    if (st.InputId != 0 && ImGui::GetActiveID() == st.InputId) {
        ImGui::ClearActiveID();
    }
    st.Filter[0] = '\0';
    st.FocusInput = true;
}

// The pinned filter field: magnifier, text input, clear button. Returns true when Enter was pressed in it.
bool DrawSearchRow(BodyState& st, const Options& options, float width, int frame, bool popupFocused) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fontSize = ImGui::GetFontSize();
    const float frameH = ImGui::GetFrameHeight();
    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const float iconW = frameH;

    const ImGuiID inputId = ImGui::GetID("##dropdown_filter");
    st.InputId = inputId;
    const bool inputActive = ImGui::GetActiveID() == inputId;
    const bool idle = ImGui::GetActiveID() == 0 && !ImGui::IsAnyMouseDown();
    bool requestFocus = false;
    if (!inputActive && (st.FocusInput || popupFocused)) {
        SpliceQueuedCharsIntoFilter(st); // also while a scrollbar is being dragged
        if (st.FocusInput || idle) {
            // A request lands on the next frame; asking again on that frame would only repeat it.
            requestFocus = st.FocusInput || st.FocusRequestFrame < frame - 1;
            st.FocusInput = false;
            st.CaretToEnd = true;
        }
    }

    const bool hasText = st.Filter[0] != '\0';
    const float clearW = hasText ? frameH : 0.0f;
    dl->AddRectFilled(rowMin, ImVec2(rowMin.x + width, rowMin.y + frameH), ImGui::GetColorU32(ImGuiCol_FrameBg),
                      style.FrameRounding);
    DrawMagnifier(dl, ImVec2(rowMin.x + iconW * 0.5f, rowMin.y + frameH * 0.5f), fontSize,
                  ImGui::GetColorU32(ImGuiCol_TextDisabled));

    ImGui::SetCursorScreenPos(ImVec2(rowMin.x + iconW, rowMin.y));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImU32 focusRingCol = ImGui::GetColorU32(ImGuiCol_NavCursor);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // drawn below, around the whole row
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, style.FramePadding.y));
    ImGui::SetNextItemWidth((std::max)(1.0f, width - iconW - clearW));
    if (requestFocus) {
        ImGui::SetKeyboardFocusHere();
        st.FocusRequestFrame = frame;
    }
    const char* hint =
        options.Hint != nullptr ? options.Hint : SmatchetLocalization::T("dropdown.filter_hint", "Filter");
    bool enterPressed = ImGui::InputTextWithHint(
        "##dropdown_filter", hint, st.Filter, sizeof(st.Filter),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_CallbackHistory,
        &FilterInputCallback, &st);
    if (!inputActive && popupFocused && idle &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        enterPressed = true; // Enter in the frame or two before focus is back in the field
    }
    const bool fieldActive = ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    if (fieldActive) {
        dl->AddRect(rowMin, ImVec2(rowMin.x + width, rowMin.y + frameH), focusRingCol, style.FrameRounding);
    }

    if (hasText) {
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 clearMin = ImGui::GetCursorScreenPos();
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_NoTabStop, true);
        if (ImGui::InvisibleButton("##dropdown_clear", ImVec2(clearW, frameH))) {
            ClearFilter(st);
        }
        const bool clearHovered = ImGui::IsItemHovered();
        ImGui::PopItemFlag();
        DrawCross(dl, ImVec2(clearMin.x + clearW * 0.5f, clearMin.y + frameH * 0.5f), fontSize * 0.2f,
                  ImGui::GetColorU32(clearHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    }
    return enterPressed;
}

Event ActivateRow(const BodyState& st, const pure::Row& row, const Item* items, int count) {
    Event ev;
    if (row.Kind == pure::RowKind::Clear) {
        ev.Kind = EventKind::Cleared;
    } else if (row.Kind == pure::RowKind::Create) {
        ev.Kind = EventKind::Created;
        ev.Id = pure::TrimFilter(st.Filter);
    } else if (row.Kind == pure::RowKind::Item) {
        if (row.ItemIndex >= 0 && row.ItemIndex < count) {
            if (!items[row.ItemIndex].Disabled) {
                ev.Kind = EventKind::Picked;
                ev.ItemIndex = row.ItemIndex;
                ev.Id = items[row.ItemIndex].Id != nullptr ? items[row.ItemIndex].Id : "";
            }
        } else if (row.RecentIndex >= 0 && row.RecentIndex < static_cast<int>(st.Recents.size())) {
            ev.Kind = EventKind::Picked;
            ev.Id = st.Recents[static_cast<std::size_t>(row.RecentIndex)];
        }
    }
    return ev;
}

void DrawSectionHeader(const pure::Row& row, float rowW, float rowH) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(rowW, rowH));
    const bool isAll = row.Kind == pure::RowKind::HeaderAll;
    if (isAll) {
        dl->AddLine(ImVec2(pos.x + style.FramePadding.x, pos.y + 0.5f),
                    ImVec2(pos.x + rowW - style.FramePadding.x, pos.y + 0.5f), ImGui::GetColorU32(ImGuiCol_Separator));
    }
    const char* title = isAll ? SmatchetLocalization::T("dropdown.section.all", "All")
                              : SmatchetLocalization::T("dropdown.section.recent", "Recent");
    const float size = ImGui::GetFontSize() * kSmallTextScale;
    dl->AddText(ImGui::GetFont(), size, ImVec2(pos.x + style.FramePadding.x, pos.y + (rowH - size) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), title);
}

// What one row shows.
struct RowContent {
    const Item* Option = nullptr; // the caller's option behind the row, when it is one
    const char* Label = "";
    const char* Secondary = nullptr;
};

RowContent ResolveRowContent(BodyState& st, const pure::Row& row, const Item* items, int count,
                             const Options& options) {
    RowContent rc;
    if (row.Kind == pure::RowKind::Clear) {
        rc.Label = options.ClearLabel != nullptr ? options.ClearLabel : "";
    } else if (row.Kind == pure::RowKind::Create) {
        // Format returns a pointer into its own storage: copy it before anything else formats.
        st.CreateLabel =
            SmatchetLocalization::Format("dropdown.add_fmt", "Add \"%s\"", pure::TrimFilter(st.Filter).c_str());
        rc.Label = st.CreateLabel.c_str();
    } else if (row.ItemIndex >= 0 && row.ItemIndex < count) {
        rc.Option = &items[row.ItemIndex];
        rc.Label = rc.Option->Label != nullptr ? rc.Option->Label : "";
        rc.Secondary = rc.Option->Secondary;
    } else if (row.RecentIndex >= 0 && row.RecentIndex < static_cast<int>(st.Recents.size())) {
        rc.Label = st.Recents[static_cast<std::size_t>(row.RecentIndex)].c_str();
    }
    return rc;
}

// The mark left of the text: a cross (clear), a plus (create), a tick box (multi-select), or a check
// mark on the current value.
void DrawRowMark(ImDrawList* dl, const pure::Row& row, const RowContent& rc, bool multiSelect, ImVec2 markPos,
                 float markSz, ImU32 textCol) {
    const ImVec2 markCenter(markPos.x + markSz * 0.5f, markPos.y + markSz * 0.5f);
    if (row.Kind == pure::RowKind::Clear) {
        DrawCross(dl, markCenter, markSz * 0.28f, textCol);
        return;
    }
    if (row.Kind == pure::RowKind::Create) {
        DrawPlus(dl, markCenter, markSz * 0.32f, textCol);
        return;
    }
    const bool checked = rc.Option != nullptr && rc.Option->Checked;
    if (!multiSelect) {
        if (checked) {
            ImGui::RenderCheckMark(dl, markPos, textCol, markSz);
        }
        return;
    }
    const ImVec2 boxMax(markPos.x + markSz, markPos.y + markSz);
    dl->AddRectFilled(markPos, boxMax, ImGui::GetColorU32(ImGuiCol_FrameBg), 2.0f);
    dl->AddRect(markPos, boxMax, ImGui::GetColorU32(ImGuiCol_Border), 2.0f);
    if (checked) {
        const float inset = markSz * 0.18f;
        ImGui::RenderCheckMark(dl, ImVec2(markPos.x + inset, markPos.y + inset), ImGui::GetColorU32(ImGuiCol_CheckMark),
                               markSz - inset * 2.0f);
    }
}

// What a row could not show in full.
struct RowTextFit {
    bool LabelCut = false;        // the label ran out of space
    bool SecondaryHidden = false; // the secondary text did not fit beside the label
};

// The label, clipped to its space with the part that matches the filter highlighted, and the dimmed
// secondary text on the right when both fit.
RowTextFit DrawRowText(ImDrawList* dl, const BodyState& st, const pure::Row& row, const RowContent& rc, ImVec2 textPos,
                       float textMaxX, float rowTop, float rowH, ImU32 textCol) {
    const float fontSize = ImGui::GetFontSize();
    const char* label = rc.Label;
    const float labelW = ImGui::CalcTextSize(label, nullptr, false).x;
    RowTextFit fit;
    if (rc.Secondary != nullptr && rc.Secondary[0] != '\0') {
        const float secondaryW = ImGui::CalcTextSize(rc.Secondary, nullptr, false).x;
        fit.SecondaryHidden = textPos.x + labelW + fontSize + secondaryW > textMaxX;
        if (!fit.SecondaryHidden) {
            dl->AddText(ImVec2(textMaxX - secondaryW, textPos.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                        rc.Secondary);
            textMaxX -= secondaryW + fontSize * 0.5f;
        }
    }
    dl->PushClipRect(ImVec2(textPos.x, rowTop), ImVec2(textMaxX, rowTop + rowH), true);
    const bool filtered = row.Kind == pure::RowKind::Item && !st.AppliedFilterLower.empty();
    const std::size_t at = filtered ? pure::FindCaseInsensitive(label, st.AppliedFilterLower) : std::string::npos;
    if (at != std::string::npos) {
        // Exact glyph advances: the usual text measure rounds every width up, which would drift the
        // box to the right of the letters it marks.
        ImFont* font = ImGui::GetFont();
        const char* matchBegin = label + at;
        const char* matchEnd = matchBegin + st.AppliedFilterLower.size();
        const float x0 = textPos.x + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, label, matchBegin).x;
        const float x1 = x0 + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, matchBegin, matchEnd).x;
        dl->AddRectFilled(ImVec2(x0 - 1.0f, textPos.y), ImVec2(x1 + 1.0f, textPos.y + fontSize),
                          ImGui::GetColorU32(ImGuiCol_TextSelectedBg), 2.0f);
    }
    dl->AddText(textPos, textCol, label);
    dl->PopClipRect();
    fit.LabelCut = textPos.x + labelW > textMaxX;
    return fit;
}

// An option's own tooltip, else whatever the row could not show: the full label, the secondary text.
void ShowRowTooltip(const RowContent& rc, const RowTextFit& fit) {
    if (rc.Option != nullptr && rc.Option->Tooltip != nullptr && rc.Option->Tooltip[0] != '\0') {
        ImGui::SetTooltip("%s", rc.Option->Tooltip);
    } else if (fit.LabelCut && fit.SecondaryHidden) {
        ImGui::SetTooltip("%s\n%s", rc.Label, rc.Secondary);
    } else if (fit.SecondaryHidden) {
        ImGui::SetTooltip("%s", rc.Secondary);
    } else if (fit.LabelCut) {
        ImGui::SetTooltip("%s", rc.Label);
    }
}

// One option / clear / create row. Sets `ev` when the row is clicked.
void DrawOptionRow(BodyState& st, int rowIndex, const pure::Row& row, const Item* items, int count,
                   const Options& options, float rowW, float rowH, Event& ev) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const RowContent rc = ResolveRowContent(st, row, items, count, options);
    const bool disabled = rc.Option != nullptr && rc.Option->Disabled;

    ImGui::PushID(rowIndex);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true); // the highlight is ours; keyboard focus stays in the filter
    if (disabled) {
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool wantTooltip = hovered && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    if (disabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopItemFlag();
    ImGui::PopID();

    // The mouse takes the highlight only when it moves, scrolls or clicks, so a pointer resting over the
    // list does not fight the arrow keys.
    const bool mouseActed = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f || pressed;
    if (hovered && mouseActed) {
        st.Highlight = rowIndex;
        st.UserMovedHighlight = true;
    }
    if (rowIndex == st.Highlight) {
        dl->AddRectFilled(ImVec2(pos.x + 1.0f, pos.y), ImVec2(pos.x + rowW - 1.0f, pos.y + rowH),
                          ImGui::GetColorU32(held ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered),
                          (std::max)(style.FrameRounding, 3.0f));
    }

    const bool dimmed = disabled || row.Kind == pure::RowKind::Clear;
    const ImU32 textCol = ImGui::GetColorU32(dimmed ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    const float padX = style.FramePadding.x;
    const float markSz = fontSize * 0.8f;
    const ImVec2 markPos(pos.x + padX, pos.y + (rowH - markSz) * 0.5f);
    DrawRowMark(dl, row, rc, options.MultiSelect, markPos, markSz, textCol);

    // Text is drawn on whole pixels; the match highlight starts from the same origin.
    const ImVec2 textPos(static_cast<float>(static_cast<int>(markPos.x + markSz + padX)),
                         static_cast<float>(static_cast<int>(pos.y + (rowH - fontSize) * 0.5f)));
    const RowTextFit fit = DrawRowText(dl, st, row, rc, textPos, pos.x + rowW - padX, pos.y, rowH, textCol);
    if (wantTooltip) {
        ShowRowTooltip(rc, fit);
    }
    if (pressed) {
        ev = ActivateRow(st, row, items, count);
    }
}

void DrawEmptyState(const Options& options, bool filtered, float width) {
    if (filtered) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("dropdown.no_matches", "No matches"));
        return;
    }
    const char* text =
        options.EmptyText != nullptr ? options.EmptyText : SmatchetLocalization::T("dropdown.no_options", "No options");
    if (text[0] != '\0') {
        ImGui::TextDisabled("%s", text);
    }
    if (options.EmptyHint != nullptr && options.EmptyHint[0] != '\0') {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        ImGui::TextDisabled("%s", options.EmptyHint);
        ImGui::PopTextWrapPos();
    }
}

// Draws a small "key cap" at `x` and returns the x just past it. `text` null draws the up / down arrows.
float DrawKeyCap(ImDrawList* dl, float x, float y, float h, const char* text, ImU32 col) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * kSmallTextScale;
    const float pad = size * 0.35f;
    const float innerW = text != nullptr ? font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x : size * 1.3f;
    const ImVec2 capMin(x, y);
    const ImVec2 capMax(x + innerW + pad * 2.0f, y + h);
    dl->AddRect(capMin, capMax, col, 3.0f);
    if (text != nullptr) {
        dl->AddText(font, size, ImVec2(x + pad, y + (h - size) * 0.5f), col, text);
    } else {
        const float cy = y + h * 0.5f;
        const float t = size * 0.22f;
        const float ax = x + pad + size * 0.3f;
        const float bx = x + pad + size * 1.0f;
        dl->AddTriangleFilled(ImVec2(ax - t, cy + t * 0.7f), ImVec2(ax + t, cy + t * 0.7f), ImVec2(ax, cy - t), col);
        dl->AddTriangleFilled(ImVec2(bx - t, cy - t * 0.7f), ImVec2(bx + t, cy - t * 0.7f), ImVec2(bx, cy + t), col);
    }
    return capMax.x;
}

void DrawFooter(const Options& options, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * kSmallTextScale;
    const float h = size + 4.0f;
    ImGui::Separator();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, h));
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    struct Hint {
        const char* Key; // null = arrows
        const char* Text;
    };
    const Hint hints[] = {
        {nullptr, SmatchetLocalization::T("dropdown.hint.navigate", "Navigate")},
        {"Enter", options.MultiSelect ? SmatchetLocalization::T("dropdown.hint.toggle", "Toggle")
                                      : SmatchetLocalization::T("dropdown.hint.select", "Select")},
        {"Esc", SmatchetLocalization::T("dropdown.hint.close", "Close")},
    };
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + h), true);
    float x = pos.x;
    for (const Hint& hint : hints) {
        x = DrawKeyCap(dl, x, pos.y, h, hint.Key, col) + size * 0.4f;
        dl->AddText(font, size, ImVec2(x, pos.y + (h - size) * 0.5f), col, hint.Text);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, hint.Text).x + size * 1.1f;
    }
    dl->PopClipRect();
}

// Rebuilds this frame's visible rows from the caller's options and the filter text.
void BuildRowsForFrame(BodyState& st, const Item* items, int count, const Options& options,
                       const std::string& filterLower, bool hasScope) {
    st.Views.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        pure::ItemView& view = st.Views[static_cast<std::size_t>(i)];
        view.Id = items[i].Id != nullptr ? items[i].Id : "";
        view.Label = items[i].Label != nullptr ? items[i].Label : "";
        view.Secondary = items[i].Secondary;
        view.Checked = items[i].Checked;
        view.Disabled = items[i].Disabled;
    }
    pure::ModelInput in;
    in.Items = st.Views.data();
    in.Count = count;
    in.FilterLower = filterLower;
    in.Recents = hasScope ? &st.Recents : nullptr;
    in.RecentsMinOptions = options.RecentsMinOptions;
    in.RecentsShowUnknown = options.RecentsShowUnknown;
    in.KeepCheckedVisible = options.KeepCheckedVisible;
    in.AllowCreate = options.AllowCreate;
    in.ShowClear = options.ClearLabel != nullptr;
    pure::BuildRows(in, st.Rows);
}

// Keeps the highlight on the right row. It gets a fresh default when the popup opens, when the filter
// changes, and, until the user has moved it, whenever the list changes size (options arriving under an
// open popup). Otherwise it follows its row by identity, so a rebuilt list never leaves it on a
// different option. Then the arrow and page keys move it.
void UpdateHighlight(BodyState& st, int count, const Options& options, const std::string& filterLower,
                     bool popupFocused, int maxRows) {
    const int rowCount = static_cast<int>(st.Rows.size());
    const pure::ItemView* views = st.Views.data();
    if (!st.NeedDefaultHighlight) {
        if (!st.UserMovedHighlight && rowCount != st.LastRowCount) {
            st.NeedDefaultHighlight = true;
        } else if (st.HighlightIdentity.Valid) {
            st.Highlight =
                pure::ResolveHighlight(st.Rows, views, count, &st.Recents, st.HighlightIdentity, st.Highlight);
            st.NeedDefaultHighlight = st.Highlight < 0; // its row is gone
        } else {
            st.NeedDefaultHighlight = true; // nothing was highlighted: take one as soon as a row can be
        }
    }
    if (st.NeedDefaultHighlight) {
        const bool preferChecked = filterLower.empty() && !options.MultiSelect;
        st.Highlight = pure::DefaultHighlight(st.Rows, views, count, preferChecked, filterLower);
        st.NeedDefaultHighlight = false;
        st.UserMovedHighlight = false;
        st.ScrollToHighlight = st.Highlight >= 0;
    }
    st.LastRowCount = rowCount;
    if (!popupFocused) {
        return;
    }
    int move = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        move += 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        move -= 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) {
        move += maxRows;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) {
        move -= maxRows;
    }
    if (move != 0) {
        st.Highlight = pure::MoveHighlight(st.Rows, views, count, st.Highlight, move);
        // A key press with no row to move to is not a move: the default still applies when rows arrive.
        if (st.Highlight >= 0) {
            st.UserMovedHighlight = true;
            st.ScrollToHighlight = true;
        }
    }
}

// The scrolling list of rows. Sets `ev` when a row is clicked.
void DrawRowList(BodyState& st, const Item* items, int count, const Options& options, float width, int maxRows,
                 bool appearing, Event& ev) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const int rowCount = static_cast<int>(st.Rows.size());
    const float rowH = static_cast<float>(static_cast<int>(ImGui::GetFontSize() + style.FramePadding.y * 2.0f + 2.0f));
    const float listH = rowH * static_cast<float>((std::min)(rowCount, maxRows));
    if (ImGui::BeginChild("##dropdown_rows", ImVec2(width, listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
        if (appearing) {
            ImGui::SetScrollY(0.0f);
        }
        // Dragging the scrollbar is the user's own positioning: options arriving meanwhile must not
        // re-default the highlight and scroll the list back.
        if (ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(ImGui::GetCurrentWindow(), ImGuiAxis_Y)) {
            st.UserMovedHighlight = true;
        }
        if (st.ScrollToHighlight) {
            const float from = appearing ? 0.0f : ImGui::GetScrollY();
            ImGui::SetScrollY(pure::ScrollToRevealRow(st.Highlight, rowH, listH, from));
            st.ScrollToHighlight = false;
        }
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGuiListClipper clipper;
        clipper.Begin(rowCount, rowH);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const pure::Row& row = st.Rows[static_cast<std::size_t>(i)];
                if (row.Kind == pure::RowKind::HeaderRecent || row.Kind == pure::RowKind::HeaderAll) {
                    DrawSectionHeader(row, rowW, rowH);
                } else {
                    DrawOptionRow(st, i, row, items, count, options, rowW, rowH, ev);
                }
            }
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
}

// Follows up on what the user did: remembers the pick and decides whether the popup stays. Returns true
// when it should close.
bool FinishEvent(BodyState& st, const Item* items, const Options& options, bool hasScope, const Event& ev) {
    if (hasScope && !ev.Id.empty()) {
        // Multi-select records a value when it is ticked on; unticking records nothing.
        const bool tickedOff = options.MultiSelect && ev.ItemIndex >= 0 && items[ev.ItemIndex].Checked;
        if (ev.Kind == EventKind::Created || (ev.Kind == EventKind::Picked && !tickedOff)) {
            smatchet::recents::RecordAndPersist(options.RecentsScope, ev.Id);
        }
    }
    if (ev.Kind == EventKind::Created) {
        ClearFilter(st);
    }
    if (!options.MultiSelect) {
        return true;
    }
    st.FocusInput = true; // Enter released the field; keep typing after a toggle
    return false;
}

} // namespace

Event DrawBody(const Item* items, int count, const Options& options) {
    Event ev;
    if (items == nullptr || count < 0) {
        count = 0;
    }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const int frame = ImGui::GetFrameCount();
    BodyState& st = StateForWindow(window->ID);
    const bool appearing = ImGui::IsWindowAppearing() || st.LastFrame < frame - 1;
    if (appearing) {
        ResetForOpen(st);
    }
    st.LastFrame = frame;

    const float width = (std::max)(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * kMinWidthEm);
    const bool popupFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    const int maxRows = (std::max)(1, options.MaxVisibleRows);
    const bool hasScope = options.RecentsScope != nullptr && options.RecentsScope[0] != '\0';
    if (hasScope && !st.RecentsLoaded) {
        st.Recents = smatchet::recents::SharedGet(options.RecentsScope); // memory only
        st.RecentsLoaded = true;
    }

    // Enter on a keyboard-focused combo opens this popup in the same frame, with the key still down.
    // That press, and its key repeat for as long as it is held, opens the popup and nothing more.
    const bool enterDown = ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter);
    st.OpeningEnterHeld = enterDown && (appearing || st.OpeningEnterHeld);
    const bool enterPressed = DrawSearchRow(st, options, width, frame, popupFocused) && !st.OpeningEnterHeld;
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.25f));

    const std::string filterLower = pure::NormalizeFilter(st.Filter);
    if (filterLower != st.AppliedFilterLower) {
        st.AppliedFilterLower = filterLower;
        st.NeedDefaultHighlight = true;
    }
    BuildRowsForFrame(st, items, count, options, filterLower, hasScope);
    UpdateHighlight(st, count, options, filterLower, popupFocused, maxRows);

    const int rowCount = static_cast<int>(st.Rows.size());
    if (rowCount > 0) {
        DrawRowList(st, items, count, options, width, maxRows, appearing, ev);
    }
    if (!pure::HasOptionRows(st.Rows)) {
        DrawEmptyState(options, !filterLower.empty(), width); // also under a lone clear row
    }
    if (enterPressed && ev.Kind == EventKind::None && st.Highlight >= 0 && st.Highlight < rowCount) {
        ev = ActivateRow(st, st.Rows[static_cast<std::size_t>(st.Highlight)], items, count);
    }
    pure::IdentityOfRow(st.Rows, st.Views.data(), count, &st.Recents, st.Highlight, st.HighlightIdentity);
    if (options.ShowFooter) {
        DrawFooter(options, width);
    }

    bool closePopup = popupFocused && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ev.Kind != EventKind::None && FinishEvent(st, items, options, hasScope, ev)) {
        closePopup = true;
    }
    if (closePopup) {
        ImGui::CloseCurrentPopup();
    }
    return ev;
}

Event DrawBody(const std::vector<Item>& items, const Options& options) {
    return DrawBody(items.data(), static_cast<int>(items.size()), options);
}

Options SingleSelect(const std::string& recentsScope) {
    Options options;
    options.RecentsScope = recentsScope.c_str();
    return options;
}

Options TickBoxes(const std::string& recentsScope, const char* clearAllLabel) {
    Options options;
    options.MultiSelect = true;
    options.KeepCheckedVisible = true;
    options.ClearLabel = clearAllLabel;
    options.RecentsScope = recentsScope.c_str();
    return options;
}

} // namespace dropdown
} // namespace smatchet
