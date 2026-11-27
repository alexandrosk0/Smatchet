- 2026-11-26 · offline-conflict-esc-hint-fr-row · [debt] · P3 — The conflict resolver's Esc hint has no localization row; its old row is unused

Details:
The fix for #2510 reworded the conflict resolver's "not saved" hint (shown after an Esc around a typed resolution in
`Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp`, `ConflictEscUnsavedHintText`) so it makes the same promise as the
Cancel message: a cancelled resolution is kept, also if Smatchet closes. The old wording said it was kept only until
Smatchet closes, which stopped being true when cancelled resolutions moved into the persistent draft store.
`Source/Core/src/SmatchetLocalization.cpp` was locked by another session, so the call site moved to a new key,
`offline.conflict.esc_unsaved_kept`, that has no table row. `T()` returns the English call-site fallback in every
language, so fr-FR shows the English text (and logs the one-time missing-translation warning). The old row
`offline.conflict.esc_unsaved` in `SmatchetLocalization.cpp` is now unused, and its French column carries the same
wrong promise. The same workaround was used for `offline.conflict.draft_saved` (see
`2026-11-22-drafts-undo-deferred.md`, the fr-FR rows bullet).

Concrete next action:
When the lock on `SmatchetLocalization.cpp` lifts, add an English + French row for `offline.conflict.esc_unsaved_kept`
(English column identical to the call-site fallback) and delete the unused `offline.conflict.esc_unsaved` row; run
`bash agent-layer/agents/scripts/project/test-localization-drift.sh --diff origin/develop`.

Status: open
Last-reviewed: 2026-11-26
