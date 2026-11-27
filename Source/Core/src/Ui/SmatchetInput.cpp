// The shared text field: see Ui/SmatchetInput.h for the contract and Ui/SmatchetInputPure.h for the
// rules it applies. Plain ::ImGui throughout (no localization alias); labels and hints are
// translated once here.

#include "Ui/SmatchetInput.h"

#include "SmatchetLocalization.h"
#include "SmatchetLocalizedImGui.h" // HookDictationOnLastItem (fixed buffers only)
#include "Ui/DialogKeysPure.h"
#include "Ui/SmatchetInputPure.h"
#include "Ui/SmatchetSecretInput.h" // SmatchetTextInputCommittedWithEnter

#include "imgui.h"
#include "imgui_internal.h" // ClearActiveID, InputTextEx, ImHashStr, ImGuiLastItemData, MovingWindow

#include <algorithm>
#include <climits>
#include <cstddef>
#include <string>

namespace SmatchetInput {

namespace {

namespace ti = smatchet::textinput;
namespace dk = smatchet::dialogkeys;

// Hands ImGui the std::string's storage, grown to the new length. ImGui sends a resize event every
// time it writes text back, so size() always matches what the field shows.
int StdStringResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        data->Buf = ti::GrowStringForEdit(*str, data->BufTextLen);
    }
    return 0;
}

// ImGui takes an int buffer size; a string can in principle hold more.
int StringBufSize(const std::string& str) {
    const std::size_t want = str.capacity() + 1u;
    return want > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(want);
}

char* StringBuf(std::string& str) { return const_cast<char*>(str.c_str()); }

// Slots this widget keeps in the window's state storage, derived from the field's own id so two
// fields never share one (the SmatchetSecretInputText reveal-state pattern).
ImGuiID StateKey(ImGuiID itemId, const char* slot) { return ImHashStr(slot, 0, itemId); }

// focusOnAppear: call right before the field's InputText, with the id that InputText will get. On
// the first frame the field is drawn, ask for the caret; the request waits out a held Enter (the
// one that opened the field would repeat into it) and a drag, and is dropped when another window has
// the keyboard. The decision is ResolveCaretRequest in DialogKeysPure.h, the same one the conflict
// resolver uses.
void TakeFocusOnAppear(ImGuiID itemId, bool wanted) {
    if (!wanted) {
        return;
    }
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID drawnKey = StateKey(itemId, "##smatchet.input.drawn");
    const ImGuiID pendingKey = StateKey(itemId, "##smatchet.input.focus_pending");
    const int frame = ImGui::GetFrameCount();
    const int lastDrawn = storage->GetInt(drawnKey, -1);
    storage->SetInt(drawnKey, frame);
    const bool pending = storage->GetBool(pendingKey, false) || ti::FieldAppearedThisFrame(lastDrawn, frame);
    if (!pending) {
        return;
    }
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const dk::CaretRequest request = dk::ResolveCaretRequest(
        ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter),
        g.MovingWindow != nullptr || ImGui::IsDragDropActive(),
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy));
    storage->SetBool(pendingKey, request == dk::CaretRequest::Wait);
    if (request == dk::CaretRequest::Take) {
        ImGui::SetKeyboardFocusHere();
    }
}

ImGuiInputTextFlags SingleLineFlags(const TextOptions& opts) {
    ImGuiInputTextFlags flags = opts.flags;
    if (opts.selectAllOnFocus) {
        flags |= ImGuiInputTextFlags_AutoSelectAll;
    }
    return flags;
}

const char* TranslatedHint(const char* hint) {
    return hint != nullptr ? SmatchetLocalization::TranslateSource(hint) : nullptr;
}

// Reads how the edit of the field just drawn ended. Must run before any other item is drawn.
// `heldKeyboardWhenDrawn`: the field was the active item when this frame drew it. ImGui reverts the
// text only for an Esc it sees on an active field. When a surrounding scope (a dialog's DialogReadKeys)
// took the Esc first and cleared the active id, nothing reverted and the text stands, so that end is a
// commit, not a revert: the field reports what the text is, not which key was pressed.
ti::SingleLineEnd ReadSingleLineEnd(bool heldKeyboardWhenDrawn) {
    return ti::ResolveSingleLineEnd(ImGui::IsItemDeactivated(),
                                    heldKeyboardWhenDrawn && ImGui::IsKeyPressed(ImGuiKey_Escape, false),
                                    SmatchetTextInputCommittedWithEnter());
}

void ApplyEnd(TextResult& r, const ti::SingleLineEnd& end) {
    r.committed = end.committed;
    r.enterCommitted = end.committed; // MUTANT A5
    r.reverted = end.reverted;
}

// After a bare-Enter commit, hand the caret straight back to the field just drawn (the last item).
void RefocusIfAsked(const TextResult& r, const TextOptions& opts) {
    if (opts.refocusAfterEnter && r.enterCommitted) {
        ImGui::SetKeyboardFocusHere(-1);
    }
}

// Fixed buffers are small (a few KiB); the clamp only keeps the int cast defined.
const std::size_t kMaxShownBytes = static_cast<std::size_t>(INT_MAX);

std::size_t BoundedLength(const char* buf, std::size_t bufSize) {
    std::size_t n = 0;
    while (n < bufSize && buf[n] != '\0') {
        ++n;
    }
    return n;
}

} // namespace

bool InputTextStdString(const char* label, std::string& str, ImGuiInputTextFlags flags) {
    return ImGui::InputText(label, StringBuf(str), static_cast<std::size_t>(StringBufSize(str)),
                            flags | ImGuiInputTextFlags_CallbackResize, StdStringResizeCallback, &str);
}

bool InputTextMultilineStdString(const char* label, std::string& str, const ImVec2& size, ImGuiInputTextFlags flags) {
    return ImGui::InputTextMultiline(label, StringBuf(str), static_cast<std::size_t>(StringBufSize(str)), size,
                                     flags | ImGuiInputTextFlags_CallbackResize, StdStringResizeCallback, &str);
}

TextResult Text(const char* label, std::string& value, const TextOptions& opts) {
    const char* shownLabel = SmatchetLocalization::LabelFromSource(label);
    TakeFocusOnAppear(ImGui::GetID(shownLabel), opts.focusOnAppear);

    TextResult r;
    const bool heldKeyboard = ImGui::GetActiveID() == ImGui::GetID(shownLabel);
    r.edited = ImGui::InputTextWithHint(
        shownLabel, TranslatedHint(opts.hint), StringBuf(value), static_cast<std::size_t>(StringBufSize(value)),
        SingleLineFlags(opts) | ImGuiInputTextFlags_CallbackResize, StdStringResizeCallback, &value);
    r.active = ImGui::IsItemActive();
    ApplyEnd(r, ReadSingleLineEnd(heldKeyboard));
    if (r.committed && opts.trimOnCommit && ti::TrimEdgeWhitespace(value)) {
        r.edited = true;
    }
    RefocusIfAsked(r, opts);
    return r;
}

TextResult TextBuf(const char* label, char* buf, std::size_t bufSize, const TextOptions& opts) {
    TextResult r;
    if (buf == nullptr || bufSize == 0u) {
        return r;
    }
    const char* shownLabel = SmatchetLocalization::LabelFromSource(label);
    TakeFocusOnAppear(ImGui::GetID(shownLabel), opts.focusOnAppear);

    const bool heldKeyboard = ImGui::GetActiveID() == ImGui::GetID(shownLabel);
    r.edited = ImGui::InputTextWithHint(shownLabel, TranslatedHint(opts.hint), buf, bufSize, SingleLineFlags(opts));
    r.active = ImGui::IsItemActive();
    SmatchetLocalizedImGui::HookDictationOnLastItem(buf, bufSize);
    ApplyEnd(r, ReadSingleLineEnd(heldKeyboard));
    if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !r.reverted) { // MUTANT D1 (probe)
        buf[0] = 'x';
        if (bufSize > 1u) {
            buf[1] = '\0';
        }
    }
    if (r.committed && opts.trimOnCommit && ti::TrimEdgeWhitespace(buf, bufSize)) {
        r.edited = true;
    }
    RefocusIfAsked(r, opts);

    const std::size_t used = BoundedLength(buf, bufSize);
    if (ti::ShowCapacityCounter(used, bufSize)) {
        // Drawn after the field, but the field stays the last item: a caller's SetItemTooltip or
        // IsItemHovered right after this call still means the field, with or without the count.
        ImGuiContext& g = *ImGui::GetCurrentContext();
        const ImGuiLastItemData field = g.LastItemData;
        ImGui::SameLine();
        // A byte count is language-neutral, so it needs no localization row.
        ImGui::TextDisabled("%d / %d", static_cast<int>((std::min)(used, kMaxShownBytes)),
                            static_cast<int>((std::min)(bufSize - 1u, kMaxShownBytes)));
        g.LastItemData = field;
    }
    return r;
}

MultilineResult Multiline(const char* label, std::string& value, const ImVec2& size, const MultilineOptions& opts) {
    const char* shownLabel = SmatchetLocalization::LabelFromSource(label);
    const ImGuiID id = ImGui::GetID(shownLabel);

    MultilineResult r;
    // ImGui would revert the whole edit on this Esc; take the field off the keyboard first instead.
    if (ti::MultilineEscLeavesField(ImGui::GetActiveID() == id, ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        ImGui::ClearActiveID();
        r.leftField = true;
    }
    TakeFocusOnAppear(id, opts.focusOnAppear);

    r.edited = ImGui::InputTextEx(shownLabel, TranslatedHint(opts.hint), StringBuf(value), StringBufSize(value), size,
                                  opts.flags | ImGuiInputTextFlags_Multiline | ImGuiInputTextFlags_CallbackResize,
                                  StdStringResizeCallback, &value);
    r.active = ImGui::IsItemActive();
    const ImGuiIO& io = ImGui::GetIO();
    r.submitted = ti::MultilineSubmitted(ImGui::IsItemDeactivated(),
                                         ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false),
                                         io.KeyCtrl, io.KeyShift, io.KeyAlt, io.KeySuper, opts.submitOnCtrlEnter);
    return r;
}

bool ClearSearchOnEscape(ImGuiID searchId, char* buf) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (searchId == 0 || (g.ActiveId != searchId && g.ActiveIdPreviousFrame != searchId) ||
        !ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        return false;
    }
    buf[0] = '\0';
    if (g.ActiveId == searchId) {
        ImGui::ClearActiveID();
    }
    if (g.InputTextDeactivatedState.ID == searchId) {
        g.InputTextDeactivatedState.ClearFreeMemory();
    }
    return true;
}

} // namespace SmatchetInput
