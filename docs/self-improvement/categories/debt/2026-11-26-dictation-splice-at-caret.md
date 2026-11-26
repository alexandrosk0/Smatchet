- 2026-11-26 · input-ux-w2-dictation-drain · [debt] · P3 — Dictation appends at the end of a field, not at the caret

Details:
`docs/plans/shipped/input-ux-w2-dictation-drain.md` (Wave 2 leftover, PR #2496) made dictated text survive the
next keystroke in every routed field. It kept insertion where it was, at the end of the text: the plan lists
"splice at the caret" as a non-goal.

- The router's splice (`DictationInsertionRouter_Whisper.cpp`, `insertAt`) inserts at the registered entry's
  cursor only when the entry has one. `SmatchetLocalizedImGui::HookDictationOnLastItem`
  registers every routed field with a null cursor, so text goes to the end.
- `SmatchetLocalizedImGui::ApplyPendingDictationReload` then calls `ReloadUserBufAndMoveToEnd`, which puts the
  caret after the end of the text, which is right only for an append.

So a user who places the caret mid-sentence and dictates gets the words at the end of the field.

Why debt and not a GitHub Issue: appending has been the insertion behaviour since dictation shipped, and the plan
kept it on purpose. Nothing regressed and no text is lost. If a user reports it as a bug, open an Issue
(`area:` the dictation subsystem) and link it here.

Concrete next action:
Have the hook pass the active widget's caret (`ImGuiInputTextState::GetCursorPos()`, or the selection when there
is one) to the router. Splice there and replace a selection. Reload with the caret after the inserted text
(`ReloadUserBufAndKeepSelection` with the selection set to that point), not at the end. Extend
`OfflineFirst/Comments_DictatedTextIsKeptAsDraft` with a caret-in-the-middle case.

Status: open
