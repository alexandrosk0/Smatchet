#pragma once

#include "DictationInsertionRouter.h"
#include "SmatchetLocalization.h"
#include "imgui.h"

#include <cstdarg>
#include <cstddef>

namespace SmatchetLocalizedImGui {

using namespace ::ImGui;

// Floating popouts (Watchers / Votes / Attachment Preview) bypass this wrapper and
// call ::ImGui::Begin directly with NoDocking.
constexpr ImGuiWindowFlags kSmatchetPanelFlags = ImGuiWindowFlags_NoCollapse;

inline bool Begin(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0) {
    return ::ImGui::Begin(SmatchetLocalization::WindowTitleFromSource(name), p_open, flags | kSmatchetPanelFlags);
}

inline bool BeginPopupModal(const char* name, bool* p_open = nullptr, ImGuiWindowFlags flags = 0) {
    return ::ImGui::BeginPopupModal(SmatchetLocalization::WindowTitleFromSource(name), p_open, flags);
}

inline void SetWindowFocus(const char* name) {
    ::ImGui::SetWindowFocus(SmatchetLocalization::WindowTitleFromSource(name));
}

inline void SetWindowFocus() { ::ImGui::SetWindowFocus(); }

inline bool BeginMenu(const char* label, bool enabled = true) {
    return ::ImGui::BeginMenu(SmatchetLocalization::LabelFromSource(label), enabled);
}

inline bool MenuItem(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true) {
    return ::ImGui::MenuItem(SmatchetLocalization::LabelFromSource(label), shortcut, selected, enabled);
}

inline bool MenuItem(const char* label, const char* shortcut, bool* p_selected, bool enabled = true) {
    return ::ImGui::MenuItem(SmatchetLocalization::LabelFromSource(label), shortcut, p_selected, enabled);
}

inline bool BeginTabItem(const char* label, bool* p_open = nullptr, ImGuiTabItemFlags flags = 0) {
    return ::ImGui::BeginTabItem(SmatchetLocalization::LabelFromSource(label), p_open, flags);
}

inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Button(SmatchetLocalization::LabelFromSource(label), size);
}

inline bool SmallButton(const char* label) {
    return ::ImGui::SmallButton(SmatchetLocalization::LabelFromSource(label));
}

inline bool Checkbox(const char* label, bool* v) {
    return ::ImGui::Checkbox(SmatchetLocalization::LabelFromSource(label), v);
}

inline bool RadioButton(const char* label, bool active) {
    return ::ImGui::RadioButton(SmatchetLocalization::LabelFromSource(label), active);
}

inline bool RadioButton(const char* label, int* v, int v_button) {
    return ::ImGui::RadioButton(SmatchetLocalization::LabelFromSource(label), v, v_button);
}

inline bool BeginCombo(const char* label, const char* preview_value, ImGuiComboFlags flags = 0) {
    return ::ImGui::BeginCombo(SmatchetLocalization::LabelFromSource(label), preview_value, flags);
}

inline bool Combo(const char* label, int* current_item, const char* const items[], int items_count,
                  int popup_max_height_in_items = -1) {
    return ::ImGui::Combo(SmatchetLocalization::LabelFromSource(label), current_item, items, items_count,
                          popup_max_height_in_items);
}

inline bool Combo(const char* label, int* current_item, const char* items_separated_by_zeros,
                  int popup_max_height_in_items = -1) {
    return ::ImGui::Combo(SmatchetLocalization::LabelFromSource(label), current_item, items_separated_by_zeros,
                          popup_max_height_in_items);
}

inline bool Combo(const char* label, int* current_item, const char* (*getter)(void* user_data, int idx),
                  void* user_data, int items_count, int popup_max_height_in_items = -1) {
    return ::ImGui::Combo(SmatchetLocalization::LabelFromSource(label), current_item, getter, user_data, items_count,
                          popup_max_height_in_items);
}

// Dictation hook (Phase D): every routed InputText / InputTextMultiline /
// InputTextWithHint call registers its buffer with the dictation router for
// the lifetime of the widget's focus. Registration is idempotent (re-asserting
// each frame is cheap) and the router's stub TU exports the same symbol as a
// no-op shell, so call sites need no SMATCHET_WITH_WHISPER ifdefs. Cursor is
// passed as nullptr — ImGui does not expose a per-widget byte cursor through
// the C++14 surface; insertion lands at end-of-content, which is correct for
// the four target surfaces (AI Assistant input, Command Palette filter,
// long-text editor, focused-InputText catch-all). Tighter splice-at-cursor
// for the multiline editor lands when an ImGuiInputTextCallback wire-up is
// added (Phase E or later).
// Defined out-of-line in SmatchetDictationHook.cpp (debt 2026-05-18) so this hot,
// ~45-includer header carries only the declaration — the body's ImGui item-state
// queries (IsItemActive / IsItemFocused / GetItemID) stay out of every TU that
// merely draws a localized widget. Behaviour is identical to the prior inline body.
void HookDictationOnLastItem(char* buf, std::size_t buf_size);

inline bool InputText(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0,
                      ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr) {
    const bool result =
        ::ImGui::InputText(SmatchetLocalization::LabelFromSource(label), buf, buf_size, flags, callback, user_data);
    HookDictationOnLastItem(buf, buf_size);
    return result;
}

inline bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size = ImVec2(0, 0),
                               ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr,
                               void* user_data = nullptr) {
    const bool result = ::ImGui::InputTextMultiline(SmatchetLocalization::LabelFromSource(label), buf, buf_size, size,
                                                    flags, callback, user_data);
    HookDictationOnLastItem(buf, buf_size);
    return result;
}

inline bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t buf_size,
                              ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr,
                              void* user_data = nullptr) {
    const bool result = ::ImGui::InputTextWithHint(SmatchetLocalization::LabelFromSource(label),
                                                   SmatchetLocalization::TranslateSource(hint), buf, buf_size, flags,
                                                   callback, user_data);
    HookDictationOnLastItem(buf, buf_size);
    return result;
}

inline bool InputInt(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0) {
    return ::ImGui::InputInt(SmatchetLocalization::LabelFromSource(label), v, step, step_fast, flags);
}

inline bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d",
                      ImGuiSliderFlags flags = 0) {
    return ::ImGui::SliderInt(SmatchetLocalization::LabelFromSource(label), v, v_min, v_max,
                              SmatchetLocalization::TranslateSourceAsFormat(format), flags);
}

inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                       const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(SmatchetLocalization::LabelFromSource(label), selected, flags, size);
}

inline bool Selectable(const char* label, bool* p_selected, ImGuiSelectableFlags flags = 0,
                       const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(SmatchetLocalization::LabelFromSource(label), p_selected, flags, size);
}

/** Per-frame / data labels (paths, CLs, usernames, etc.): do not run through LabelFromSource/TranslateSource. */
inline bool SelectableRaw(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0,
                          const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(label, selected, flags, size);
}

inline bool SelectableRaw(const char* label, bool* p_selected, ImGuiSelectableFlags flags = 0,
                          const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(label, p_selected, flags, size);
}

inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0) {
    return ::ImGui::CollapsingHeader(SmatchetLocalization::LabelFromSource(label), flags);
}

inline bool CollapsingHeader(const char* label, bool* p_visible, ImGuiTreeNodeFlags flags = 0) {
    return ::ImGui::CollapsingHeader(SmatchetLocalization::LabelFromSource(label), p_visible, flags);
}

inline void SeparatorText(const char* label) {
    // Same ID rules as Button/Checkbox: LabelFromSource keeps ## / ### suffixes and appends a stable
    // English-derived id when the label has no explicit suffix (TranslateSource alone would change IDs
    // when the UI language changes).
    ::ImGui::SeparatorText(SmatchetLocalization::LabelFromSource(label));
}

inline void TextUnformatted(const char* text, const char* text_end = nullptr) {
    if (text_end) {
        ::ImGui::TextUnformatted(text, text_end);
        return;
    }
    ::ImGui::TextUnformatted(SmatchetLocalization::TranslateSource(text));
}

inline void Text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void TextColored(const ImVec4& col, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextColoredV(col, SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void TextDisabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextDisabledV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void TextWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::TextWrappedV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void BulletText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::BulletTextV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void SetTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::SetTooltipV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

inline void SetItemTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ::ImGui::SetItemTooltipV(SmatchetLocalization::TranslateSourceAsFormat(fmt), args);
    va_end(args);
}

} // namespace SmatchetLocalizedImGui
