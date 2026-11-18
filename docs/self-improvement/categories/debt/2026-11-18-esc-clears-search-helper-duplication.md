- 2026-11-18 · input-ux-w2-action-keys · [debt] · P3 — The "Esc clears the search and leaves the box" block exists twice (grid search box and Preferences search box)

Details:
The grid search box (`SmatchetGridHeaderUi.cpp`, develop) and the Preferences search box
(`SmatchetPreferencesUi.cpp`, input UX Wave 2 piece 4.4) carry the same ~15-line block: when the box held the
caret this frame or the last one and Esc is pressed, clear the buffer, take the caret out, and drop ImGui's saved
copy of the deactivated field (`InputTextDeactivatedState`) so the cleared buffer stays cleared. The DRY gate
flagged the pair (91-token clone). The PR exempted its own occurrence with `SMATCHET_DEVIATION(rule=duplication)`
because plan-lock `pane-picker-jira-sites` held `SmatchetGridHeaderUi.cpp` at the time, so a shared helper could
not edit both sides.

Concrete next action:
Add one helper (for example `ConsumeEscClearsSearch(ImGuiID inputId, char* buf)` beside the submit-key widgets in
`Ui/SmatchetSubmitKeys.h`, or in `Ui/SmatchetInputPure.h` if the field helper grows an Esc-clear option), call it from
both boxes, drop the deviation marker in `SmatchetPreferencesUi.cpp`, and keep the bucket-E cases
`PrefsSearchFilter/SearchKeys_EscClearsTheQueryAndLeavesTheBox` and the grid search Esc case green.

Status: open
Last-reviewed: 2026-11-18
