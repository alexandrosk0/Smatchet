#pragma once

#include "TicketFieldEditorCommitPolicyPure.h"

#include "imgui.h"
#include "imgui_internal.h" // ImHashStr / OpenPopupEx / IsPopupOpen for the arm-then-popup gate

#include <string>

// P1.3 shared touch/click open-gate for grid cell editors (#1018 item 23). Unifies the open/arm
// gesture across the five cell editors (inline text, SingleSelect / MultiSelect / Cascading combos,
// Labels, DateTime) so the long-press-vs-click rule lives in one place rather than being re-derived
// per editor. The pure predicate it consumes (ShouldOpenCellEditorByLongPress) is unit-tested in
// TicketFieldEditorCommitPolicyPure.h without ImGui or __ANDROID__.
//
// Lives at the include/ root (a leaf/utility header, NOT under Ui/): its consumers are the
// domain-side cell-editor TUs (TicketFieldEditor.cpp plus the Tracker Labels/DateTime editors), and
// domain code must not include Ui/ headers (lint rule no-ui-include-in-domain — the layer DAG flows
// Ui -> domain, never the reverse). It depends only on ImGui (third-party, layer-neutral) and the
// pure commit-policy header beside it — nothing from the Ui/ subsystem.
//
// Include this BEFORE any `#define ImGui SmatchetLocalizedImGui` in the consuming TU so the ImGui::
// calls below bind to the real ImGui namespace (the localization wrapper only forwards text-drawing
// calls; the input/state queries used here — GetIO / IsItemHovered / IsMouseDragging — are identical
// either way, but binding to the real namespace keeps the header independent of the macro).
namespace SmatchetTouchEdit {

// Compile-time: is this the mobile/touch build? The NDK toolchain auto-defines __ANDROID__ and
// Source/Core has no other mobile gate today. Desktop is constexpr-false, so the touch branch in
// ShouldOpenCellEditorOnGesture dead-eliminates and the gate collapses to the exact pre-existing
// `clicked && (openOnClick || IsMouseDoubleClicked(0))` expression — byte-identical desktop codegen.
#if defined(__ANDROID__)
constexpr bool kMobileTouchBuild = true;
#else
constexpr bool kMobileTouchBuild = false;
#endif

// Long-press hold threshold to open a cell editor on the touch build (seconds). A quick tap selects
// the cell / scrolls the grid; a stationary hold past this opens the editor (the standard Android
// disambiguator, since touch has no double-click).
constexpr float kCellLongPressOpenSeconds = 0.5f;

// True this frame if the cell editor should open / arm. Call UNCONDITIONALLY right after the cell
// Selectable (NOT nested inside `if (Selectable(...))`): on the touch build the open must fire
// mid-hold, before the Selectable's tap-release return.
//   cellSelectableClicked - the Selectable's return value (the desktop click/double-click path).
//   openOnClick           - desktop single-click-to-edit affordance, folding any blank-cell rule
//                           (an empty cell that opens on a single click). Ignored on the touch build,
//                           where long-press is the sole open gesture.
inline bool ShouldOpenCellEditorOnGesture(bool cellSelectableClicked, bool openOnClick) {
    if (kMobileTouchBuild) {
        // IsItemHovered() refers to the cell Selectable drawn immediately before this call.
        const ImGuiIO& io = ImGui::GetIO();
        return TicketFieldEditorCommitPolicyPure::ShouldOpenCellEditorByLongPress(
            ImGui::IsItemHovered(), io.MouseDown[0], io.MouseDownDuration[0], ImGui::IsMouseDragging(0),
            kCellLongPressOpenSeconds);
    }
    return cellSelectableClicked && (openOnClick || ImGui::IsMouseDoubleClicked(0));
}

// Arm-then-popup state-machine + collapsed-preview Selectable, shared by the four combo/popup cell
// editors (SingleSelect / MultiSelect / Cascading / Labels). Returns true if the editor is ARMED
// this frame — the caller draws the open BeginCombo/popup body. Returns false if it drew the
// collapsed Selectable preview — the just-drawn Selectable is the current item, so the caller may
// draw its own preview tooltip / icon overlay (those queries refer to it) and then `return`.
//   editArmedKey / editArmedJustOpened - the two SpreadsheetState arm fields, by ref (keeps this
//       header free of the heavy SpreadsheetState include).
//   editorKey      - this cell's arm key (ticket.id + "::" + field.Id).
//   comboIdForHash - the BeginCombo string id used to derive the popup id; MUST match the id the
//       caller later passes to BeginCombo (e.g. "##multiselect"), or the OpenPopupEx/IsPopupOpen
//       probe targets the wrong popup.
//   selectableLabel - the collapsed Selectable's full id label (the combos pass the bare preview,
//       which doubles as id; Labels passes a "##LabelsPreview_..."-suffixed label).
inline bool ArmThenPopupCellGate(std::string& editArmedKey, bool& editArmedJustOpened, const std::string& editorKey,
                                 const char* comboIdForHash, const char* selectableLabel, float cellAvail,
                                 bool singleClickToEdit) {
    bool armed = (editArmedKey == editorKey);
    if (armed) {
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, ImGui::GetID(comboIdForHash));
        if (editArmedJustOpened) {
            ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
            editArmedJustOpened = false;
        } else if (!ImGui::IsPopupOpen(popupId, 0)) {
            // User dismissed the popup on a prior frame; release arm.
            editArmedKey.clear();
            armed = false;
        }
    }
    if (!armed) {
        const ImVec2 selSize(cellAvail > 0.0f ? cellAvail : 0.0f, 0.0f);
        const bool armCellClicked =
            ImGui::Selectable(selectableLabel, false, ImGuiSelectableFlags_AllowDoubleClick, selSize);
        if (ShouldOpenCellEditorOnGesture(armCellClicked, singleClickToEdit)) {
            editArmedKey = editorKey;
            editArmedJustOpened = true;
        }
    }
    return armed;
}

} // namespace SmatchetTouchEdit
