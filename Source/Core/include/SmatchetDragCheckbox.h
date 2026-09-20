#pragma once

#if defined(ImGui)
#error "Include SmatchetDragCheckbox.h BEFORE `#define ImGui SmatchetLocalizedImGui` — the alias would"\
       " rewrite this header's own ImGui:: calls and double-localize the label."
#endif

#include "SmatchetDragCheckboxPure.h"
#include "SmatchetLocalization.h"

#include "imgui.h"
#include "imgui_internal.h" // ImRect / ImGuiWindow / ImGuiItemFlags_Disabled for the hit + disabled tests

// Drag-to-paint checkbox — a drop-in replacement for ImGui::Checkbox in any LIST of
// checkboxes (Views > Fields, the multi-select and Labels cell editors). A plain click
// toggles the row exactly like ImGui::Checkbox does; pressing and DRAGGING across the
// neighbouring rows paints them all to the value the press produced — drag from a checked
// row to clear a run, from an unchecked row to set one. Ticking twenty fields one click at
// a time is the thing this removes.
//
// Rules of the gesture (state machine + rationale: SmatchetDragCheckboxPure.h). The run is
// scoped to the window or child the press started in, so a drag can never paint a list the
// pointer merely passes over. Disabled rows are skipped, so a locked checkbox mid-run stays
// as it is. The swept pointer path is hit-tested, so a fast flick leaves no unpainted holes,
// while a held-still pointer never paints. Keyboard activation (Space) keeps Dear ImGui's own
// behaviour untouched.
//
// One behaviour does change for the row the press lands on: because the toggle is applied on
// the press, dragging off it and releasing elsewhere no longer cancels that first toggle —
// it is the start of a run, which is the whole point of the gesture.
//
// `label` is localized exactly as SmatchetLocalizedImGui::Checkbox localizes it, so a call
// site swapping ImGui::Checkbox for this keeps its visible text and its widget id.
//
// Returns true on the frames the row's value changed (press, or paint), with `*value`
// already updated — the same `if (SmatchetDragCheckbox(...)) { ... }` shape call sites use
// with ImGui::Checkbox. UI thread only.
//
// Lives at the include/ root (a leaf/utility header, NOT under Ui/) for the same reason as
// TouchCellEditGesture.h: its consumers include the domain-side cell-editor TUs, and domain
// code must not include Ui/ headers (lint rule no-ui-include-in-domain — the layer DAG flows
// Ui -> domain, never the reverse). It depends only on ImGui, the localization seam, and the
// pure state machine beside it.

enum SmatchetDragCheckboxFlags_ {
    SmatchetDragCheckboxFlags_None = 0,
    // Paint only while the pointer is over the checkbox square itself. The default hit band
    // spans the row (the label and the space after it), which is what a list wants; use this
    // for rows carrying several independent checkboxes side by side.
    SmatchetDragCheckboxFlags_TightHitRect = 1 << 0,
};

namespace SmatchetDragCheckboxDetail {

// The one in-flight gesture. Every call site runs inside SmatchetUI::Draw on the single UI
// thread (Ui/AGENTS.md), so no synchronisation is needed — and a single instance is what
// scopes the gesture: a press in one list owns the pointer until the button comes up. A
// function-local static keeps it one object across TUs without a C++17 inline variable.
inline SmatchetDragCheckboxPure::PaintGesture& Gesture() {
    static SmatchetDragCheckboxPure::PaintGesture gesture;
    return gesture;
}

// Hit band for the row just drawn. The default spans the content width at the row's height
// so the pointer keeps painting when it wanders off the square onto the label.
inline ImRect HitBandForLastItem(int flags) {
    const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if ((flags & SmatchetDragCheckboxFlags_TightHitRect) != 0) {
        return itemRect;
    }
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    return ImRect(ImVec2(window->WorkRect.Min.x, itemRect.Min.y), ImVec2(window->WorkRect.Max.x, itemRect.Max.y));
}

// True when the pointer crossed the row this frame. The band is clipped to the window's live
// clip rect first: a row scrolled out of the list is still laid out (and still reports item
// geometry), and painting one the user cannot see would be invisible data loss.
inline bool PointerCrossedRow(const ImRect& band) {
    if (!ImGui::IsMousePosValid()) {
        return false;
    }
    // Another window on top of the list (a popup, a menu) owns the pointer — geometry alone
    // would paint straight through it.
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        return false;
    }
    ImRect visible = band;
    visible.ClipWith(ImGui::GetCurrentWindow()->ClipRect);
    if (visible.Min.x > visible.Max.x || visible.Min.y > visible.Max.y) {
        return false; // fully scrolled out of the list
    }
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 cur = io.MousePos;
    if (cur.x < visible.Min.x || cur.x > visible.Max.x) {
        return false;
    }
    const ImVec2 prev = io.MousePosPrev;
    if (!ImGui::IsMousePosValid(&prev)) {
        return false; // no previous sample to sweep from (pointer just re-entered the window)
    }
    if (prev.x == cur.x && prev.y == cur.y) {
        // Paint follows motion: a held-still pointer never paints. That is what keeps a list
        // that re-flows mid-gesture (a row leaving the filter the moment it is unticked, a
        // catalog refresh) from cascading the run onto whatever row slides under the cursor.
        return false;
    }
    return SmatchetDragCheckboxPure::SegmentCrossesBand(prev.y, cur.y, visible.Min.y, visible.Max.y);
}

} // namespace SmatchetDragCheckboxDetail

inline bool SmatchetDragCheckbox(const char* label, bool* value, int flags = SmatchetDragCheckboxFlags_None) {
    IM_ASSERT(label != nullptr && value != nullptr);
    namespace detail = SmatchetDragCheckboxDetail;
    namespace pure = SmatchetDragCheckboxPure;

    // LabelFromSource mirrors SmatchetLocalizedImGui::Checkbox, which every call site reached
    // through the per-TU `#define ImGui SmatchetLocalizedImGui` alias before this widget
    // existed — same visible text, same widget id, in every locale.
    const char* localized = SmatchetLocalization::LabelFromSource(label);
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (ImGui::GetCurrentWindow()->SkipItems) {
        // Clipped/collapsed window: ImGui::Checkbox early-outs and leaves LastItemData owned
        // by whatever item ran last, so no gesture decision may be taken off it.
        return ImGui::Checkbox(localized, value);
    }

    const int frame = ImGui::GetFrameCount();
    const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    // The release frame reads button-up but is exactly the frame ImGui::Checkbox reports its
    // own toggle on, so the gesture has to survive it (SmatchetDragCheckboxPure::GestureLapsed).
    const bool mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    pure::PaintGesture& gesture = detail::Gesture();
    if (pure::GestureLapsed(gesture, mouseDown, mouseReleased, frame)) {
        gesture = pure::PaintGesture();
    }

    pure::ItemFrame item;
    item.Value = *value; // captured BEFORE ImGui applies its own release-toggle
    item.Clicked = ImGui::Checkbox(localized, value);
    item.Id = g.LastItemData.ID;
    item.Scope = ImGui::GetCurrentWindow()->ID;
    item.Disabled = (g.LastItemData.ItemFlags & ImGuiItemFlags_Disabled) != 0;
    item.Pressed = ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    item.DraggedOver = gesture.Active && gesture.Scope == item.Scope && mouseDown &&
                       detail::PointerCrossedRow(detail::HitBandForLastItem(flags));

    const pure::ItemDecision decision = pure::DecideItem(gesture, item);
    if (decision.Write) {
        *value = decision.Value;
    }
    if (decision.Begin) {
        gesture.Active = true;
        gesture.Scope = item.Scope;
        gesture.Origin = item.Id;
        gesture.Value = decision.Value;
    }
    if (gesture.Active && gesture.Scope == item.Scope) {
        gesture.Frame = frame; // this list is still drawing — the gesture has not lapsed
    }
    return decision.Changed;
}
