# Smatchet Input UX Audit

**Date:** 2026-11-13
**Branch:** `claude/frictionless-user-input-ux-ea174b`
**Scope:** Every first-party user-input surface of the standalone Dear ImGui desktop client under `Source/Core` and `Source/Plugins`: shared text-entry primitives, the command palette, grid keyboard navigation and inline editing, typed field editors, issue creation and composing, search and JQL input, Preferences and first-run setup, cross-cutting keyboard / focus / modal conventions, the AI assistant composer, dictation and the Lua console, the offline-queue conflict resolver, the toolbar button editor, Annotate settings, and file-drop / pointer gestures. **Not read in depth:** the mobile shell and the Unreal-embedded input path (touched only where they share code with the desktop client), and vendored third-party code. Items already marked Done in the UX critique were excluded.
**Method:** Static reading of the source (the app was not run). Thirteen auditor agents each read one surface and reported friction with `file:line` and a quote. A second agent per surface tried to refute every finding by rereading the cited code; 36 findings were corrected that way and 1 was dropped as already fixed. A completeness critic compared the files read against `Source/Core/src/Ui/` and named four surfaces the first round missed; those were audited and verified the same way. Eight high-severity findings were rechecked by hand and all eight held.
**Companions:** [`UX_DESIGN_CRITIQUE.md`](UX_DESIGN_CRITIQUE.md) (2026-07-05, general design critique; this report covers input only and does not re-report it), [`CPP_CODE_AUDIT.md`](CPP_CODE_AUDIT.md), [`SECURITY_AUDIT.md`](SECURITY_AUDIT.md).

> **How to read severity.** High = a common path is blocked, work is lost, or extra steps are paid every session. Medium = friction with a workaround. Low = polish. **Effort:** S = under a day in one file, M = a few files, L = cross-subsystem and needs a plan. Finding numbers are stable; cite them as `INPUT_UX_AUDIT.md #N`.

---

## 1. Summary

Smatchet already has a modern answer for almost every input pattern, and each one lives in a single place. The grid inline editor, the JQL autocomplete, the Save-as-new dialog and the long-text editor behave the way Linear or VS Code would. The comment box, the worklog dialog, Preferences, the assistant panel and most confirm dialogs do not, because nothing is shared between them. Closing the gap takes seven shared contracts (§ 4) and a batch of small fixes (§ 5). No surface needs a rebuild.

| | |
|---|---|
| Verified findings | 142 |
| Severity | 33 high, 68 medium, 41 low |
| Effort | 78 S, 55 M, 9 L |
| Surfaces | 13 |
| Source files read | 100 |
| Findings closed by the seven shared fixes | 91 of 142 |

---

## 2. What already meets the bar

These are the models to copy. None of them should be rebuilt.

- **Grid inline text edit.** Caret on the first frame, Enter commits, Esc reverts, the save runs off-thread with per-cell Saving, Saved, Error and Queued badges.
- **JQL autocomplete.** Up and Down move, Enter and Tab accept, Esc dismisses, user lookups are debounced and never block the frame.
- **Grid search box.** A glyph and tooltip preview what Enter will do before it is pressed. A pasted issue key jumps to its row.
- **Long-text editor.** Ctrl+Enter saves, Esc cancels, and the footer says so.
- **Field viewer.** Esc leaves the field first and closes second. Unsaved input gets a Discard or Keep choice where Esc means keep.
- **Save-as-new dialog.** Name prefilled, caret placed, Enter commits, Save disabled while empty.
- **Credential input.** Show and Hide, character count, a warning for stray whitespace, values trimmed on save.
- **Test connection.** Probes what is typed, not what is saved, and reports the server diagnostic.
- **Settings search.** Typo-tolerant, filters the real widgets in place.
- **Shortcut rebinding.** Several combos per command, capture that Esc cancels, hints that follow rebinds.
- **Command palette basics.** Focus on the first frame, recent commands first, bound shortcut on each row.
- **Assistant composer.** An over-size paste is reported with the dropped size. A failed send keeps the draft.

Per-surface strengths with their `file:line` evidence are listed at the top of each surface in § 7.

---

## 3. Proposed keyboard contract

Today the same key means different things depending on the window. Esc alone has three behaviours: leave the field, cancel at once, or nothing. This table is the proposed contract. The last column names the code that already follows it.

| Context | On open | Enter | Ctrl+Enter | Esc | Tab | Already does it |
|---|---|---|---|---|---|---|
| Single-line field | Caret in the field, existing text selected | Commit | Same as Enter | Revert to the previous value | Commit, move to next control | Save-as-new dialog; grid inline text edit |
| Multiline composer | Caret in the field, placeholder shows the submit key | New line | Submit | First press leaves the field. Second press closes, and a dirty draft is kept or asked about | Next control, never a tab character | Long-text editor; field viewer (layered Esc) |
| Chat composer | Caret in the field | Send | New line | Stop the reply in flight, else leave the field | Next control | Assistant composer (Enter and Ctrl+Enter only) |
| Picker or filter popup | Caret in the filter box | Pick the highlighted row; nothing on an empty list | n/a | Close, change nothing | Accept the highlighted row | Single-select combo; JQL autocomplete |
| Form dialog | Caret in the first field | Field-level commit | Save | Cancel with a dirty guard; focus returns to the opener | Next field | Bug report; Quick Create (minus the dirty guard) |
| Confirm dialog | Focus on the safe button when destructive, on the primary otherwise | Activate the focused button | n/a | Cancel | Move between buttons | Field viewer discard confirm (Esc keeps) |
| Grid cell | F2, Enter or typing starts the edit | Commit, move down | Commit, apply to the selection | Revert | Commit, move right | Enter commits and Esc reverts today; nothing moves on |
| Command palette | Caret in the query, top match selected | Run | n/a | Argument form steps back to the list; list closes | n/a | First-frame focus, recents, shortcut per row |

Global shortcuts stay off while any dialog is open. Every dialog button that has a key shows it.

**Pointer gestures.** Every drag gesture has a command with a rebindable default key, and every popup list is navigable with the arrows and Enter.

- A drag gesture is any of `BeginDragDropSource`, `IsMouseDragging`, `GetMouseDragDelta` or a reorderable table. Reorder, resize, splitter and list-action gestures all count.
- The command is registered once and reachable from every front-end (palette, CLI, MCP, Lua), and bound in `KeybindingsConfig::Defaults()`.
- A gesture that acts on one widget (a list row, a splitter) uses a focus-scoped command (`ui.list.*`, `ui.splitter.*`): the widget with the keyboard takes the request, and with none focused the key does nothing. A gesture on a named thing (a panel) names it in the command's args.
- The drag source carries a `keyboard-equivalent: <command.id>` comment naming that command, or `keyboard-equivalent: native(<why>)` when ImGui's own navigation already covers it, or a `SMATCHET_DEVIATION(rule=drag-without-command; ...)` marker. `scripts/dev/test-drag-keyboard-equivalent.sh` fails any drag source without one of the three.
- A popup list with a filter box moves a highlight with Up / Down, picks it with Enter, and closes on Esc, all with the caret still in the filter (the "Picker or filter popup" row above). The shared helper for this is `FilterDropdown`.

---

## 4. Seven shared fixes

Each item is one helper or convention. Building it once closes the findings listed under it (bold = high severity). Proposed file and helper names are suggestions.

### 4.1 A dialog contract (closes 18, 7 high)

- **Today:** Dialogs open with no caret, most ignore Esc, confirms have no default button and list the destructive action first, and app-wide shortcuts still fire underneath.
- **Build:** One dialog scope helper beside Ui/SmatchetModalPopupOpen.h and Ui/SmatchetFocusRequest.h: focus the first field on frame one, restore the previous focus on close, route Esc to cancel and Enter or Ctrl+Enter to the default action, mute global shortcuts while open. A confirm footer helper that draws the button pair and puts focus on the safe one.
- **Model in the tree:** Save-as-new dialog and the field viewer already behave this way.
- **Closes:** #3, #24, **#37**, #43, **#48**, #69, **#73**, **#85**, **#86**, #88, #89, #91, #92, #94, **#96**, **#109**, #120, #121

### 4.2 A shared text field (closes 12, 0 high)

- **Today:** Every field is a raw InputText over a fixed char buffer. The localisation wrapper adds a label and the dictation hook, nothing else, so focus, select-all, trimming and length limits differ per call site and several buffers clip silently.
- **Build:** A SmatchetInput family backed by std::string with a resize callback, with options for focus on open, select-all on focus, trim on commit, placeholder, and the Enter, Esc and Ctrl+Enter rules from the contract. Migrate Preferences first, then pickers, then composers.
- **Model in the tree:** SmatchetSecretInputText is a shared field for one case. The toolbar editor already has a std::string resize helper to hoist.
- **Closes:** #1, #2, #4, #5, #9, #10, #13, #36, #102, #112, #125, #132

### 4.3 Drafts and undo (closes 16, 6 high)

- **Today:** Typed work lives in memory only. Esc, a closed window, a failed save or a crash discards it, and destructive actions rely on confirm dialogs because nothing can be undone.
- **Build:** A draft store keyed by surface and issue that saves on change and restores on reopen, plus an undo action on the existing toast for optimistic commits and dismissals. Dirty-guard every Esc that would drop text.
- **Model in the tree:** The field viewer asks Discard or Keep. A failed assistant send keeps the draft.
- **Closes:** #7, **#14**, **#28**, #31, #33, **#39**, #50, #51, #56, **#74**, **#87**, #90, #103, **#110**, #116, #141

### 4.4 A key for every primary action (closes 13, 5 high)

- **Today:** Submit, save, search, stop and run often have no key at all, and where a key exists nothing on screen says so.
- **Build:** Wire Ctrl+Enter on every multiline field through one helper that also prints the key on the button. Register a focus-search chord and the other missing actions as commands so they are rebindable and listed in the shortcut guide. Add the dialog rules to docs/guides/keyboard-shortcuts.md.
- **Model in the tree:** The long-text editor prints its keys in the footer. Menu items already show live shortcut hints.
- **Closes:** #6, **#49**, #59, **#60**, #65, **#72**, #79, #95, **#97**, **#99**, #105, #107, #111

### 4.5 Tolerant parsing with inline errors (closes 14, 6 high)

- **Today:** Dates accept one format, durations reject 1.5h, a bad query or bad JSON fails later and elsewhere, and several failures are reported only in the log.
- **Build:** One parse-and-explain layer that returns a value or a message saying how to fix it, drawn by a shared inline error under the field while typing. Accept natural dates, decimal and spaced durations, pasted URLs. Every silent failure path gets a toast with cause and fix.
- **Model in the tree:** Test connection reports the server diagnostic inline. The conflict resolver explains a bad scalar.
- **Closes:** **#38**, **#40**, #53, #58, **#63**, #66, #80, #82, **#98**, #115, **#117**, #127, #128, **#136**

### 4.6 Keyboard equivalents for pointer gestures (closes 11, 1 high)

- **Today:** Reorder, resize, calendar picking, sort editing and several list actions exist only as mouse gestures.
- **Build:** A rule in the keyboard contract: every drag gesture has a command with a rebindable key, and every popup list is navigable with arrows and Enter. A check that lists drag sources without a matching command.
- **Model in the tree:** Side bars and the bottom panel have rebindable toggles. The toolbar context menu offers Move Left and Move Right.
- **Closes:** #12, #18, #29, #30, #44, #71, **#108**, #119, #122, #130, #138
- **Status:** the rule is in § 3 and `scripts/dev/test-drag-keyboard-equivalent.sh` enforces it. Shipped in `docs/plans/input-ux-w2-gesture-keys.md`: the `ui.list.*`, `ui.splitter.*` and `view.panel.grow` / `shrink` commands, the toolbar list keys (#122), the calendar keys and quick picks (#44), the path-remap flow (#130), read-only text select-all from the keyboard (#12), and panel resize (#138, panels). #108 and half of #18 shipped in Wave 1. Waiting on the open plans that hold the files: grid column move / resize (#138, columns), the sort-rule list (#71), the filter + list popups (#119) and the palette outside click (#18). #29 and #30 stay with Wave 3.

### 4.7 One matcher (closes 7, 2 high)

- **Today:** Pickers and filters use exact substring tests, some case-sensitive, some ASCII-only, with no ranking. Multi-word search needs the exact phrase.
- **Build:** Promote Commands/FuzzyMatch.cpp to a shared token-based, case-folding matcher that returns a score and match positions, and route every picker, filter and the title search through it. Highlight the matched letters.
- **Model in the tree:** Settings search already falls back to the fuzzy score.
- **Closes:** #8, #20, #42, **#62**, #67, **#118**, #131

---

## 5. Suggested sequence

### 5.1 Wave 1: small fixes on daily paths

High severity, effort S, one file each. No new shared code needed.

| # | Fix | Surface | How often | Where |
|---|---|---|---|---|
| #16 | After arrowing down and then editing the query, the highlighted row stays at its old index instead of jumping to the best match. | Command palette | every session | `CommandPaletteUi.cpp:134` |
| #49 | In the inline new-issue row, Tab in the description types a tab character, and nothing submits or cancels from the keyboard once you leave Summary. | Issue creation and long-form composing | every session | `SmatchetNewIssueDraftUi.cpp:530` |
| #60 | There is no shortcut to jump into the grid search box, so every search starts with a mouse click. | Search | every session | `SmatchetGridHeaderUi.cpp:302` |
| #62 | Multi-word title search only matches when the exact phrase appears contiguously inside one field, so "login crash" finds nothing if the words are apart. | Search | every session | `TicketGridFilterPure.cpp:70` |
| #72 | On the connection form, Enter in a field does nothing and Save & Sync has no keyboard shortcut, so finishing setup means reaching for the mouse. | Preferences | every session | `SmatchetPreferencesUi.cpp:420` |
| #73 | Opening Preferences, including the forced first-run open, puts the caret nowhere, so the first keystroke is lost and the user must click a field. | Preferences | every session | `SmatchetUI_MainMenu.cpp:142` |
| #96 | Opening the assistant panel does not put the caret in the chat input, so the user must click the box every time. | AI assistant composer | every session | `SmatchetAiAssistantUi.cpp:1607` |
| #15 | Scrolling the palette list with the mouse wheel snaps back to the selected row. | Command palette | daily | `CommandPaletteUi.cpp:403` |
| #37 | Opening Log work gives no caret and no keyboard submit, so every worklog needs a mouse click into the field and a click on Save. | Typed field editors | daily | `TicketFieldEditor_Worklog.cpp:376` |
| #48 | Writing a comment means clicking into the box first, then clicking Post; there is no Ctrl+Enter, and an empty Post click does nothing. | Issue creation and long-form composing | daily | `SmatchetCommentsModalUi.cpp:554` |
| #87 | Pressing Esc in the long-text editor throws away the whole edit with no confirmation, while the sibling field viewer guards the same case. | Cross-cutting keyboard | daily | `TicketFieldEditor_Modal.cpp:497` |
| #97 | Esc does nothing in the assistant: an in-flight response can only be stopped with the mouse, and Esc does not return focus. | AI assistant composer | daily | `SmatchetAiAssistantUi.cpp:1247` |
| #98 | When dictation fails (consent off, mic error, transcription error, empty result, busy), the user sees nothing and the key press just does nothing. | AI assistant composer | daily | `WhisperPlugin.cpp:836` |
| #14 | When a palette command fails (bad argument), the palette closes, my typed arguments are gone, and I see no error. | Command palette | occasional | `CommandPaletteUi.cpp:192` |
| #74 | Closing Preferences with unsaved Assistant (API key, provider) edits silently discards them; only Tracker and MCP edits trigger the guard. | Preferences | occasional | `SmatchetPreferencesUi.cpp:927` |
| #108 | I cannot open a merge conflict from the keyboard; it needs a double-click or a right-click menu. | Offline queue conflict resolver (merge editor) | occasional | `SmatchetOfflineQueueUi.cpp:985` |
| #109 | Conflict dialog opens with nothing focused, and Enter or Ctrl+Enter do not submit | Offline queue conflict resolver (merge editor) | occasional | `SmatchetOfflineQueueUi.cpp:1420` |
| #117 | Typing invalid JSON in Args gives no feedback; the button later runs with empty args and only a log warning. | Custom toolbar button editor | occasional | `SmatchetToolbarUi.cpp:665` |
| #118 | The command filter is case-sensitive substring, so typing 'refresh' misses 'Refresh'. There is no fuzzy match. | Custom toolbar button editor | occasional | `SmatchetToolbarUi.cpp:655` |
| #136 | Dropping a non-markdown file does nothing visible, and a mixed drop silently loses the non-markdown files. | File drop | occasional | `StandaloneBoot_detail.cpp:124` |

### 5.2 Wave 2: the seven shared pieces

Build the dialog helper (4.1) and the field helper (4.2) first, since they close the most findings, then the matcher, the parse-and-explain layer, and the draft store with the undo toast. New shared headers under `Source/Core/include/Ui/` widen a shared contract, so each needs a plan-doc (`Source/Core/src/Ui/AGENTS.md` § Before you edit).

### 5.3 Wave 3: structural work

Large items that change how a surface works. Each wants its own plan.

- Grid as a spreadsheet: active cell, type-to-edit, Enter-down and Tab-right, cell ranges, paste, bulk apply, undo (**#26**, **#27**, **#28**, #29, #30). Plan: `docs/plans/active/input-ux-w3-grid-spreadsheet.md`, four slices. **Slice 1 shipped:** the active cell, the arrow / Home / End / Page keys, a click that selects without editing, Shift+click / Shift+arrow ranges, and rows, ranges and the active cell kept by issue id across a sync, re-sort or filter (#30, #32, and the navigation half of #29). **Slice 2 shipped:** F2, Enter and type-to-edit open the active cell's editor (a dropdown cell opens its popup), and Enter / Shift+Enter / Tab / Shift+Tab save and move the active cell over editable cells, with no editor opened on the neighbour (#26, and the editing half of #29). **Slice 3 shipped:** Ctrl+Z and the "saved" toast's Undo put a saved grid edit back (one bulk set undoes row by row), the rejected text comes back in the editor with the tracker's message under it, and the noisy per-edit toast is now the one undoable toast (#28 undo, #31, #33, #34). Paste, fill-down and Ctrl+Enter are slice 4.
- Filter without JQL: suggestions and facet chips in the grid search box, inline query errors (**#61**, **#63**, #68).
- Attachments by drop and paste on drafts and comments (#54, **#137**).
- Templates, preview and mentions in descriptions (#55).

---

## 6. Distribution

| Surface | keyboard | focus | validation | discoverability | steps | lost-work | parsing | responsiveness | bulk | accessibility | all | high |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Text-entry primitives and shared input widgets | 1 | 3 | · | 2 | 2 | 2 | 1 | · | · | 2 | 13 | 0 |
| Command palette | 2 | 3 | 2 | 3 | · | · | 1 | 1 | · | · | 12 | 3 |
| Grid keyboard navigation | 2 | · | 2 | 1 | 1 | 3 | · | 1 | 1 | · | 11 | 3 |
| Typed field editors | 1 | 2 | 1 | 1 | 2 | 1 | 3 | · | · | · | 11 | 4 |
| Issue creation and long-form composing | 2 | · | 2 | 2 | 3 | 3 | · | · | · | · | 12 | 2 |
| Search | 4 | 1 | 1 | 1 | 2 | · | 3 | · | · | · | 12 | 4 |
| Preferences | 4 | 1 | 2 | 1 | 3 | 1 | 1 | · | · | · | 13 | 3 |
| Cross-cutting keyboard | 5 | 2 | · | 2 | · | 2 | · | · | · | · | 11 | 3 |
| AI assistant composer | 3 | 1 | 1 | 2 | 2 | 1 | · | 1 | 1 | · | 12 | 4 |
| Offline queue conflict resolver (merge editor) | 2 | 1 | 2 | 1 | 1 | 2 | · | · | · | · | 9 | 3 |
| Custom toolbar button editor | 2 | 1 | 3 | 1 | 1 | 1 | 1 | · | · | · | 10 | 2 |
| Annotate / analysis settings and path remaps | 1 | · | 5 | 2 | · | · | 1 | · | · | · | 9 | 0 |
| File drop | 1 | 1 | 1 | 2 | 1 | 1 | · | · | · | · | 7 | 2 |
| **All surfaces** | 30 | 16 | 22 | 21 | 18 | 17 | 11 | 3 | 2 | 2 | 142 | 33 |

---

## 7. Findings by surface

### 7.1 Text-entry primitives and shared input widgets

Every text field in Smatchet is a raw Dear ImGui InputText over a fixed char buffer, routed through a thin localization wrapper in SmatchetLocalizedImGui.h. The wrapper only translates labels and hints and registers the buffer with the dictation router. It adds no select-all, trimming, Esc handling, focus handling or truncation feedback. Each screen decides its own flags and focus. Across the first-party tree there are about 103 call sites: InputText 57, InputTextWithHint 28 and InputTextMultiline 18. Only 11 files use EnterReturnsTrue, none use AutoSelectAll or CallbackCompletion, and one site uses CtrlEnterForNewLine. SetKeyboardFocusHere appears in 12 files. The pieces that are already good are standalone: SmatchetSecretInputText, the JQL autocomplete callback, the AI composer's paste-truncation detection, and the grid editor's truncated-load guard.

**Already good here:**

- Masked credential input shows a Show/Hide toggle, a char count and a leading/trailing whitespace warning. It exposes an editing flag so callers do not act on half-typed prefixes (Source/Core/include/Ui/SmatchetSecretInput.h:44-49 "(%d chars, has leading/trailing whitespace)"; :33 outEditing)
- The AI composer follows the modern chat convention: Enter sends and Ctrl+Enter inserts a newline. It also detects over-cap pastes through CallbackResize and toasts the dropped size (Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1163-1164 "ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_CallbackResize"; :1178 "Paste truncated")
- The JQL autocomplete has a complete keyboard model. Up/Down navigate through CallbackHistory, Enter and Tab accept, Esc dismisses the list, and the caret is placed inside inserted parentheses. User lookups are debounced, run through std::async and never block the UI thread (Source/Core/src/Ui/SmatchetAutocompleteUi.cpp:199-203 ResolveAcpHistoryNav; :247-251 ResolveAcpCommit; :609 "Dispatch to worker thread via std::async")
- Grid inline text editing refuses to commit a value that was truncated on load, so the tail of a long field cannot be silently destroyed (Source/Core/src/TicketFieldEditor.cpp:480-484 "Committing the truncated copy would silently overwrite the field"; InlineEditLoadedTruncated)
- Some surfaces already request focus on the first frame. The single-select filter combo focuses its search on open and commits on Enter. The New Issue summary field has a focus latch, EnterReturnsTrue and a trimmed non-empty guard. Quick-create has a latch, Ctrl+Enter to submit and Esc to cancel (Source/Core/src/TicketFieldEditor.cpp:646-653 "auto-focused on combo open"; Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:543-545 newIssueFocusSummaryPending; Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp:310-313)

#### #1 — There is no shared text-field widget, so every field gets different Enter, Esc, focus, trim and truncation behaviour

**Severity: Medium** · Effort L · keyboard · every session · `no-shared-input-primitive`

- **Evidence:** SmatchetLocalizedImGui.h:124-149 wrappers only translate labels and hook dictation; Ui/SmatchetSecretInput.h:27 shows a shared widget exists for secrets only; AutoSelectAll and CallbackCompletion absent from first-party Source. Raw call counts in the original are inflated.
- **Today:** The wrapper only translates labels and registers the dictation hook. Each call site picks its own flags, focus logic and commit rules. Whether Enter commits, Esc cancels, Ctrl+Enter submits or the caret is ready on open depends on which window you are in.
- **Change:** Add a SmatchetInput::Text(label, hint, std::string&, opts) family in SmatchetLocalizedImGui.h (or Ui/SmatchetInput.h). Use ImGuiInputTextFlags_CallbackResize on a std::string, with a focusOnOpen option that calls SetKeyboardFocusHere on the first frame. Add options for commitOnEnter, escClears, selectAllOnFocus and trimOnCommit, and an optional multiline Ctrl+Enter submit. Migrate the raw call sites by priority: Preferences (about 40), pickers, then composers.

#### #2 — Worklog description (1 KB) and grid text-edit (512 B) buffers silently clip pasted/typed text

**Severity: Medium** · Effort M · lost-work · occasional · `silent-fixed-buffer-truncation`

- **Evidence:** TicketFieldEditor_Worklog.cpp:100,356-358,364; SpreadsheetState.h:76 EditBuffer[512]; SmatchetUiSession.h:413 prefsSearchBuf[64]. Drop the Templates 512 claim (now 4096 + counter, P2-L9 Done).
- **Today:** A worklog note over 1 KB, a long grid cell, a long preference path or an overlong pasted token is clipped at the buffer size. No toast, no counter, no inline cue. The user finds out when the saved value is cut short.
- **Change:** Move long-text fields (worklog description, comment box, grid text edit, template bodies) to std::string with an InputTextStdStringResize-style CallbackResize. SmatchetToolbarUi.cpp:51-58 already has this helper; hoist it into the shared wrapper. For the fields that stay fixed, reuse the AI composer's InputBufferResizeCallback pattern to toast the dropped byte count and show a character counter near the limit.

#### #3 — Filter box in the labels popup, project picker combo and icon picker modal is not autofocused (single-select combo is)

**Severity: Medium** · Effort S · focus · daily · `filter-in-popup-focus-inconsistent`

- **Evidence:** TrackerLabelsEditor.cpp:69 ##LabelSearch; SmatchetProjectPicker.cpp:308 ##projsearch; SmatchetIconPickerUi.cpp:36 ##iconsearch; none call SetKeyboardFocusHere. Compare TicketFieldEditor.cpp:647-653.
- **Today:** Open the project picker or the labels popup and start typing: nothing happens until you click the search box. The single-select field combo, by contrast, takes typing straight away.
- **Change:** Shared FilterInput helper calling SetKeyboardFocusHere on first popup frame (IsWindowAppearing or the picker's existing open latch such as state.popupWasOpen); reuse the single-select Enter-on-single-match logic. Apply to the 3 popups; leave window toolbar filters alone.

#### #4 — After pressing Enter to add an item (such as a label), the field loses focus and you must click it again before adding the next

**Severity: Medium** · Effort S · focus · daily · `enter-add-loses-focus`

- **Evidence:** TrackerLabelsEditor.cpp:104-122; no SetKeyboardFocusHere in file. P2-L10 concerns Templates suggestion fields and is marked Done, not this label editor.
- **Today:** With EnterReturnsTrue, ImGui deactivates the widget on Enter. The label is added and the field is cleared, but the caret is gone. Adding several labels needs one click per label.
- **Change:** In the shared helper, call SetKeyboardFocusHere(-1) on the frame after an EnterReturnsTrue commit when the widget was submitted. The same pattern applies to the new-label draft, the template suggestion add and the bulk add fields.

#### #5 — Clicking Rename on a view title shows the edit box but the caret is not in it

**Severity: Medium** · Effort S · focus · daily · `view-rename-no-focus`

- **Evidence:** Ui/SmatchetViewsDashboardUi.cpp:372-374 "if (ImGui::SmallButton(\"Rename\")) { d.viewsTitleEditing = true; }" and :357-360 the InputText with no SetKeyboardFocusHere. The file has no SetKeyboardFocusHere at all.
- **Today:** Rename swaps the label for an InputText but nothing activates it. The user must click again before typing. Typing right after Rename goes nowhere, or triggers global shortcuts.
- **Change:** When viewsTitleEditing flips to true, set a one-shot latch and call ImGui::SetKeyboardFocusHere() before the InputText. SetKeyboardFocusHere selects all in ImGui by default, which is the desired behaviour here. Mirror the existing newIssueFocusSummaryPending latch pattern.

#### #6 — Comments post box and Worklog description have no Ctrl+Enter submit and no in-field placeholder (Quick-create already has Ctrl+Enter but does not advertise it)

**Severity: Medium** · Effort M · discoverability · daily · `multiline-no-ctrl-enter-or-placeholder`

- **Evidence:** SmatchetCommentsModalUi.cpp:554-562; TicketFieldEditor_Worklog.cpp:364-366; contrast SmatchetQuickCreateIssueUi.cpp:309-312.
- **Today:** Posting a comment needs the mouse on the Post Comment button. The 'placeholder' sits under the box rather than inside it, and the worklog box has none.
- **Change:** Add Ctrl+Enter to Comments and Worklog (mirror QuickCreate :309-310 or TicketFieldEditor_Modal.cpp:496), and show a Ctrl+Enter hint on the buttons including Quick-create's. Draw-list placeholder inside multiline is feasible in ImGui.

#### #7 — Esc inside Quick-create discards the whole draft with no confirm and no undo

**Severity: Medium** · Effort S · lost-work · occasional · `esc-discards-draft-mid-typing`

- **Evidence:** Ui/SmatchetQuickCreateIssueUi.cpp:313-314 "if (!d.quickCreateInFlight && (cancelClicked || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) { ResetAndClose(d); }"; :132 "d.quickCreateSummaryBuf.clear();". Compare UX_DESIGN_CRITIQUE.md P2-M3 (Done), where Esc defocuses first via WantTextInput.
- **Today:** A reflex Esc while typing a summary and description closes the window and clears both buffers. The P2-M3 'Esc defocuses first' convention was applied to Comments only.
- **Change:** Add a shared EscCloseGuard(draftNonEmpty) helper: first Esc while io.WantTextInput defocuses the field, and a second Esc closes. If the buffers are non-empty it either asks 'Discard draft?' or keeps the draft and restores it on next open.

#### #8 — Pickers and filters match only exact ASCII substrings, with no typo tolerance and no case folding outside Latin letters

**Severity: Medium** · Effort M · parsing · daily · `ascii-substring-filters`

- **Evidence:** SmatchetProjectPicker_detail.cpp:14; TrackerLabelsEditor.cpp:72; SmatchetAuditUi.cpp:286; StringUtil.h:39-42. FuzzyScore is already reused by PreferencesFilter.cpp:187, which shows the sharing path works.
- **Today:** Typing 'prj' does not find 'Project X'. Cyrillic or Greek names are case-sensitive in these pickers even though the font atlas supports them. Wasted time on large project and label lists.
- **Change:** Expose the palette's FuzzyMatch as a shared FilterScore(query, text) in Core and switch the project picker, labels, view lists, icon picker and audit search to it. Keep a plain substring path for IDs. Use a Unicode-aware lowercase (utf8 to codepoint) instead of ToLowerAsciiCopy.

#### #9 — Clicking into a field with existing text (URL, domain, path, title) never selects all, so replacing a value takes manual clearing

**Severity: Low** · Effort S · steps · daily · `no-select-all-on-click-focus`

- **Evidence:** AutoSelectAll count is 0 across Source/Core and Source/Plugins. Example: Ui/SmatchetPreferencesUi.cpp:420 "InputText(\"Domain\", d.domainBuf, ... CharsNoBlank)" and :424 Email. TicketFieldEditor.cpp:96-115 has code to DESELECT because SetKeyboardFocusHere/Tab selects all by default.
- **Today:** Tab or SetKeyboardFocusHere selects the contents. A mouse click places a caret instead. Replacing a pasted URL or base path means Ctrl+A first.
- **Change:** Add an opt-in selectAllOnFocus to the shared wrapper using ImGuiInputTextFlags_AutoSelectAll on single-line settings, path and name fields. Do not set it on the grid cell editor.

#### #10 — The inline keyboard-driven suggestion popup (Up/Down + accept) exists only for the view JQL editor

**Severity: Low** · Effort L · steps · daily · `autocomplete-jql-only`

- **Evidence:** SmatchetViewsDashboardUi_widgets.cpp:319,323 are the only CallbackHistory wirings; TrackerLabelsEditor.cpp:84-101 already offers a filtered suggestion list for labels. Assignee free-text claim unverified.
- **Today:** Free-text fields such as the New Issue label, the assignee text and Preferences model or URL fields offer no completion. JQL gets the full popup.
- **Change:** Generalise TrackerQueryAcp into a SuggestPopup&lt;T> bound to any InputText via a ImGuiInputTextFlags_CallbackAlways|CallbackHistory callback and a provider interface. The history, commit and async user-search code in SmatchetAutocompleteUi.cpp:185-274 and :609-620 is already generic.

#### #11 — Chinese, Japanese and Korean text cannot be displayed or typed meaningfully in any input

**Severity: Low** · Effort M · accessibility · rare · `no-cjk-glyphs`

- **Evidence:** Ui/SmatchetImGuiFonts.cpp:30-70 adds Greek, Cyrillic, Vietnamese, Latin-1 and "kCjkSymbolsAndPunctuation[] = {0x3000, 0x303F, 0}" only. There is no Han, Hiragana, Katakana or Hangul range.
- **Today:** Greek and Cyrillic work. CJK characters pasted or typed render as missing-glyph boxes. The Windows IME candidate window is not positioned by the app in this code.
- **Change:** Add a lazily loaded CJK font merge (ImFontConfig MergeMode with GetGlyphRangesChineseSimplifiedCommon, Japanese and Korean) behind a preference. Verify IME composition position through the GLFW/Win32 backend before promising full IME support.

#### #12 — In read-only text runs, Ctrl+A only works while the mouse hovers over the text, so keyboard-only users cannot select it

**Severity: Low** · Effort S · accessibility · occasional · `readonly-text-select-all-hover-gated`

- **Evidence:** Ui/SelectableTextRun.cpp:184-187 "if (hovered && ctrl && ImGui::IsKeyPressed(ImGuiKey_A, false) && segCount > 0)". Ctrl+C is not hover-gated (:190).
- **Today:** Selecting all text in a rendered comment or description requires the pointer to be over it. Copy then works from the keyboard. There is no keyboard focus model for the run.
- **Change:** Track a focused-run id (set on click, cleared on click elsewhere) in SelectableTextRun::Context and gate Ctrl+A on that instead of hover. Add a visible focus rectangle.

#### #13 — Settings fields put their example value in a hover tooltip instead of a visible placeholder

**Severity: Low** · Effort S · discoverability · daily · `hint-coverage-inconsistent`

- **Evidence:** SmatchetPreferencesUi.cpp:420-421, 452-453, 538-547 (InputText followed by SetItemTooltip with e.g. text); :424 Email and :481 Base URL have no placeholder.
- **Today:** The Jira Domain field relies on a hover tooltip (SmatchetPreferencesUi.cpp:421 "e.g. companyname.atlassian.net"), and the Plane URL, GitHub Base URL and Linear fields show none. Overall ratio is about 57 hint-less to 28 hinted single-line calls.
- **Change:** Convert the SetItemTooltip examples to InputTextWithHint placeholders on the same fields; keep tooltips for longer help.

### 7.2 Command palette, menu-bar command box and fuzzy matching

The command palette is a fixed 600x420 non-modal ImGui window (Ctrl+Shift+P) with a filter InputText and a Selectable list. Commands with required params switch to a plain text-field form; others dispatch via a latched deferred call. A menu-bar inline InputText ("Search commands, not issues") opens the palette on first keystroke and copies its text into the palette filter. Matching is a greedy ASCII subsequence scorer over Title+Name+Summary. Empty query shows up to 8 MRU recents followed by all commands. Rows show Title, Summary, dimmed id, and a right-aligned bound hotkey.

**Already good here:**

- Filter gets focus on first frame, including SetWindowFocus for the re-arm path (Source/Core/src/Commands/CommandPaletteUi.cpp:280 `if (ImGui::IsWindowAppearing() || focusFilterPending_)` then SetWindowFocus + SetKeyboardFocusHere)
- Dispatch is deferred outside Begin/End, avoiding window-stack corruption (CommandPaletteUi.cpp:173 `pendingDispatchName_ = cmd.Name;` drained at :254)
- Empty query shows persisted MRU recents first; Open() clears stale list and scope (CommandPaletteUi.cpp:90 `app.Commands().Recents(8)`; :38 `filtered_.clear()`)
- Rows show Title, dimmed id, and bound hotkey right-aligned; right-click offers Set shortcut (CommandPaletteUi.cpp:397-401 BoundHotkeyDisplay; :385 `Set shortcut...`)
- Menu-bar box placeholder says it searches commands, not issues, with the chord (Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:911 `Search commands, not issues (Ctrl+Shift+P)`)

#### #14 — When a palette command fails (bad argument), the palette closes, my typed arguments are gone, and I see no error

**Severity: High** · Effort S · validation · occasional · `palette-failure-closes-and-discards-args`

- **Evidence:** Source/Core/src/Commands/CommandPaletteUi.cpp:192-199 `if (!r.Ok) { LOG_WARN(...) } else {...}` followed unconditionally by `Close();`; arg form only checks `argFormBufs_[i][0] == '\0'` (:213)
- **Today:** User fills the arg form and presses Run. Registry validation (type, enum, range, MaxLen) fails; the failure goes only to the log, the palette closes, and the typed values are cleared (Close clears argFormBufs\_).
- **Expected:** VS Code / Raycast keep the input open, show the error inline beside the field, and preserve what was typed.
- **Change:** In drainPendingDispatch, on !r.Ok keep the palette open: re-enter argFormStep\_ with the old buffers retained, store r.Error.Message in a member and render it in red under the form (or toast for non-arg errors). Close only on success.

#### #15 — Scrolling the palette list with the mouse wheel snaps back to the selected row

**Severity: High** · Effort S · responsiveness · daily · `palette-scroll-recenter-every-frame`

- **Evidence:** CommandPaletteUi.cpp:403-405 `if (isSelected) { ImGui::SetItemDefaultFocus(); ImGui::SetScrollHereY(0.5f); }` runs every frame
- **Today:** The selected row is re-centered every frame, so wheel/scrollbar scrolling to browse the long empty-query list is undone immediately. It also recenters rather than doing minimal scroll-into-view on arrow keys.
- **Expected:** VS Code / Raycast scroll only when the selection changes, and by the minimum needed.
- **Change:** Track a `scrollToSelected_` flag set when selected\_ changes via keys/filter; call SetScrollHereY only when set, using the conditional ImGui::IsItemVisible()/SetScrollHereY(0.f) minimal-scroll idiom, then clear.

#### #16 — After arrowing down and then editing the query, the highlighted row stays at its old index instead of jumping to the best match

**Severity: High** · Effort S · keyboard · every session · `palette-selection-sticks-while-typing`

- **Evidence:** CommandPaletteUi.cpp:134-140 rebuildFiltered only clamps `selected_`; filterChanged path at :297-298 does not reset it
- **Today:** Arrow to row 5, type more letters: selection stays on index 5 of the new list, so Enter runs a lower-ranked command than the top hit.
- **Expected:** Every launcher resets selection to the top result whenever the query text changes.
- **Change:** In Draw, when filterChanged is true set `selected_ = 0` and the scroll flag before rebuildFiltered.

#### #17 — Commands with arguments open a bare form: the caret stays in the search box, fields are untyped with no hints, and Esc kills the whole palette

**Severity: Medium** · Effort M · focus · occasional · `palette-arg-form-untyped-unfocused`

- **Evidence:** CommandPaletteUi.cpp:202-245 drawArgForm uses `ImGui::InputText(label...)` for every param, ignores `ParamSpec.Type/Enum/Description/MaxInt` (Command.h:36-49); no SetKeyboardFocusHere in it; `IsKeyPressed(ImGuiKey_Escape, false)` -> Close() at :241-244; filter input still drawn at :294
- **Today:** After choosing a command, focus remains in the filter box; the user must click or Tab into the first arg. Enum params are free text, descriptions never shown, placeholders absent, required marked only with `*`. Esc discards everything rather than going back to the list. Enter submits from any field including the filter.
- **Change:** On entering arg form call SetKeyboardFocusHere once for field 0 and hide/disable the filter input; render InputTextWithHint using p.Description; render p.Enum via BeginCombo; show a disabled-Run reason tooltip; make Esc set argFormStep\_=-1 (back to list) instead of Close().

#### #18 — The palette has no close button and clicking outside it does nothing, so a mouse user must find Esc

**Severity: Medium** · Effort S · discoverability · daily · `palette-no-cancel-affordance`

- **Evidence:** CommandPaletteUi.cpp:268-270 `ImGuiWindowFlags_NoTitleBar ... ImGui::Begin("##cmdpalette", &open_, flags)` (no title bar means the p_open X is never drawn); no outside-click check; footer only says `Esc to close` (:322)
- **Today:** Window stays open when clicking the grid behind it; only Esc (or running a command) closes it. Mouse-only users see no X.
- **Change:** After Begin, if ImGui::IsMouseClicked(0) and !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows|AllowWhenBlockedByPopup) then Close(); optionally add a small X in the corner. Guard against the context-menu popup.

#### #19 — The palette list supports only Up/Down with no wrap; PageUp/PageDown, Home/End and Ctrl+N/P do nothing

**Severity: Medium** · Effort S · keyboard · daily · `palette-nav-keys-incomplete`

- **Evidence:** CommandPaletteUi.cpp:332-335 `IsKeyPressed(ImGuiKey_UpArrow, true) && selected_ > 0`; docs/guides/keyboard-shortcuts.md:120 lists only `Up / Down / Enter / Esc`
- **Today:** With ~100+ commands in the empty-query list, reaching the end takes many presses; Up at the top and Down at the bottom are dead.
- **Change:** Extend handleListNavigation: wrap at ends, PageUp/PageDown move by visible-rows count, Ctrl+Home/End jump; remember to enable the scroll flag. Home/End must stay as text-caret keys inside the input, so use Ctrl+Home/End. Update keyboard-shortcuts.md.

#### #20 — Typing a command's initials or a slightly misspelled word often ranks the wrong command or finds nothing, and matched letters are not highlighted

**Severity: Medium** · Effort M · parsing · every session · `palette-fuzzy-weak-ranking`

- **Evidence:** FuzzyMatch.cpp:10-12,51-72; CommandPaletteUi.cpp:119,126,373-374
- **Today:** Scoring takes the first left-most match so consecutive/word-start runs later in the string are missed; Summary prose is scored as one blob, so common letters match unrelated commands; no transposition/typo tolerance; recents and usage are ignored once a query exists; non-ASCII (the app ships French strings) is not case-folded; leading/trailing spaces in the query are matched literally; no match highlighting so users cannot see why a row matched.
- **Change:** Trim the query; score Title, Name and Summary separately and take weighted max (Title x3); replace greedy scan with a small DP (Smith-Waterman style) or try each start position; add a recency boost from Commands().Recents(); return match indices from FuzzyScore and draw the Title via segmented TextColored/DrawList on the Selectable. Folding to UTF-8 lowercase via a codepoint helper is optional.

#### #21 — A query that matches nothing shows a blank list with no message

**Severity: Medium** · Effort S · discoverability · daily · `palette-no-results-blank`

- **Evidence:** CommandPaletteUi.cpp:354 `for (int i = 0; i < filtered_.size(); ++i)` with no else branch; footer still says `Enter to run` (:322)
- **Today:** An empty list under the input; Enter does nothing silently; footer hint still advertises Enter-to-run.
- **Change:** After the loop, if filtered\_.empty() draw TextDisabled with the query and, when categoryPrefix\_ is set, a 'Show all commands' button; swap footer text.

#### #22 — Pressing Enter or clicking a destructive command without Shift silently does nothing

**Severity: Low** · Effort S · validation · occasional · `palette-destructive-silent-noop`

- **Evidence:** CommandPaletteUi.cpp:340 `if (!sel.Destructive || io.KeyShift)` and :376 `if (!c.Destructive || io.KeyShift)` with no else; hint only appears in the footer at :317
- **Today:** Enter or click on a destructive row without Shift does not run it; the only feedback is the red footer hint (:317-320) shown while the row is selected, which is easy to miss, and nothing pulses on the blocked keypress.
- **Change:** On blocked Enter/click set a `destructiveNudge_` timer that pulses the footer hint, or advance to a one-key confirm state ('Press Enter again to confirm') instead of requiring Shift.

#### #23 — Typing in the menu-bar command box teleports focus into the palette after the first keystroke, and later edits can leave a stale list

**Severity: Low** · Effort S · focus · daily · `menubar-box-focus-handoff-and-stale-list`

- **Evidence:** SmatchetUI_MainMenu.cpp:916-924; CommandPaletteUi.cpp:28,63-71,280-284,297; UX_DESIGN_CRITIQUE.md:178 (M7 Done for placeholder copy)
- **Today:** First edit in the menu-bar box opens the palette and moves focus to the palette's own filter box, so there are briefly two inputs with the same text; re-editing the inline box while the palette is open sets filterBuf\_ without invalidating filtered\_, so the list can be stale.
- **Change:** Have SetFilterText clear filtered\_ (or set a rebuild flag); optionally make the menu-bar control a launcher that calls Open() on click and carries no text input.

#### #24 — Closing the palette does not return focus to the grid cell or field I was in

**Severity: Low** · Effort S · focus · daily · `palette-close-no-focus-return`

- **Evidence:** CommandPaletteUi.cpp:47-54 Close() only clears state and unregisters dictation; no focus restore
- **Today:** After Esc the previously active pane or input is not explicitly refocused; keyboard users can land in no widget and must click.
- **Change:** In Open() record ImGui::GetFocusedWindow() (or ImGuiID of nav focus); on cancel call ImGui::SetWindowFocus(saved) the next frame. Skip when the command itself moves focus.

#### #25 — Empty-query state mixes recents with the alphabetical list with no section labels or hint what to type

**Severity: Low** · Effort S · discoverability · every session · `palette-recents-unlabelled`

- **Evidence:** CommandPaletteUi.cpp:87-109 recents then all appended into one `filtered_` vector (duplicates skipped); rows drawn identically (:354-407)
- **Today:** The first eight rows are recents but look the same as the rest; the full ~all-commands list follows with no headings.
- **Change:** Store recentCount\_ in rebuildFiltered and draw a SeparatorText('Recent') / SeparatorText('All commands') in drawCommandList when query is empty.

### 7.3 Grid keyboard navigation, selection and in-place cell editing

The Active Project grid is mouse-driven for editing. A value cell is a Selectable; a single click (default SingleClickToEditGridCells = true, ConfigManager.h:153) or a double click opens an inline InputText for plain text, or a popup for combo fields. Enter commits and Esc cancels. Focus loss also commits when the value changed, and the commit is queued to a serial background worker with per-cell Saving, Saved and Error badges. Grid-specific keyboard handling is limited to Ctrl+A (select all rows), Ctrl+C (TSV copy), Esc (clear selection) and Shift+Space (promote the active row). I found no arrow-key cell navigation, F2, type-to-edit, Tab or Enter move-to-next-cell, paste, undo or redo, or multi-cell edit in the grid files. Selection is row-based through the key column, and selected rows feed only copy, export and the AI assistant. The only keyboard movement is ImGui's generic NavEnableKeyboard. The new-issue row is a good keyboard path: it auto-focuses Summary and Enter submits.

**Already good here:**

- New-issue draft row is keyboard-friendly. It inherits fields from the last visible ticket, auto-focuses Summary on the first frame, and Enter submits when the gates are clear (Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:224 `d.newIssueFocusSummaryPending = true;` and :548-563 SetKeyboardFocusHere + EnterReturnsTrue + TrySubmitNewIssueDraft)
- Inline text edit has a consistent core contract: caret on the first frame, Enter commits, Esc cancels, and an unchanged value never fires a PUT. The dirty baseline is captured at edit start so a background sync cannot cause a spurious write (Source/Core/src/TicketFieldEditor.cpp:458-460 SetKeyboardFocusHere; :479 `valueChanged = state.EditInitialValue != ...`; :490-491 `if (escapePressed) state.ClearEditing(); // cancel - never PUT`)
- Commit is optimistic and off-thread, with per-cell Saving, Saved, Error and queued-offline badges and tooltips, so typing never blocks on the network (Source/Core/src/SmatchetGridFieldEditPipeline.cpp:298-302 `feedback.State = CellWriteState::Saving; ... "Saving to Tracker..."`; Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp:394-398 and :380-391)
- Selection model has Excel-style row gestures (click, Shift range, Ctrl toggle, drag), Ctrl+A, Ctrl+C as TSV, and Shift+Space. These are guarded by !io.WantTextInput so text fields keep their own shortcuts (Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp:678 `windowFocused && !io.WantTextInput && effCtrl && ... ImGuiKey_A`; :544-583 shift/ctrl/drag row gestures)
- Single-click versus double-click edit is a user preference, and the truncation guard refuses to overwrite a value longer than the inline buffer (Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp:36 `Checkbox("Single-click to edit grid cells"...)`; Source/Core/src/TicketFieldEditor.cpp:484-489 inline_too_long tooltip)

#### #26 — After I press Enter to commit a cell, nothing moves on: no Enter-down or Tab-right to the next cell

**Severity: High** · Effort M · steps · every session · `grid-no-commit-and-move`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:493-495 `QueueEdit(...); state.ClearEditing();` and :471 only Escape is read. There is no ImGuiKey_Tab or ImGuiKey_Enter move logic and no ImGuiInputTextFlags_AllowTabInput.
- **Today:** Enter commits and the editor closes with nothing focused. To edit the cell below I must click it. Tab leaves the InputText through ImGui's generic nav, which deactivates and commits, but it does not open the next cell's editor.
- **Expected:** Spreadsheets and Jira bulk tables: Enter commits and moves down, Shift+Enter moves up, Tab and Shift+Tab move across and open the editor, Esc stays put.
- **Change:** In RenderTextInlineEdit (TicketFieldEditor.cpp:434), after a commit triggered by Enter or Tab, set a pending-advance request (next row or column) on SpreadsheetState. The grid loop (drawActiveProjectGridValueCell) consumes it and calls StartEditingField on the neighbour. Intercept Tab with ImGuiInputTextFlags_CallbackCompletion or check IsKeyPressed(ImGuiKey_Tab) while the item is active. Skip read-only cells.

#### #27 — I can select many rows but cannot edit them together: no fill, set-for-selected or paste across the selection

**Severity: High** · Effort L · bulk · daily · `grid-no-bulk-edit`

- **Evidence:** Source/Core/include/SpreadsheetState.h:70-71 single `EditingId` and `EditingFieldId`; RectSel.Rows is consumed only by copy, AI and export (SmatchetAiAssistantUi.cpp:884, SmatchetBulkTicketsUi.cpp:664, SmatchetGridUiSupport.cpp:168-192). The cell context menu offers only Copy items (SmatchetGridUiSupport.cpp:239-323).
- **Today:** Selecting 20 rows and wanting Status = Done means 20 separate cell edits, each with its own worker round trip and toast. Selection is decorative for writes.
- **Expected:** Linear and Jira: select rows then change a field once. Sheets: fill-down with Ctrl+D, or paste one value over a range.
- **Change:** When the edited cell's row is inside RectSel.Rows, offer 'Apply to N selected' on commit: a one-line popover or an Alt+Enter variant that calls QueueEdit per selected issue id in one batch. Add a 'Set &lt;column> for N rows...' entry in DrawGridCellRightClickPopups, plus Ctrl+D fill-down, backed by one coalesced undo entry.

#### #28 — No undo for committed grid edits and no paste onto a non-editing cell or range (Ctrl+V works only inside the open editor)

**Severity: High** · Effort L · lost-work · daily · `grid-no-paste-no-undo`

- **Evidence:** Grep of Source/Core/src/\*Grid\* for paste, GetClipboardText, ImGuiKey_V, undo and redo finds no grid code (only unrelated comments). Ctrl+C is wired at SmatchetActiveProjectGridCells.cpp:682-684; no V or Z exists. A committed edit goes straight to QueueEdit (TicketFieldEditor.cpp:494).
- **Today:** A committed edit goes straight to QueueEdit with no undo. Ctrl+V pastes only into an already-open inline editor (native InputText); there is no paste onto a selected cell or range.
- **Expected:** Optimistic commit with an undo toast (Linear, Gmail) or Ctrl+Z. Sheets-style Ctrl+V into a cell or range.
- **Change:** Record {issueId, fieldId, originalValue, newValues} at QueueEdit, since OriginalValue is already passed (TicketFieldEditor.cpp:494). Keep a bounded per-session undo stack and handle Ctrl+Z in drawActiveProjectGridRectSelKeys by re-queuing the inverse edit. Show 'Saved - Undo' on the success toast. Add Ctrl+V for the active cell, parsed through the field's normal validation path.

#### #29 — Keyboard can open a cell only via generic ImGui nav; no active cell, F2 or type-to-edit

**Severity: Medium** · Effort L · keyboard · every session · `grid-no-keyboard-edit-start`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:586-588 `ShouldOpenCellEditorOnGesture(textCellClicked, singleClickToEdit)` then `state.StartEditingField(...)`; Source/Core/include/TouchCellEditGesture.h:58 `cellSelectableClicked && (openOnClick || IsMouseDoubleClicked(0))`. The only keys in the grid are Ctrl+A, Ctrl+C, Esc and Shift+Space (SmatchetActiveProjectGridCells.cpp:678-693). No F2 or arrow handling exists in any Grid file.
- **Today:** By default (single-click-to-edit on) ImGui's generic keyboard nav can focus a cell Selectable and Enter/Space opens the editor, but there is no grid active-cell concept, no F2, no type-to-replace, and the nav path only works through generic widget traversal; with single-click-to-edit disabled keyboard cannot open an editor at all.
- **Change:** Add an active-cell cursor (row index plus column) to SpreadsheetState, updated in drawActiveProjectGridRectSelKeys (SmatchetActiveProjectGridCells.cpp:636). Handle arrows, Home/End and PageUp/PageDown, F2 and Enter, gated on windowFocused && !io.WantTextInput. On F2/Enter call state.StartEditingField for the active cell, reusing the existing editors. For type-to-edit, read io.InputQueueCharacters and seed EditBuffer. Draw a focus rectangle on the active cell.

#### #30 — Clicking a value cell clears my selection and opens the editor, so I can never select a cell range, only whole rows

**Severity: Medium** · Effort M · keyboard · daily · `grid-cell-range-select-dead`

- **Evidence:** Source/Core/src/Ui/SmatchetActiveProjectGridCells.cpp:586-589 `else { sel.ClearAll(); ... }` for non-key cells. The only drag branch for cells sets ExtentRow (:600) but AnchorRow is never assigned anywhere outside ClearAll (SpreadsheetState.h:22,56). The Shift+Space handler still reads sel.AnchorRow (:71).
- **Today:** The comment says 'Google-Sheets-style' but cell ranges cannot be started. A click on a value cell drops any selection, and with single-click-to-edit on it also opens an editor. Row selection works only through the key column. Ctrl+C therefore copies whole rows, never a column slice.
- **Change:** Separate select from edit. In handleActiveProjectCellRectSel, a plain click on a value cell sets AnchorRow and AnchorCol and sel.Active, and starts a drag. Open the editor on double-click, Enter or F2 only, or on a click of an already-selected cell. Shift+click and Shift+arrow extend the range. This pairs with the active-cell cursor in grid-no-keyboard-edit-start.

#### #31 — When a save fails, my typed text is gone and I only get a small red '!' with a tooltip

**Severity: Medium** · Effort M · validation · occasional · `grid-failed-commit-loses-typed-value`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:494-495 `QueueEdit(...); state.ClearEditing();` happens before the result is known. Failure is only `feedback.State = CellWriteState::Error; ... Message = d.gridEditError` (SmatchetGridFieldEditPipeline.cpp:112-116), rendered as badgeText "!" with the message in a hover tooltip (SmatchetActiveProjectGridCells.cpp:380-385).
- **Today:** A rejected value closes the editor, an 'Active Project' error toast shows the message once, and the cell keeps a small red '!' with a hover tooltip. The typed text is gone and the user must reopen and retype.
- **Change:** Keep the rejected value in CellWriteFeedback and let the '!' badge (or reopening the cell) reseed the editor with it plus the message inline; the toast already exists so no extra toast is needed.

#### #32 — A background sync silently wipes my row selection, and a re-sort mid-gesture invalidates it

**Severity: Medium** · Effort M · lost-work · daily · `grid-sync-clears-selection`

- **Evidence:** Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp:1120-1123 `gridSortSig = ComputeGridSortSignature(pane.cachedSortFingerprint, pane.snapshotRevision, tickets.size()); if (... SortSignature != gridSortSig) { RectSel.ClearAll(); }`. Selection is stored as sort-order row indices (SpreadsheetState.h:16).
- **Today:** Because snapshotRevision is part of the signature, any sync or merged edit that bumps the revision clears a half-built multi-row selection. The user then has to redo the Shift or Ctrl selection before copy or other actions.
- **Change:** Store selected rows as issue ids (and keep indices as a derived cache). On a signature change, re-map ids to the new filteredIndices and drop only missing ones, instead of RectSel.ClearAll().

#### #33 — Clicking elsewhere or the window losing focus commits a half-typed edit instead of asking or keeping it

**Severity: Medium** · Effort S · lost-work · daily · `grid-edit-focus-loss-commits-silently`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:465-468 `deactivated = !editJustStarted && ImGui::IsItemDeactivated();` and :492-494 commit on deactivated with valueChanged. Esc is the only cancel (:471). Rect-select clicks elsewhere fire the same path.
- **Today:** Any stray click, including on another cell, scrollbar or sort header, writes the typed text to the tracker. There is no distinction between click-away-to-save and click-away-by-accident, and a per-cell edit is not recoverable (see grid-no-paste-no-undo). Rows re-sorting under the editor do not reconcile the buffer with the new row position beyond the dirty baseline.
- **Change:** Keep blur-commit, but pair it with the undo stack (grid-no-paste-no-undo) and show 'Saved - Undo' for about 5 seconds. Optionally add small check and cross buttons beside the editor for pointer users.

#### #34 — Every edit pops a 'Field update saved to Tracker' toast and edits save one at a time, which gets noisy in a run of edits

**Severity: Medium** · Effort S · responsiveness · daily · `grid-toast-and-serial-queue-per-edit`

- **Evidence:** Source/Core/src/SmatchetGridFieldEditPipeline.cpp:97-100 `Push("Success", "Field update saved to Tracker.", ToastType::Success)` on each commit; :122 `d.hasInFlightEdit = false;` and :288 `d.hasInFlightEdit = true;` show one in-flight edit at a time.
- **Today:** Editing ten cells in a row yields ten success toasts on top of the per-cell 'OK' badge, and each cell waits for the previous HTTP round trip. Per-cell badges already carry the same information.
- **Change:** Drop the success toast in ApplyCommitResultOnUiThread for SavedOnline (keep the cell badge) and keep toasts for Error and queued-offline. Where the tracker allows it, coalesce consecutive edits to the same issue into one PUT.

#### #35 — A cell that cannot be edited looks like one that can, and I am not told why

**Severity: Low** · Effort S · discoverability · daily · `grid-readonly-cell-no-affordance-or-reason`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:1258-1260 `if (!allowEdits) { renderPlainText(true); return true; }` and :1255 `renderPlainText(column.CatalogReadOnly)`; no read-only hint text or cursor change appears in these paths.
- **Today:** Disabled plain text is drawn and a click does nothing. Whether it is read-only because of the tracker, the project, the issue state or the global Read-only Mode is not stated at the cell.
- **Change:** In RenderFieldCell, when !allowEdits and the cell is hovered, call ImGui::SetMouseCursor(ImGuiMouseCursor_NotAllowed) and show a tooltip. The reason comes from the existing CanEditFieldForIssue and readOnlyMode inputs. Add a lock glyph to read-only column headers.

#### #36 — Long values cannot be edited inline: the buffer is 512 bytes and the commit is refused with a tooltip

**Severity: Low** · Effort S · validation · rare · `grid-inline-edit-512-byte-cap`

- **Evidence:** Source/Core/include/SpreadsheetState.h:76 `char EditBuffer[512] = "";`; Source/Core/src/TicketFieldEditor.cpp:484-489 `loadedTruncated` blocks the commit; message 'Value too long to edit inline ... Edit it elsewhere.'
- **Today:** Only plain single-line values of 512 bytes or more open inline; typing is accepted, but Enter or blur closes the editor without saving, with the reason shown only in a hover tooltip. ADF/description fields already go to the modal.
- **Change:** Detect InlineEditLoadedTruncated at edit start in StartEditingField. Instead of opening the inline editor, route the cell to the long-text modal (OpenLongTextEditor already exists) or show the message inline under the field rather than on hover.

### 7.4 Typed field editors: dates, durations, labels, users, status, worklog

Typed field editors live in the grid: single-select combos (status, priority, assignee, components) open an arm-then-popup combo with a filter box. Labels use a checkbox combo with separate Search and Add inputs. Durations use an InputText plus a suggestions popup. Dates use a typed M/D/YYYY box plus a mouse-driven calendar popup with Apply/Clear/Cancel. Worklog is a modal dialog (duration, remaining, date, description) that saves on a worker. Long text uses a modal with Ctrl+Enter and Esc. No user-specific picker with assign-to-me or recents turned up in the files read. The checkpoint file is at build/input-ux-audit/results/audit-field-editors.md (the file holds a condensed list; this structured result is the full version).

**Already good here:**

- Single-select combos (status, priority, etc.) auto-focus a filter box on open; Enter commits the first match and closes (Source/Core/src/TicketFieldEditor.cpp:647-653 "if (justOpened) { ImGui::SetKeyboardFocusHere(); }" and :691 "if (submitOnEnter && firstMatch != nullptr)")
- Duration cell supports type-to-edit: the first printable keystroke is spliced into the buffer so no characters are dropped when the suggestions popup holds focus (Source/Core/src/TicketFieldEditor.cpp:250-273 "SpliceTypedCharsIntoBuf")
- Worklog save is async with a duplicate-submit guard across dialog re-opens and a visible 'Saving worklog...' cue (Source/Core/src/TicketFieldEditor_Worklog.cpp:399-400 and :519-522 "Saving worklog...")
- Status combo is honest about data freshness (live, saved workflow, offline) with a cue row (Source/Core/src/TicketFieldEditor.cpp:704-721 "Valid moves as last fetched (offline)")
- Long-text modal has the modern contract (Ctrl+Enter save, Esc cancel) with the hint printed in the footer (Source/Core/src/TicketFieldEditor_Modal.cpp:495-501 "Ctrl+Enter to save · Esc to cancel")

#### #37 — Opening Log work gives no caret and no keyboard submit, so every worklog needs a mouse click into the field and a click on Save

**Severity: High** · Effort S · focus · daily · `worklog-dialog-no-focus-no-ctrl-enter`

- **Evidence:** Source/Core/src/TicketFieldEditor_Worklog.cpp:376-406 OpenWorklogDialog sets JustOpened but never requests focus; :500 "if (ImGui::Button(\"Save\", ImVec2(80, 0)))" is the only submit; no IsKeyPressed(Enter) anywhere in the file
- **Today:** Click Log work, modal opens, no field is active. The user clicks Time spent, which opens a suggestions popup when the buffer is empty. They type, click Date and description if needed, then click Save. Ctrl+Enter does nothing.
- **Expected:** Linear / Jira quick-log: dialog opens with the caret in Time spent, Enter in the duration field or Ctrl+Enter anywhere submits, Esc cancels.
- **Change:** In RenderTimeTrackingModal, call ImGui::SetKeyboardFocusHere() on the first frame after OpenPopup for ##WorklogTimeSpent (via a JustFocused flag). Add a Ctrl+Enter check that calls HandleWorklogSave, and Enter on the single-line duration (it already returns on EnterReturnsTrue at TicketFieldEditor.cpp:282). Print the hint beside Save.

#### #38 — Typing a natural duration like 1.5h or 90 m is rejected, and the error never says which part is wrong

**Severity: High** · Effort M · parsing · daily · `worklog-duration-parser-strict`

- **Evidence:** Source/Core/src/Tracker/TrackerFieldValueParser.cpp:672-740 digit run then single unit char, "else { break; }" on '.' or ' ' (no decimal branch, no space-before-unit); Source/Core/include/Ui/SmatchetWorklogSubmitPure.h:24-25 "Invalid Time spent format. Please use e.g. 2h 30m."
- **Today:** "1.5h" parses to 0 seconds (stops at '.'), "90 m" stops at the space, bare "90" means 90 seconds (not minutes). Failure shows one generic red line only after Save is clicked. The hint under the field is a static line (Worklog.cpp:330).
- **Expected:** Toggl/Jira Tempo style: 1.5h, 90m, 1h30, 1:30 all accepted, a bare number read as minutes or hours by preference, and a live parsed preview such as '= 1h 30m' under the field.
- **Change:** Extend ParseWorkDurationToSeconds to accept decimals (number then h/d/w), optional whitespace before the unit, h:mm, and a configurable meaning for bare numbers. In DrawWorklogTimeInputs add a TextDisabled parsed preview or red hint while typing, and disable Save with a tooltip when the parse is &lt;= 0. Keep the pure parser testable in bucket A.

#### #39 — Pressing Esc or the X in the description editor throws away everything typed with no warning or undo

**Severity: High** · Effort M · lost-work · daily · `longtext-modal-esc-discards-draft`

- **Evidence:** Source/Core/src/TicketFieldEditor_Modal.cpp:497-498 "const bool escPressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);" ... "bool cancel = escPressed;" and :698-703 comment "the same discard semantics as Esc and the Cancel button"; grep for dirty/unsaved found nothing
- **Today:** Esc, X and Cancel all close the editor and drop the buffer immediately. A stray Esc after a long edit loses it, and re-opening reloads the saved text.
- **Expected:** Notion/GitHub: Esc with unsaved changes asks 'Discard changes?' or keeps a draft that restores on re-open (VS Code hot exit).
- **Change:** Track dirty = buffer != loaded snapshot. On cancel when dirty, either show a small confirm popup (Discard / Keep editing) or stash the buffer per issueId+field in a session map and restore it with a 'Restored draft' banner. Do it in DrawLongTextFooter's cancel branch.

#### #40 — The date box only understands M/D/YYYY, so typing tomorrow, fri or 2026-11-20 does nothing and gives no message

**Severity: High** · Effort M · parsing · daily · `date-text-only-us-format`

- **Evidence:** Source/Core/src/Tracker/TrackerDateTimePure.cpp:116-118 "std::sscanf(s.c_str(), \"%d/%d/%d\", &m, &d, &y) == 3"; Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:626-635 on parse failure nothing happens (no error, valueChanged stays false)
- **Today:** Any text that is not M/D/YYYY is ignored with no message. dateBuf is rebuilt from the stored value each frame, so unparsed typed text is lost once the field loses focus.
- **Expected:** Linear/Todoist/Fantastical: type tomorrow, next fri, +3d, 2026-11-20 or 20/11 and see a resolved date preview; locale-aware order.
- **Change:** Add a pure ParseFlexibleDate(text, today, localeOrder) in TrackerDateTimePure (ISO, M/D, d/m by locale, today/tomorrow/yesterday, weekday names, +Nd/+Nw) and use it in RenderGenericDatePicker. Keep the typed text in a persistent per-widget buffer while active, show red hint 'Try 2026-11-20 or tomorrow' on failure, and commit on Enter/deactivate.

#### #41 — Adding a new label means finding a second 'Add label' box; typing in the search box and pressing Enter does nothing and the box is not focused on open

**Severity: Medium** · Effort S · steps · daily · `labels-editor-two-boxes-no-create`

- **Evidence:** Source/Core/src/Tracker/TrackerLabelsEditor.cpp:60-66 "InputTextWithHint(\"##LabelSearch\", \"Search labels\"...)" no SetKeyboardFocusHere and no EnterReturnsTrue; :98-100 separate "##NewLabelDraft" / "Add label" input plus Add button
- **Today:** Open combo, click Search labels, type, see '(no matching labels)', then retype the same text into Add label and press Enter. Commas are not split. Each checkbox toggle queues an edit immediately (:81-93). Backspace does not remove the last label.
- **Change:** Make DrawLabelsComboBody focus ##LabelSearch on the open frame (same pattern as TicketFieldEditor.cpp:647), add EnterReturnsTrue so Enter toggles the first visible suggestion, and when no exact match exists draw a 'Create "&lt;text>"' Selectable that queues the new label. Drop the second input or keep it only as a fallback.

#### #42 — Label autocomplete only knows labels already on tickets currently loaded in the grid, so a valid but unused label looks nonexistent

**Severity: Medium** · Effort M · parsing · daily · `label-suggestions-only-loaded-tickets`

- **Evidence:** Source/Core/src/Tracker/TrackerLabelsEditor.cpp:20-37 CollectLabelSuggestions iterates "app.GetActiveTicketsSnapshot()" and "appendLabels(candidate.GetFieldValue(\"labels\"))"; empty case text "(no labels discovered yet)" at :71
- **Today:** Suggestions are mined from the visible tickets. In a narrow view or a fresh pane the list is empty and the user can easily create a near-duplicate (case differences are caught by ContainsLabelCaseInsensitive, typos are not).
- **Change:** Feed CollectLabelSuggestions from a cached tracker label lookup (like the user roster lookup cache in AppController_CatalogAndFieldEdit.cpp) plus a per-user recent-labels list, and fall back to the ticket scan. Use a typo-tolerant match for 'did you mean'.

#### #43 — Components / multi-select combos open with no caret in the search box, and every option filter is plain substring matching

**Severity: Medium** · Effort S · focus · daily · `multiselect-search-not-focused-and-filter-substring-only`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:867-868 "InputTextWithHint(\"##MultiSelectSearch\"..." without SetKeyboardFocusHere (single-select does at :647-649); Source/Core/include/TicketFieldEditorOptionFilterPure.h:23-33 "ToLowerAsciiCopy(option.Value).find(filterLower)"
- **Today:** Single-select auto-focuses the filter; multi-select and labels do not, so the same gesture behaves differently per field. Filtering finds only contiguous substrings: 'prg' will not find 'Progress'; non-ASCII names are lower-cased ASCII only. Enter in the multi-select search does nothing.
- **Change:** Mirror the justOpened focus in the multi-select body, add Enter-toggles-first-match, and replace the substring test in OptionMatchesFilter with the shared fuzzy scorer (pure, bucket-A testable) with Unicode-aware lowering.

#### #44 — The calendar popup has no Today or quick-pick shortcut and no day-grid navigation; time is three drag fields

**Severity: Low** · Effort M · keyboard · daily · `calendar-mouse-only-no-today`

- **Evidence:** Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:163-183 "&lt;&lt;", "&lt;", ">", ">>" buttons, :199 "ImGui::SmallButton(std::to_string(d).c_str())" per day, :302-308 "ImGui::DragInt(\"##h\", &h, 0.25f, 0, 23"; no Today button found (only TodayUtcParsed as a seed at :602)
- **Today:** Keyboard nav is generic ImGui nav only: arrows hop between buttons, with no PageUp/PageDown for month, no T for today, no Today/Tomorrow/Next week chips. Hour/minute/second are DragInt fields (:302-308), awkward for typing exact values, and the time is labelled 'Time (UTC wall)'.
- **Change:** In DrawCalendarPicker add quick-pick buttons row, handle ImGuiKey_LeftArrow/RightArrow/UpArrow/DownArrow/PageUp/PageDown/Enter/T while the popup is focused, and swap the three DragInt fields for InputInt-style text fields (or one HH:MM text box like the existing ##FriendlyTime).

#### #45 — Pressing Enter in the status/priority filter with an empty box silently commits the first option

**Severity: Low** · Effort S · validation · occasional · `single-select-enter-empty-commits-first`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:689-700 "the top one is committed as a least-surprise default"; firstMatch is the first row when filterLower is empty (:657-667)
- **Today:** Open the combo, tap Enter without typing, and the first status (or first priority) is queued as an edit. For status that can fire a workflow transition.
- **Change:** Guard the Enter branch with !filterLower.empty(); draw the first match with a highlight (or SetKeyboardFocusHere target / Selectable selected) so Enter's target is visible.

#### #46 — The duration suggestions popup opens on first focus of an empty field and is a fixed list that does not follow what is typed

**Severity: Low** · Effort M · discoverability · daily · `duration-suggestions-popup-unfiltered-steals-focus`

- **Evidence:** Source/Core/src/TicketFieldEditor.cpp:151-163 ComputeDurationPopupShouldOpen opens when "buf[0] == '\\0'"; :171-196 DrawDurationSuggestionsPopup loops LoadDurationSuggestions() with no filtering by buf
- **Today:** A popup covers part of the dialog on first focus; typing closes it only via the type-to-edit path (grid) and the worklog call passes typeToEditFocus=false (Worklog.cpp:312-313), so the user must click into the field again after the popup grabs focus.
- **Change:** Filter the suggestions by the parsed current text, keep focus in the InputText (draw as a non-focus popup via ImGuiWindowFlags_NoFocusOnAppearing), and show the parsed total as a hint.

#### #47 — Empty date pickers seed from UTC today, and the worklog resets duration and description on every open with no recents

**Severity: Low** · Effort S · steps · occasional · `date-defaults-utc-and-no-last-values`

- **Evidence:** Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:600-603 "parsed = TodayUtcParsed(isDateTime);"; Source/Core/src/TicketFieldEditor_Worklog.cpp:379 "TimeSpent[0] = '\\0'" reset on every open
- **Today:** An empty date field seeds from UTC, not local, so evenings in far time zones show tomorrow or yesterday. Worklog starts blank each time with no recent-values chips or last-used description.
- **Change:** Use a local-time TodayLocalParsed for seeding, and keep a small last-used duration/description list shown as quick chips in the worklog dialog.

### 7.5 Issue creation and long-form composing

Issue creation has three entry points. (1) The inline grid draft row ("+ New Issue" in SmatchetNewIssueDraftUi.cpp) seeds from the last visible ticket, autofocuses Summary, and submits on Enter in Summary or via a Create button. The description there is a 7-line multiline box inside a grid cell. (2) The Quick Create popup has project, type, summary and description, with Ctrl+Enter and Esc handled. (3) Bulk import is a path field plus Paste, then Parse preview, then Run import. The comments composer is a 16 KB multiline box with a Post button inside a viewer shell. Attachments come only from a file dialog. Bug report has the best keyboard flow of the group. Drafts live only in memory. There are no issue templates, mentions, markdown preview in composers, image paste or drag-drop attach.

**Already good here:**

- Inline draft: Summary autofocuses on the first frame and Enter submits; Cancel only confirms when the draft has content (Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:548-564 `ImGui::SetKeyboardFocusHere()` / `EnterReturnsTrue`; :380-386 `NewIssueDraftHasUserContent`)
- Option combos have an autofocused filter box where Enter picks the first match (Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:458-463,509-512 `submitOnEnter && firstMatch != nullptr`)
- Quick Create has Ctrl+Enter submit, Esc cancel, autofocus on Summary, and project/type seeded from the last ticket. Offline-queue fallback exists (Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp:310 `ctrlHeld && ImGui::IsKeyPressed(ImGuiKey_Enter, false)`; :86-90 `BuildDraftFromLastTicket`; :350-351)
- Bug report has the full modern flow: autofocus, Ctrl+Enter, Esc (Source/Core/src/Ui/SmatchetBugReportUi.cpp:311-312,344,385)
- Bulk import previews first, retries only failed rows, and has Stop that preserves results (Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:434-439 `NEVER requeue rows that already succeeded`; :472 `ImGui::Button("Stop")`)

#### #48 — Writing a comment means clicking into the box first, then clicking Post; there is no Ctrl+Enter, and an empty Post click does nothing

**Severity: High** · Effort S · keyboard · daily · `comment-composer-no-focus-no-ctrl-enter`

- **Evidence:** Source/Core/src/Ui/SmatchetCommentsModalUi.cpp:554-562 `InputTextMultiline("##CommentsPostBox"...)` ... `ImGui::Button("Post Comment"...) && hasText`; no SetKeyboardFocusHere or IsKeyPressed in the file
- **Today:** Open Comments (viewer shell). Caret is not in the post box. User clicks the box, types, then must click Post Comment. Ctrl+Enter inserts nothing and submits nothing. With empty text the enabled button silently does nothing.
- **Expected:** Linear, GitHub and Jira: the composer is focused on open, Ctrl/Cmd+Enter posts, and the button shows the shortcut.
- **Change:** In DrawCommentsPostBox, call ImGui::SetKeyboardFocusHere() on the first frame after OpenCommentsModal (add a pending-focus bool to CommentsModalState). After the InputTextMultiline, if it is active and Ctrl+Enter is pressed (IsKeyChordPressed(ImGuiMod_Ctrl|ImGuiKey_Enter)) and hasText, call KickCommentPost. Disable the Post button while empty and label it 'Post Ctrl+Enter'.

#### #49 — In the inline new-issue row, Tab in the description types a tab character, and nothing submits or cancels from the keyboard once you leave Summary

**Severity: High** · Effort S · keyboard · every session · `draft-description-tab-trap-no-submit-key`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:530-531 `InputTextMultiline("##input"... ImGuiInputTextFlags_AllowTabInput)`; file has no Escape or Ctrl+Enter handling (only EnterReturnsTrue at :552 for Summary)
- **Today:** Type Summary, press Tab to the description: Tab then inserts a literal tab and cannot be used to move on. Create and Cancel are buttons with no shortcut; Enter submits only from Summary. Esc does nothing; the draft row stays open.
- **Expected:** Tab moves on and Ctrl+Enter submits from any field (Linear, Jira). Esc cancels, with a confirm if the draft has content.
- **Change:** Drop AllowTabInput on the draft description (or use a callback that treats Shift+Tab/Tab as nav). Add a draft-level handler in DrawDraftIdColumnCell or RenderNewIssueDraftRow: Ctrl+Enter calls TrySubmitNewIssueDraft, Esc runs the existing discard flow (open the 'Discard draft?' popup when NewIssueDraftHasUserContent). Show 'Ctrl+Enter' in the Create button tooltip.

#### #50 — Quick Create: one Esc discards typed summary and description with no confirm, and Enter in the summary does not submit

**Severity: Medium** · Effort S · lost-work · daily · `quick-create-esc-wipes-and-global`

- **Evidence:** Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp:313-315 `cancelClicked || ImGui::IsKeyPressed(ImGuiKey_Escape, false)` -> ResetAndClose (:129-137 clears bufs); :353 InputText has no EnterReturnsTrue
- **Today:** Esc anywhere while the popup is visible, not gated on the window being focused or on text being active, closes it and clears both buffers. The user types a summary and presses Enter, and nothing happens. They must use Ctrl+Enter or the button.
- **Change:** Gate the Esc/Ctrl+Enter block on ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows). If a summary or user-edited description exists, route Esc to a discard confirm or stash the buffers into a per-session 'last quick-create draft' restored on the next open. Add EnterReturnsTrue to ##quickcreate_summary and submit when canSubmit.

#### #51 — A half-written new issue or comment exists only in memory; Esc, a crash or closing the window loses it

**Severity: Medium** · Effort M · lost-work · occasional · `draft-not-persisted`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:369-379 discardDraft resets `d.newIssueDraft = IssueDraft{}`; SmatchetCommentsModalUi.cpp:621 PostBuf reassigned on open; grep for draft Save/Persist/Restore in Source found no match
- **Today:** Drafts live in UiDrawSession and static CommentsModalState. Closing the app, a crash, a view change (auto-discard with a toast) or confirming Discard erases the text. Only a failed create falls back to the offline queue.
- **Change:** Debounce-save IssueDraftHelpers::ToJson (IssueDraft.cpp) of the active draft, the quick-create buffers and per-issue comment text to a small drafts file or SQLite table, keyed by project or issue id, and offer 'Restore draft' on open. In ImGui, write when the InputText returns true plus a 1 s timer.

#### #52 — After an issue is created the draft closes; filing several issues in a row needs a fresh click each time

**Severity: Medium** · Effort S · steps · daily · `no-create-another`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:165-173 `d.newIssueDraftActive = false; ... d.gridEditSuccess = "Created " + r.IssueKey + "."`; :206 `SmallButton("+ New Issue")`
- **Today:** Create succeeds, the row collapses and a banner reads 'Created KEY.'. For the next issue the user re-clicks '+ New Issue' and re-finds the field. Quick Create also closes on success (QuickCreateIssueUi.cpp:152).
- **Change:** Add a 'Create another' checkbox or Ctrl+Shift+Enter chord to the draft row and Quick Create. On success, when set, reseed via BuildDraftFromLastTicket, set newIssueFocusSummaryPending = true and keep the project and type. Persist the checkbox in cfg.

#### #53 — Required-field errors appear only after clicking Create and stay red after the user fixes the field

**Severity: Medium** · Effort S · validation · daily · `required-errors-submit-only-and-stale`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:98-105 missing set computed only in TrySubmitNewIssueDraft; edit handlers at :532-534 and :555-557 clear only queueFallback flags; Create disabled only for project at :340-352
- **Today:** Leave a required field empty, click Create: banner 'Missing required fields: ...' plus red FrameBg and '(required)'. Fill the field: the red state and '(required)' remain until the next Create. Create gives no hint about missing fields beforehand.
- **Change:** Recompute `missing` each frame from IssueDraftHelpers::MissingRequiredFields (memory-only, cheap) once the user has attempted submit, or erase the field id from d.newIssueMissingFieldIds when its edit handler fires. Extend the Create tooltip beyond 'Pick a project' to list missing field names.

#### #54 — Attachments can only be added through a file dialog: no drag-drop onto a draft, no pasted screenshots, and none in comments

**Severity: Medium** · Effort M · steps · daily · `attachments-file-dialog-only`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:401 `Button("Add attachment...")` -> RequestOpenFilePaths; Source/Standalone/StandaloneBoot_detail.cpp:108-127 drop callback keeps only .md/.markdown for Plan Docs; SmatchetCommentsModalUi.cpp has zero Attachment/Paste/clipboard matches
- **Today:** To attach a screenshot, save it to disk, click Add attachment, browse and select. Dropping a file on the window is ignored for drafts. Ctrl+V of an image does nothing. Comments cannot carry files.
- **Change:** Extend MarkdownFileDropCallback (glfwSetDropCallback is already installed) to route non-.md paths to TryAppendStagedFromAbsPath when a draft is active. Add a Ctrl+V handler on the description and comment boxes that reads CF_DIB through a Win32 helper beside Win32PickFiles.cpp, writes a temp PNG and stages it. Standalone only; the DX12 plugin has no GLFW drop.

#### #55 — The description box is plain text: no issue templates, no formatting shortcuts, no live preview, and no @mentions in descriptions or comments

**Severity: Medium** · Effort L · discoverability · daily · `no-templates-preview-mentions`

- **Evidence:** Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:529-537 InputTextMultiline plus static hint 'Markdown: \*\*bold\*\*, \*em\*...'; Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp:224 templates are worklog/comment text only; MarkdownPreviewRender used only to render existing comments (CommentsModalUi.cpp:358-369); Mention appears only in MarkdownToAdf.cpp/AdfToMarkdown.cpp/LinearClient.cpp
- **Today:** A new issue starts with an empty description. The user types markdown blind and cannot see the rendering until after creation. Typing '@name' is raw text and gets no autocomplete.
- **Change:** Phase 1: a 'Template' combo next to Summary populated from a cfg list (pattern from Prefs Templates tab) that fills the description buffer. Phase 2: a Preview toggle that renders the buffer with MarkdownPreviewRender::RenderPlan. Phase 3: an InputText callback (CallbackEdit/CallbackCompletion) that opens a user popup on '@' using the cached user catalog and Ctrl+B/I wrappers. Rich-text WYSIWYG is not feasible in ImGui.

#### #56 — A comment that is half-written is lost when the comments window closes, switching issues, or the app exits

**Severity: Medium** · Effort S · lost-work · daily · `comment-draft-lost-on-close`

- **Evidence:** Source/Core/src/Ui/SmatchetCommentsModalUi.cpp:597-603 `HasUnsavedInput`/`Discard comment?` confirm; :614-621 `s_CommentsState = CommentsModalState{}` and PostBuf reassigned on every open
- **Today:** Close prompts 'Discard comment?'. Choosing Discard, or opening Comments on another issue, wipes the text. The next open of the same issue starts empty.
- **Change:** Keep a map&lt;issueId, string> of drafts in the comments module, written in OnClose (instead of discarding) and loaded in OpenCommentsModal if prefillBody is empty. Persist alongside the finding on drafts. Replace the Discard modal with a quiet 'Draft saved' toast.

#### #57 — Bulk import takes a typed file path and three separate button presses, and the preview can go stale after editing

**Severity: Low** · Effort M · steps · occasional · `bulk-import-manual-three-step`

- **Evidence:** SmatchetBulkTicketsUi.cpp:296 path InputText; :331 'Paste clipboard'; :352 'Parse preview'; no Browse/RequestOpenFilePaths in the file. The stale-preview claim was not verified.
- **Today:** The user types or pastes a path (no browse dialog), clicks Load file or Paste clipboard, clicks Parse preview (and picks a project in a modal when the view has no project), then clicks Run import. Editing the source text does not re-parse, so the preview shown can differ from the text.
- **Change:** Add a Browse button via RequestOpenFilePaths and auto-parse after Load/Paste; drop the stale-preview claim unless re-verified.

#### #58 — Quick Create's Create button greys out with no explanation, and the Type combo has no filter

**Severity: Low** · Effort S · validation · daily · `quick-create-submit-no-reason`

- **Evidence:** Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp:280-291 canSubmit requires summary and ProjectKey, `BeginDisabled()` with no tooltip; :253-262 plain BeginCombo/Selectable list
- **Today:** With an empty summary or a missing project the button is dead and nothing says why. The Type dropdown is click-and-scroll only, unlike the inline draft's filter combos.
- **Change:** After EndDisabled, add `IsItemHovered(AllowWhenDisabled)` + SetTooltip('Enter a summary' / 'Pick a project'). Reuse DrawDraftOptionCombo's filter pattern (SmatchetNewIssueDraftUi.cpp:449-513) for the type row.

#### #59 — Nothing on screen or in the shortcut guide says Ctrl+Enter or Esc work in the create and comment surfaces

**Severity: Low** · Effort S · discoverability · every session · `shortcut-discoverability`

- **Evidence:** docs/guides/keyboard-shortcuts.md has no new-issue, quick-create, bug-report or comment entries (grep returned none); Source/Core/src/Ui/SmatchetCommentsModalUi.cpp:557-560 placeholder drawn as a separate TextDisabled line below the box
- **Today:** Quick Create and bug report accept Ctrl+Enter, but buttons read just 'Create' and 'Submit'. The comment placeholder shows under the box and disappears only when text is present.
- **Change:** Append '(Ctrl+Enter)' to the button labels or tooltips. Switch the comment box to a hint overlay (draw the hint via ImGui::GetWindowDrawList at the box's min when empty and unfocused, since multiline has no native hint). Add the new chords to docs/guides/keyboard-shortcuts.md.

### 7.6 Search, filter and JQL / query input

The grid header carries ONE per-pane search box (##GridFilter, InputTextWithHint, EnterReturnsTrue). A classifier (GridSearchInputClassifier.h) sorts the text into ticket key (Enter jumps), structured query (Enter replaces the pane view's query as an unsaved edit and re-syncs) or plain title text (live substring filter over loaded rows). The box has no autocomplete, no validation and no history. Real JQL / Plane-filter authoring with autocomplete lives only in the Views dashboard Filter tab (DrawJqlQueryEditorEmbedded: suggestion popup, debounced async user lookup, Ctrl+Enter apply and persist). Views are switched through a plain BeginCombo in the grid header, a sidebar list with a name filter in the dashboard, or the raw-id view.activate command. Sort is edited in a mouse-driven popup. The project picker is a combo with a search field.

**Already good here:**

- The search box previews what Enter will do: a live-classified mode glyph (hash / funnel / lens) plus a tooltip that names the action (Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:258-281 "Filter query — Enter replaces this view's query.")
- Plain text filters rows live and a pasted issue key jumps to the row case-insensitively. The mode is chosen from the text, so the user does not pick one (Source/Core/src/Ui/SmatchetGridSearchUi.cpp:41-55 "Case-insensitive, like the classifier (a lowercase key is a key)")
- A query typed in the grid box is an unsaved edit with a Save/Discard strip. It no longer overwrites the saved view on disk (prior P2-H1, Done and holding) (Source/Core/src/Ui/SmatchetGridSearchUi.cpp:138-142,160-163 "SnapshotActiveViewIfNeeded ... d.viewsDirty = true")
- The JQL user-value autocomplete is debounced (220 ms), drops stale responses, shows a Searching... state and an inline error. Jira account ids are shown as display names and mapped back on apply (Source/Core/src/Ui/SmatchetAutocompleteUi.cpp:565-580 "Drop stale results when the user typed past this request"; :556 "GetTime() + 0.22")
- The Views editor has Ctrl+Enter apply, and a dirty-guarded switch with a Discard & switch confirm. Zero-result states offer Clear filter / Edit view (Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:1374 "io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter)"; :1273 "ViewDraftHasUnsavedEdits"; SmatchetActiveProjectGridTable.cpp:754-760)

#### #60 — There is no shortcut to jump into the grid search box, so every search starts with a mouse click

**Severity: High** · Effort S · keyboard · every session · `search-no-focus-chord`

- **Evidence:** Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:302-304 InputTextWithHint("##GridFilter", ...) is drawn with no SetKeyboardFocusHere or focus latch. A grep for FocusSearch/focusFilter finds only the command palette (Source/Core/include/Commands/CommandPaletteUi.h:40). docs/guides/keyboard-shortcuts.md has no search/filter entry.
- **Today:** The user clicks the small box in the pane header, types, and presses Enter. After a view switch or Enter-jump the caret is nowhere. Esc does not return to the grid. I found no Ctrl+F or / binding.
- **Expected:** Linear and VS Code: Ctrl+F or / focuses the filter, Esc clears and returns focus to the list. Gmail uses /.
- **Change:** Register a `grid.search.focus` command (default Ctrl+F or /, only when no text field is active) that sets a per-pane `focusSearchPending` latch. In DrawGridSearchBox call ImGui::SetKeyboardFocusHere() before the InputText when the latch is set. Handle Esc while active: clear gridSearchBuf and refocus the grid. Add the key to the placeholder and to docs/guides/keyboard-shortcuts.md.

#### #61 — A user who does not know JQL cannot filter by status, assignee or label from the grid, because the search box has no suggestions and no filter chips

**Severity: High** · Effort L · discoverability · daily · `search-no-facets-no-autocomplete`

- **Evidence:** SmatchetGridSearchUi.cpp:107-109 "The search box has no autocomplete"; autocomplete editor only in Views dashboard Filter tab (SmatchetViewsDashboardUi.cpp:470-480). The prior_critique_ref P2-M7 does not apply (it concerns the command palette); no matching critique item found.
- **Today:** Typing "status" or "assignee" in the grid box matches only as row text. To filter structurally the user opens the Views dashboard, goes to the Filter tab, edits JQL and applies, which saves the view. The tooltip says Enter replaces the query, and the placeholder says "key, query, or title text" with no example.
- **Expected:** Linear / Jira: typing "sta" offers `status:` and its values. Notion and Linear have a "+ Filter" chip menu. Raycast completes the value as you type.
- **Change:** Reuse the existing suggestion popup (TrackerQueryAcp_InputTextCallback with a JqlEditorState per pane) on ##GridFilter, but only after the classifier detects a field-like token. Otherwise a title search would be hijacked. Add a "+ Filter" SmallButton that opens a popup of status / assignee / label pickers, which writes a clause into the buffer. Keep the placeholder example (e.g. `assignee = me`).

#### #62 — Multi-word title search only matches when the exact phrase appears contiguously inside one field, so "login crash" finds nothing if the words are apart

**Severity: High** · Effort S · parsing · every session · `title-filter-contiguous-substring`

- **Evidence:** Source/Core/src/TicketGridFilterPure.cpp:70-97 TicketMatchesGridFilter folds the whole filter into ONE needle and calls ContainsFolded per field. GridSearchUi.cpp:68-75 passes the whole trimmed string.
- **Today:** "login crash" matches only a field containing "login crash" verbatim. "crash login", "login bug" or a one-letter typo gives "No issues match the filter." Matching is ASCII-only case folding, so non-Latin text is case-sensitive.
- **Expected:** Linear, Jira quick search and Raycast split the input on whitespace, AND the tokens across fields and tolerate typos or sort by fuzzy rank.
- **Change:** In TicketMatchesGridFilter, split `filter` on ASCII whitespace into needles and require every needle to appear in the id or any field (AND). Keep the per-keystroke fold. Optionally add a subsequence or edit-distance-1 fallback on the title only when the strict pass returns 0 rows. Add cases to the existing pure test.

#### #63 — A malformed query gives no inline error. The user finds out only when the grid stays empty or the tracker rejects it

**Severity: High** · Effort M · validation · daily · `query-no-validation-feedback`

- **Evidence:** Source/Core/src/Ui/SmatchetGridSearchUi.cpp:115-129 toasts only ViewUnavailable / UpdateFailed (local state failures). The only error string in the editor path is jqlAcpAsyncUserError (SmatchetAutocompleteUi.cpp:576). SmatchetViewsDashboardUi.cpp:473-475 shows only a static hint line.
- **Today:** Unbalanced parentheses, an unknown field, or a missing quote is accepted as typed. Enter replaces the view query and re-syncs. The error, if any, surfaces later as the status-bar tracker banner or a toast, away from the field, and the view now holds the bad query as an unsaved edit.
- **Expected:** Jira's JQL editor and Linear underline the bad token and show a message under the field as you type. Apply is disabled with a reason.
- **Change:** Add a pure `ValidateQuery(text, kind, fields)` next to JqlSuggestEnginePure that checks paren and quote balance, unknown field ids against the catalog, and a dangling operator. Return {startOffset, message}. Draw it as red text under ##JQLEmbedded and as a tooltip on the funnel glyph in DrawGridSearchBox. Block the Enter-apply, instead of replacing the query, on hard errors. Route the tracker's rejection text back to the same line after sync.

#### #64 — The same query means two different Enters: grid box Enter is a temporary edit, while the Views editor's Enter or Ctrl+Enter silently saves it to disk

**Severity: Medium** · Effort M · steps · daily · `enter-semantics-diverge`

- **Evidence:** Grid box: Source/Core/src/Ui/SmatchetGridSearchUi.cpp:161-163 "d.viewsDirty = true" (Save/Discard strip). Editor: Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:1208-1223 viewsApplyAndSync -> ViewState.UpdateActive(merged) then toast "View saved", and :477-479 jqlWantsApplyFromEnter -> ctx.applyAndSync().
- **Today:** A user trying a query in the Filter tab commits and persists it on the first Enter. There is no "run without saving". Trying a query in the grid box requires remembering to hit Discard afterwards.
- **Change:** Make Enter in the Views JQL field run the query as an unsaved edit through applyQueryToPaneView, the same path as the grid box. Keep Ctrl+Enter / the button as the explicit "Apply & Save". Rename the toast to say "Saved" only on that path.

#### #65 — Switching views by keyboard is not possible: the header view selector is a plain combo, the sidebar filter cannot pick with Enter, and view.activate takes a raw id

**Severity: Medium** · Effort M · keyboard · daily · `view-switch-no-typeahead-or-chord`

- **Evidence:** Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:348-367 BeginCombo + Selectable loop, no filter field. Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:157-174 "Search views..." InputTextWithHint, rows are Selectable, no Enter handler. Source/Core/src/Commands/ViewCommands.cpp:176-182 view.activate param "id" "View id from view.list." I found no next/prev-view chord in the keyboard guide.
- **Today:** The user opens the combo with the mouse, scrolls and clicks. In the dashboard sidebar, typing in the search box only narrows the list. A mouse click is still needed to activate a row. The palette can switch only by view id.
- **Change:** Give the header combo a filter row (as the project picker has) and add a `view.switcher` palette scope that lists views by Name and calls viewsRequestActivate. In the sidebar, make Enter in ##ViewsSidebarSearch activate the first (or arrow-selected) row and add Up/Down. Add default chords for next/previous view.

#### #66 — A plain title phrase containing ( ) ! = ~ &lt; > or a word:word token is treated as JQL, so the live filter stops and Enter replaces the view query

**Severity: Medium** · Effort M · parsing · occasional · `classifier-title-with-punctuation-hijacked`

- **Evidence:** Source/Core/include/Ui/GridSearchInputClassifier.h:120-137 LooksLikeStructuredQuery returns true for any of `= ~ < > ! ( )` or `alnum:nonspace`. GridSearchUi.cpp:59-66 AppliesAsRowFilter is false for Jql.
- **Today:** Searching "fix login (again)", "Q3: roadmap" or "C++ !" flips the glyph to a funnel and the rows stop filtering as you type. Enter sends the text as a query, producing an invalid query the user must Discard. The tooltip is the only warning.
- **Change:** Tighten LooksLikeStructuredQuery: for parens and `!` require a preceding known field name or operator word (catalog lookup), and for `word:` require the word to be a known field id. Fall back to the title filter on a failed validation, and show a hint instead of silently changing mode.

#### #67 — Field-name suggestions match only the start of the id or name, so typing "points" does not find "Story Points"

**Severity: Medium** · Effort S · parsing · daily · `autocomplete-prefix-only-field-names`

- **Evidence:** Source/Core/src/Tracker/TrackerQuerySuggestCommon.cpp:144-150 AsciiStartsWithIgnoreCase(f.Id, pre) and (f.Name, pre); only values use AsciiContainsIgnoreCase (:179). Users already get mid-name matching (JqlSuggestEnginePure.cpp:169).
- **Today:** The user must know the leading characters of the field name or id. Custom fields such as customfield_10016 are found only by that id.
- **Change:** In the field-suggestion loop, add a second pass using AsciiContainsIgnoreCase on f.Name (and each space-separated word start), appended after prefix hits (the `midMatches` pattern already used for users). Cap the list as it is now.

#### #68 — There is no list of recent or saved queries in either search field, and Up/Down are claimed by the suggestion list

**Severity: Medium** · Effort M · steps · daily · `no-query-recents`

- **Evidence:** Source/Core/src/Ui/SmatchetAutocompleteUi.cpp:185-206 HandleAcpHistoryEvent uses Up/Down for suggestion selection only. Back/forward query history exists only as toolbar buttons (SmatchetLocalization.cpp:676-678 "Run the previous query from navigation history.").
- **Today:** To re-run a recent ad-hoc query the user retypes it or steps back with the nav buttons, one query at a time, with no list.
- **Change:** On focus of an empty ##GridFilter or ##JQLEmbedded, show a popup of the last ~10 distinct applied queries, backed by d.navHistory (already pushed on apply). Enter picks, and Up/Down navigates when the suggestion list is empty. Persist it so it survives restart.

#### #69 — Opening the project picker does not focus its search field, and Enter does not pick the top match

**Severity: Medium** · Effort S · focus · daily · `project-picker-no-focus-or-enter`

- **Evidence:** Source/Core/src/Ui/SmatchetProjectPicker.cpp:300-309 BeginCombo("##projectpicker") then InputTextWithHint("##projsearch", ...) with no SetKeyboardFocusHere and no Enter handling; rows are chosen by Selectable only (:63-70).
- **Today:** The user opens the combo, clicks the search field, types, then clicks a row. The Recently used section helps once the popup is open.
- **Change:** Call ImGui::SetKeyboardFocusHere() once when `!state.popupWasOpen` is observed inside BeginCombo. After the InputText, if IsKeyPressed(Enter) and exactly one or a highlighted row passes the filter, set selectedKey to it and close the popup. This picker also serves the new-issue form, so coordinate with that auditor.

#### #70 — The grid box's Clear button empties the text but leaves the applied query running, and it has no keyboard equivalent

**Severity: Low** · Effort S · keyboard · daily · `clear-resets-text-not-query`

- **Evidence:** Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:309-314 `if (ImGui::Button("Clear")) { pane.gridSearchBuf[0] = '\0'; }`. The JQL replace happened earlier at GridSearchUi.cpp:161-164.
- **Today:** After an Enter-applied query, Clear removes the text but the view keeps the query, and the unsaved strip is still showing. The user must find Discard in the strip. Esc does not clear (only ImGui's revert-on-Esc while editing).
- **Change:** In the Clear handler, if the box's last Enter applied a Jql, call the strip's Discard path (restore the snapshot and re-sync). Add Esc-to-clear with the same handler, and show the tooltip "Clear (Esc)".

#### #71 — Editing a view's sort needs the mouse: remove X buttons, direction combos, and an unfiltered menu of every column

**Severity: Low** · Effort S · keyboard · occasional · `sort-popup-mouse-only`

- **Evidence:** Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:125-135 Button("X") and BeginCombo("##dir"), :162-175 BeginMenu("+ Add Sort Rule") listing all columns with MenuItem, and no search.
- **Today:** On a wide view the Add Sort Rule menu is a long list scrolled by hand. There is no type-to-find and no keyboard path to reorder sort keys.
- **Change:** Replace the nested BeginMenu with a BeginPopup holding InputTextWithHint("Find column") plus a filtered Selectable list. Focus the field on open. Also make header-column sort cycling discoverable with a tooltip hint.

### 7.7 Preferences, credentials, first-run setup and keybinding capture

Preferences is a left-rail window with 8 categories and a global fuzzy "Search settings..." box that filters widgets in place. The apply model is mixed. Tracker and MCP fields are buffer-staged behind one global "Save & Sync" button in the nav rail, Assistant has its own Save/Discard row, and most other tabs autosave or apply immediately (each section prints a hint saying which). On first launch BackendHasBeenReachable is false, so the menus are disabled and Preferences is force-opened. The Tracker tab then shows a 3-step explainer, per-backend credential fields, a Test connection probe and the Save & Sync button. Masked fields use SmatchetSecretInputText (Show/Hide, char count). A green probe pins a credential fingerprint that Save & Sync needs to clear first-run read-only. Keybinding capture is click-to-arm: Esc cancels, modifier-less keys are rejected, conflicts only warn, and "Reset all" is confirmed. Dictation has its own separate hotkey capture. The audit was static reading only. The vexp server and the Sourcetrail index were unavailable, so Grep, Glob and Read were used.

**Already good here:**

- Masked credential input shows a Show/Hide toggle, a char count and a leading/trailing-whitespace cue. Every tracker field is trimmed on save, so a pasted token with stray spaces no longer 401s (Source/Core/include/Ui/SmatchetSecretInput.h:44-47 "has leading/trailing whitespace"; Source/Core/src/Ui/SmatchetPreferencesUi_detail.h:155 TrimTrackerCredentialFields)
- Test connection probes the live buffers (not saved cfg) on a worker with a generation guard, shows a semantic-coloured verdict with the server's diagnostic, and pins a credential fingerprint so editing after a green probe cannot unlock first-run (Source/Core/src/Ui/SmatchetPreferencesUi.cpp:801-870 "Connected - credentials verified."; Source/Core/src/Tracker/TrackerSetupPure.cpp:70 ApplyVerifiedSaveUnlock)
- Close guard for unsaved Tracker/MCP edits offers Save & Sync / Discard / Keep editing, and the nav label gets a ' \*' dirty marker (Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1447-1483 "Unsaved changes###PrefsTrackerCloseGuard"; Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp:104 e.Dirty ? " \*")
- Settings search is typo-tolerant (substring, then FuzzyScore fallback), Latin-1 case-folded, filters the real widgets in place, auto-selects the first matching category, and disables empty categories instead of hiding them. It also searches dynamic keybinding rows (Source/Core/src/Ui/PreferencesFilter.cpp:178-190 "Tier 2: fuzzy subsequence"; Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp:190-197)
- Hotkey capture is well behaved. Esc cancels without clobbering, modifier-less combos are rejected with a fix-naming message while staying armed, and a duplicate combo within a row is collapsed. Reset-all asks first and states the count (Source/Core/src/Ui/SmatchetHotkeyCapture.cpp:116-137 "Include Ctrl, Alt, or Win (Shift alone types text)"; Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp:176-180)
- Each section prints how it saves, and the MCP typed fields commit only when no longer focused. A half-typed prefix therefore never restarts the server (Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp:35-49 "Changes save automatically."; Source/Core/src/Ui/SmatchetPreferencesUi.cpp:997-1049 portEditing / tokenEditing)

#### #72 — On the connection form, Enter in a field does nothing and Save & Sync has no keyboard shortcut, so finishing setup means reaching for the mouse

**Severity: High** · Effort S · keyboard · every session · `prefs-no-enter-or-shortcut-to-save`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi.cpp:420-427 ImGui::InputText("Domain"...), "Email", SmatchetSecretInputText("API Token") use no ImGuiInputTextFlags_EnterReturnsTrue; Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp:273 the only submit is SmatchetIconLeadingButton(ICON_FA_ARROWS_ROTATE, "Save & Sync"...) in the nav rail; EnterReturnsTrue appears in prefs only in \_Templates.cpp:203,296
- **Today:** User types domain, Tab, email, Tab, token. Enter is ignored. They must click Test connection (in the body), then click Save & Sync (a separate button in the nav rail). There is no Ctrl+S or Ctrl+Enter and no shortcut hint on the buttons.
- **Expected:** VS Code and Linear sign-in forms: Enter in the last field submits, and the primary button shows its shortcut. Settings dialogs bind Ctrl+S or Ctrl+Enter to Apply.
- **Change:** In DrawTrackerJiraConfig/Plane/GitHub/Linear, give the last credential field ImGuiInputTextFlags_EnterReturnsTrue (the SecretInput wrapper needs a flags param). On Enter, run the Test connection click path, which should be factored out of DrawTrackerTestConnection. Add a Ctrl+S / Ctrl+Enter handler in drawPreferencesWindow (IsKeyChordPressed while the window is focused) that calls onPreferencesSaveAndSync. Add the shortcut to the button tooltip.

#### #73 — Opening Preferences, including the forced first-run open, puts the caret nowhere, so the first keystroke is lost and the user must click a field

**Severity: High** · Effort S · focus · every session · `prefs-no-initial-focus`

- **Evidence:** Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:142-145 "if (trackerLocked) { d.showPreferences = true; }"; Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1374 DrawPrefsSearchBoxInput(d) is drawn first with no focus request; no SetKeyboardFocusHere exists in any SmatchetPreferencesUi\*.cpp (grep returned only combo SetItemDefaultFocus at \_Shell.cpp:239)
- **Today:** New user lands in a locked app with Preferences open. No field has focus. Typing does nothing until they click Domain / Email / Token. Ctrl+, from the grid opens the window with no focused field and no hint that the search box is there. Closing returns focus to nothing in particular.
- **Expected:** Raycast and VS Code settings focus the search box on open (Ctrl+F). A setup wizard focuses its first empty required field.
- **Change:** Add an open-edge latch in drawPreferencesWindow (the preferencesBuffersLoaded false-to-true edge in loadPreferencesBuffers). When TrackerSetupPure::NeedsSetup(d.cfg), call SetKeyboardFocusHere(0) before the first empty credential InputText of the active backend. Otherwise focus ##PrefsSearch. Remember the previously focused window id to restore on close.

#### #74 — Closing Preferences with unsaved Assistant (API key, provider) edits silently discards them; only Tracker and MCP edits trigger the guard

**Severity: High** · Effort S · lost-work · occasional · `prefs-assistant-edits-lost-on-close`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi.cpp:927-929 "return d.preferencesBuffersLoaded && (TrackerPrefsFieldsDiffer(d) || McpPrefsDirty(d));" versus :1413-1414 assistantDirty computed only for the nav marker; :185 ResetAssistantPrefsSeedLatchesOnClose on reset; Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp:725 "Unsaved changes" only in-tab
- **Today:** User pastes an API key in AI & Voice, then closes the window or presses the X, or switches category and later closes. The window closes with no prompt. resetPreferencesWindowState re-seeds from cfg on the next open, and the pasted key is gone. The in-tab Save/Discard row is the only protection.
- **Expected:** Every settings surface with staged edits guards close or keeps a draft (Notion, VS Code settings sync). Never discard silently.
- **Change:** Make PrefsHaveUncommittedEdits include assistantDirty (AssistantAiFieldsDiffer under SMATCHET_WITH_AI). Extend the guard modal body and its Save button to call CommitAssistantWorkingToConfig. Better: collapse the Assistant tab onto the same Save & Sync path so there is one staged model.

#### #75 — Preferences mixes four apply models, and the one global Save & Sync button is always enabled, gives no success feedback, and re-syncs the backend even when only a theme changed

**Severity: Medium** · Effort M · steps · daily · `prefs-four-apply-models-save-sync-opaque`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi_Shell.cpp:35-49 SaveAndSync / Autosave / Immediate / Restart / ExplicitSave / AnnotateDetached; :273 button has no BeginDisabled and no dirty check; SmatchetPreferencesUi.cpp:1125-1219 onPreferencesSaveAndSync ends in app.SyncWithBackend and pushes a toast only on rejected Jira sites (:1145-1150); SmatchetPreferencesUi_Assistant.cpp:712 separate "Save"/"Discard" row
- **Today:** General/Appearance apply instantly, Annotate saves per field on deactivate, MCP autosaves, Tracker needs Save & Sync, Assistant needs its own Save. The user has to read a grey line per section to learn which. Save & Sync looks identical when nothing is dirty and shows no confirmation. It always triggers triggerCatalogRefetch and SyncWithBackend.
- **Change:** Disable Save & Sync unless PrefsHaveUncommittedEdits() or assistantDirty, with a tooltip 'Nothing to save'. Show a brief 'Saved' toast or inline check on success. Skip SyncWithBackend and triggerCatalogRefetch when TrackerPrefsFieldsDiffer was false. Longer term, move Assistant onto the same staged path.

#### #76 — First-run setup needs two explicit actions in a fixed order (Test, then Save & Sync), and Test is clickable with empty fields

**Severity: Medium** · Effort M · steps · every session · `prefs-first-run-test-then-save-two-step`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi.cpp:945-948 "1. Pick a backend. 2. Fill in the credentials below. 3. Press Test connection. Save & Sync unlocks the rest of the app once the connection is verified."; :801 Button has no gate on TrackerSetupPure::CredentialFieldsComplete; Source/Core/src/Tracker/TrackerSetupPure.cpp:70-77 unlock only if fingerprint matches the probe
- **Today:** A new user who fills fields and clicks Save & Sync without Test gets no unlock (read-only stays) and no explanation beyond the banner they may have scrolled past. Test connection on an empty form fires a probe that fails with a generic transport message. Editing one character after a green test voids the pin, with no cue.
- **Change:** When NeedsSetup, make the primary button 'Connect' (Test then Save on success, reusing ApplyVerifiedSaveUnlock). Disable it, with a tooltip naming the blank field, when CredentialFieldsComplete(probeCfg) is false. Show 'Credentials changed since last test' next to the verdict when the fingerprint no longer matches.

#### #77 — The 'Set shortcut' popup opens idle: the user must click 'Click to rebind' before pressing a key, and Enter cannot confirm

**Severity: Medium** · Effort S · keyboard · occasional · `prefs-quickbind-not-armed-no-enter`

- **Evidence:** Source/Core/src/Ui/SmatchetHotkeyCapture.cpp:223-231 QuickBindPopup::Open sets capturing\_ = false; :254 DrawHotkeyRebindControl("quickbind"...) arms only on SmallButton click (:103); :285 'Set' is a Button, no Enter or default-focus handling
- **Today:** Right-click, Set shortcut, then the popup shows the old combo and a tiny 'Click to rebind' button. The user clicks it, presses the combo, then clicks Set. Mouse, key, mouse; three steps for a one-key action. A conflict only prints 'Already bound to:' and Set still works.
- **Change:** In QuickBindPopup::Draw's pendingOpen\_ branch set capturing\_ = true and stamp g_hotkeyCaptureArmedFrame. After capture, treat Enter as Set when draft\_ is non-empty, and SetItemDefaultFocus on Set. Offer 'Replace' (unbind the other action) next to the conflict line.

#### #78 — Keybinding conflicts are written first and warned about afterwards, and there is no one-click fix, no per-row reset to default, and no undo for Clear, 'x' or Reset

**Severity: Medium** · Effort M · validation · occasional · `prefs-keybinding-conflict-committed-warn-only`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi_Keybindings.cpp:101-111 b.Hotkeys[h] = out committed before any conflict check; :156-162 "conflicts with" text is the only handling; :394-397 'Clear' and :114-116 'x' drop combos instantly; only a global 'Reset all to defaults' exists (:171); grep for per-setting reset in Preferences files returns only that button
- **Today:** Pressing a taken combo silently reassigns the slot, so two actions now share one key (both dispatch). The orange line names the other action but the user must navigate there and clear it by hand. To restore a single default the user must reset every shortcut. Clearing a combo cannot be undone.
- **Change:** Before committing in DrawBindingCombosCell, call FindKeybindingConflict on the captured combo. If it hits, show an inline 'Used by &lt;X>. Replace / Keep both / Cancel'. Add a per-row reset button shown only when Hotkeys differs from KeybindingsConfig::Defaults(). Offer a toast 'Shortcut cleared. Undo' for Clear and 'x'.

#### #79 — Settings search has no keyboard entry or exit: no Ctrl+F focus, Esc does not clear it, and Enter does not jump to the first match

**Severity: Medium** · Effort S · keyboard · daily · `prefs-search-no-keyboard-flow`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi.cpp:1232-1247 InputTextWithHint("##PrefsSearch", "Search settings...", ...) with no flags, no focus call; clear is a mouse-only SmallButton("x###PrefsSearchClear"); docs/guides/keyboard-shortcuts.md:138 lists only Preferences `Ctrl+,`; no Escape handling in any SmatchetPreferencesUi\*.cpp
- **Today:** To search, the user clicks the box. After typing 'token' they must Tab through the matched widgets or click one. The query stays when they leave and no key clears it, so the next visit shows a stale filtered pane. The Shortcuts tab also has its own 'Filter shortcuts...' box with the same gaps.
- **Change:** In drawPreferencesWindow, on Ctrl+F (or '/' when no text input is active) call SetKeyboardFocusHere on ##PrefsSearch. While the box is active, Esc clears prefsSearchBuf and Enter does SetKeyboardFocusHere on the first widget that passed ShowSetting. Clear the query in resetPreferencesWindowState.

#### #80 — Tracker URL fields do not accept a pasted URL with a path (board URL, github.com/owner/repo); only scheme and trailing slash are tolerated, and only at request time

**Severity: Low** · Effort M · parsing · occasional · `prefs-url-fields-not-normalised`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi_detail.h:155-170 (trim only); Source/Core/src/Tracker/TrackerHttpUtils.cpp:57-75 (scheme add and slash strip, no path strip); SmatchetPreferencesUi.cpp:420 (CharsNoBlank only)
- **Today:** Domain and the base-URL fields are trimmed on save. A missing https:// and a trailing slash are tolerated later by NormalizeBaseUrl, but the buffer is never rewritten and the user gets no preview. A pasted board URL with a path (/jira/software/...) is kept as typed and then builds a bad base URL. Nothing splits github.com/owner/repo into Owner and Repo.
- **Change:** Add a pure TrackerUrlNormalisePure (scheme default, strip path and trailing slash, split a board URL into host/owner/repo/workspace slug). Call it in CopyTrackerBuffersToConfig and also on field deactivation (IsItemDeactivatedAfterEdit) so the buffer visibly rewrites. Show a dim 'Using acme.atlassian.net' preview under the field when the paste was rewritten.

#### #81 — The push-to-talk hotkey capture behaves differently from every other shortcut capture (modifier rule, error exits capture, no conflict check)

**Severity: Low** · Effort M · keyboard · rare · `prefs-whisper-hotkey-capture-diverges`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:262-267 bare key sets error and 'state.capturing = false' (main capture stays armed, SmatchetHotkeyCapture.cpp:131); :255-256 Shift counts as a modifier here but not in HotkeyNeedsModifier (SmatchetHotkeyCapture.cpp:40-45); :208-244 hand-rolled key set (A-Z, 0-9, F1-F12, Space, Tab, Enter) vs the shared BindableImGuiKeys; no FindKeybindingConflict call
- **Today:** A bare key drops the user out of capture so they must click rebind again. Shift+letter is accepted as a global hotkey, which will fire while typing capitals. The same combo can be bound to an in-app command with no warning. The key set is narrower than the main editor.
- **Change:** Replace CaptureHotkeyVkThisFrame/CommitCapturedHotkey with a shared DrawHotkeyRebindControl wrapper. Keep a Whisper-specific validator (global hook needs a real modifier) but stay armed on rejection and run FindKeybindingConflict against cfg.Keybindings.

#### #82 — Executable-path fields (p4, p4vc) are bare text boxes: no Browse button, no check that the file exists, no trimming, and no feedback on a bad path

**Severity: Low** · Effort M · validation · rare · `prefs-path-fields-no-browse-validation`

- **Evidence:** Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:126-133 ImGui::InputText("p4 executable"...) and CommitTextField at :25-31 "cfgField.assign(buf)" on IsItemDeactivatedAfterEdit with no trim or existence check; command-template fields :136-143 only show a dim 'Placeholders: {file} {line} {cl}' line
- **Today:** User must paste or type a full path, with a possible trailing space or surrounding quotes from 'Copy as path' in Explorer. A wrong path is saved silently and only fails later when Annotate runs p4. There is no 'Detected: C:\...\p4.exe' or error hint.
- **Change:** Trim and strip surrounding quotes in CommitTextField. Add a Browse button (native file dialog helper already used elsewhere, if available) and an existence check rendered with sem.ErrorText below the field. Run `p4 -V` on a worker for a green 'p4 found' cue.

#### #83 — During first-run lock the Preferences window re-opens every frame and the only way forward is a successful connection, so a user without credentials ready cannot explore the app

**Severity: Low** · Effort M · steps · rare · `prefs-locked-first-run-no-escape-hatch`

- **Evidence:** Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:142-145 "const bool trackerLocked = !d.cfg.BackendHasBeenReachable; if (trackerLocked) { d.showPreferences = true; }"; :156-166 all menus except Tools/Help disabled; SmatchetPreferencesUi.cpp:1358-1361 close with edits reopens the window and the guard
- **Today:** A user evaluating the app, or one offline, cannot dismiss Preferences or use any menu until a probe succeeds. Pressing X reopens it the next frame. There is no 'Skip for now', no demo data, and no explicit offline path (ReadOnlyMode exists but needs a green probe to clear).
- **Change:** Add a 'Skip for now (read-only)' secondary button to DrawTrackerFirstRunExplainer that latches a session flag (not BackendHasBeenReachable) so the menu gate lifts while the grid keeps its welcome panel. Keep the gate for tracker-mutating commands.

#### #84 — Bounded numbers are sliders (type-in is a hidden Ctrl+click), and InputInt fields clamp mid-edit with an open upper bound or none

**Severity: Low** · Effort S · discoverability · rare · `prefs-numeric-inputs-slider-typing-hidden`

- **Evidence:** Source/Core/src/Ui/SmatchetPreferencesUi_General.cpp:208 SliderInt("Compact Relative Threshold (Days)", ..., 1, 90); SmatchetPreferencesUi_Local.cpp:307 SliderInt("Grid refresh & notify interval (seconds)", 30, 3600); SmatchetPreferencesUi.cpp:697-707 InputInt("Activity day window") clamped to >=1 each frame with no upper bound and 'Max changes per source' likewise
- **Today:** A 30..3600 s slider is awkward to set precisely and the Ctrl+click type-in is not advertised. The 'Activity day window' and 'Max changes per source' InputInts clamp to 1 every frame (cpp:698-700, 705-707), so clearing the field to retype snaps to 1. They have no upper bound.
- **Change:** Switch the long-range sliders to InputInt with step and clamp on IsItemDeactivatedAfterEdit (the AnnotateAnalysisUi_Preferences.cpp:103-108 pattern already does this). Add max clamps and a '(1-365)' unit hint to the User Info inputs. Add 'Ctrl+click to type' to slider tooltips where kept.

### 7.8 Cross-cutting keyboard, focus, modal and confirmation conventions

Input is driven through a rebindable hotkey table (KeybindingsConfig -> SmatchetUI::dispatchKeybindings), ImGui keyboard nav (NavEnableKeyboard only), and about 26 hand-rolled BeginPopupModal sites across Source/Core/src. There is no shared modal helper. SmatchetModalPopupOpen.h only centres and sizes a popup, and SmatchetDestructiveButton.h only pushes red colours. Each modal re-implements its own buttons, Esc handling and focus. Vendored ImGui deliberately does not close modals on Esc (imgui.cpp:14968), so a modal without hand-written Esc code is mouse-only to dismiss. Only 5 sites handle Esc. Global hotkeys are not suppressed while a modal is open. The toast system has no action buttons and there is no undo anywhere, so every destructive path is a confirm dialog.

**Already good here:**

- Rebindable hotkey system with several combos per command, alias de-dup, wheel-notch accumulation, and capture mode that suppresses dispatch while a rebind is being recorded (Source/Core/src/Ui/SmatchetUI.cpp:1107 `if (smatchet::ui::HotkeyCaptureArmedRecently()) { return; }`; :1143 per-frame once-per-action guard)
- Bare-key (typing-producing) hotkeys are blocked while a text field is active, while Ctrl/Alt/F-key combos still work (Source/Core/src/Ui/SmatchetUI.cpp:1132-1136 `typingSafe ... if (!typingSafe && io.WantTextInput) continue;`)
- Long-text editor sets the modern multiline contract: Ctrl+Enter saves, Esc cancels, hint shown in footer, Save gated by a visible acknowledgement checkbox (Source/Core/src/TicketFieldEditor_Modal.cpp:496-503 `Ctrl+Enter to save · Esc to cancel · Ctrl+P preview mode`)
- Field-viewer modal handles Esc layering correctly: Esc in a text field only defocuses, a second Esc closes, and unsaved input routes to a Discard/Keep confirm where Esc means keep (Source/Core/src/Ui/SmatchetFieldViewerModalUi.cpp:109-116 and :136-137)
- Save-as-new modal is the model small dialog: caret placed on first frame, name pre-filled, Enter commits, Save disabled while empty (Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp:640-654 `SetKeyboardFocusHere(); ... ImGuiInputTextFlags_EnterReturnsTrue`)
- Confirm dialogs consistently use action-first button order with a shared destructive-red style, with named verbs instead of OK/Cancel, and the layout-reset confirm stages its don't-ask-again checkbox so Cancel does not persist it (Source/Core/include/Ui/SmatchetDestructiveButton.h:12; Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:512-528)

#### #85 — Pressing Esc does nothing in most dialogs; the user must reach for the mouse to cancel

**Severity: High** · Effort M · keyboard · daily · `modal-esc-does-not-close`

- **Evidence:** Esc handlers only at TicketFieldEditor_Modal.cpp:497, SmatchetFieldViewerModalUi.cpp:114, SmatchetHotkeyCapture.cpp:124, TrackerDateTimeFieldEditor.cpp:432; ~32 BeginPopupModal sites in Source (not 26), most without Esc.
- **Today:** Open Delete view, Discard changes, Unsaved changes, worklog, icon picker or update dialog and press Esc: nothing happens. 19 of 26 BeginPopupModal sites are confirmed Esc-dead (2 annotate sites unverified). The 5 that do respond use different rules (some discard the draft, some defocus first).
- **Expected:** VS Code, Linear, Raycast, Jira: Esc always dismisses the topmost dialog as Cancel / keep-editing, and never confirms a destructive action.
- **Change:** Add one helper (for example smatchet::ui::ModalEscCancelRequested() in a new Source/Core/include/Ui/SmatchetModalKeys.h) that returns IsWindowFocused(RootAndChildWindows) && IsKeyPressed(ImGuiKey_Escape,false) && !io.WantTextInput && no nested popup open. Call it right after each BeginPopupModal and route it to the existing Cancel/Keep branch. Alternatively wrap BeginPopupModal in a shared SmatchetBeginDialog() so the 19 sites inherit it. Effort is mostly mechanical; consolidate on the FieldViewer two-stage-Esc rule.

#### #86 — Confirm dialogs have no designated default button, so Enter does nothing and the first button reached by nav is the destructive one

**Severity: High** · Effort M · focus · daily · `no-default-button-enter`

- **Evidence:** Confirm bodies list the destructive button first with no default-focus call and no Enter handling: SmatchetViewsDashboardUi.cpp:1009 `Button("Delete")`; SmatchetOfflineQueueUi.cpp:1461 `Button("Discard rows")`; SmatchetPreferencesUi_General.cpp:363 `Button("Delete and recreate")`; SmatchetAiAssistantUi.cpp:1347 `Button("Clear")`; SmatchetPreferencesUi_Keybindings.cpp:182. SetItemDefaultFocus is not used in any of the 26 modals (grep shows it only in palette, combos, autocomplete). Only Save-as-new (Grid:648) and the date picker accept Enter.
- **Today:** Enter on a confirm does nothing. Tab/arrow nav starts at the first widget, which in Delete view, Discard rows, Delete database and Clear conversation is the irreversible action, so Space/Enter after one Tab lands on destroy. The safe button is last. Runtime nav-init target not verified; this follows from ImGui's first-widget init and the button order.
- **Expected:** macOS/Windows/Raycast dialogs: Enter = primary (or safe for destructive), Esc = cancel, default button has a visible focus ring; destructive confirm defaults focus to Cancel.
- **Change:** Add one helper SmatchetConfirmFooter(primaryLabel, cancelLabel, destructive) in SmatchetDestructiveButton.h family that draws the pair, calls SetItemDefaultFocus + ImGui::SetKeyboardFocusHere on the safe button on IsWindowAppearing(), shows the nav focus ring, and handles Esc (finding 1). For non-destructive confirms (Save & switch, Reset layout) default to the primary and honour Enter. Migrate the 8 confirm sites.

#### #87 — Pressing Esc in the long-text editor throws away the whole edit with no confirmation, while the sibling field viewer guards the same case

**Severity: High** · Effort S · lost-work · daily · `longtext-esc-discards-draft`

- **Evidence:** Source/Core/src/TicketFieldEditor_Modal.cpp:497 `escPressed = IsKeyPressed(ImGuiKey_Escape,false)`; :523 `bool cancel = escPressed;`; :564-566 `else if (cancel) { CloseCurrentPopup(); CloseLongTextEditor(); }` (no dirty check). Compare SmatchetFieldViewerModalUi.cpp:122 `HasUnsavedInput()` -> discard confirm.
- **Today:** A user writing a long description or comment who hits Esc (to leave a menu, dismiss autocomplete, or by habit) loses the entire buffer; it fires even while the textarea has focus (comment at :495 `Both work even when the textarea is focused`).
- **Expected:** Notion/Jira/GitHub: Esc with unsaved text asks or keeps a draft; drafts survive close.
- **Change:** In DrawLongTextFooter compare Buffer to BufferSeedShown (the same ShouldQueueLongTextEdit predicate used at :467); if dirty, Esc/Cancel opens the same Discard/Keep sub-modal as the field viewer (Keep is default and Esc inside it keeps). Better: stash the buffer per issue+field in memory and restore on reopen.

#### #88 — App-wide shortcuts still fire while a modal dialog is open, so Ctrl+B, Ctrl+, or Ctrl+Shift+P act behind a confirm

**Severity: Medium** · Effort S · keyboard · occasional · `global-hotkeys-under-modal`

- **Evidence:** Source/Core/src/Ui/SmatchetUI.cpp:1091-1179 dispatchKeybindings only early-returns on `HotkeyCaptureArmedRecently()` (:1107); there is no ImGui::IsPopupOpen / modal check before `app.Commands().Dispatch(...)` (:1175). The palette toggle runs at :1152.
- **Today:** With Delete view? or Unsaved changes open, Ctrl+Shift+P opens the palette, Ctrl+, toggles Preferences (the modal's host window), Ctrl+B changes layout behind the dialog. Behind-modal state change can also invalidate the pending target (viewsPendingDeleteId).
- **Change:** In dispatchKeybindings, compute `const bool modalUp = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)` (the same call already exists at SmatchetStatusBarUi.cpp:327) and skip commands unless the command id is in a small allow-list (ui.zoom.\*, app.fullscreen.toggle). A command-palette popup is not a BeginPopupModal, so check it separately.

#### #89 — Icon picker and worklog dialogs open without a focused field (no caret on first frame)

**Severity: Medium** · Effort M · focus · daily · `focus-not-restored-after-modal`

- **Evidence:** No focus-restore code exists around any modal close (the grep for SetKeyboardFocusHere / SetItemDefaultFocus / SetNextWindowFocus finds none in CloseCurrentPopup paths). Opens with no focus request: SmatchetIconPickerUi.cpp:36 `InputTextWithHint("##iconsearch", "Search icons..."` (no SetKeyboardFocusHere), TicketFieldEditor_Worklog.cpp:443-475 (worklog modal, no focus call), SmatchetUI.cpp:209 update dialog.
- **Today:** Icon picker search box and worklog time field are not focused on open; user must click first. Focus-restore-to-invoking-item not verified.
- **Change:** On IsWindowAppearing() call SetKeyboardFocusHere() for the primary field (icon search, worklog time). In the shared modal helper, record ImGui::GetFocusID()/focused window before OpenPopup and, after the close frame, SetNextWindowFocus() + restore the stored id via a one-shot SetKeyboardFocusHere on the launching widget.

#### #90 — Destructive actions use "cannot be undone" confirm dialogs; there is no undo toast anywhere

**Severity: Medium** · Effort L · lost-work · occasional · `no-undo-confirm-everywhere`

- **Evidence:** SmatchetViewsDashboardUi.cpp:1006 `Delete view \"%s\"? This cannot be undone.`; SmatchetAiAssistantUi.cpp:1345 `This cannot be undone.`; SmatchetUI_MainMenu.cpp:509-510 `This cannot be undone.`; toast only has a whole-toast click action: Source/Core/src/Ui/SmatchetToast.cpp:30 `RunRowActionOnClick = true`, ToastHistoryPure.h:23 `using ToastRowAction = std::function<void()>`. Grep for Undo / Ctrl+Z across Source/Core finds only the vendored text editor (TextEditor.cpp:615).
- **Today:** Delete a view, clear the chat, discard offline rows or reset layout: every one asks a modal question first, then is irreversible. No Undo button on any toast; no app-level Ctrl+Z.
- **Change:** Add an optional `actionLabel` + callback to SmatchetToastManager::Push (SmatchetToast.cpp:27 overload) drawn as an in-toast button with an Undo hotkey hint. Start with soft-deletable local state (delete view: keep the ViewDefinition in a pending-undo slot, queued-offline discard: tombstone rows for 10 s; chat clear: hold the vectors) and drop the confirm modals for them. Keep the confirm for remote deletes and DB wipe.

#### #91 — Esc means three different things across dialogs: defocus first, cancel at once, or nothing

**Severity: Medium** · Effort M · keyboard · daily · `esc-semantics-inconsistent`

- **Evidence:** FieldViewer:114 `Esc ... && !WantTextInput` (two-stage); TicketFieldEditor_Modal.cpp:497 immediate cancel even in textarea; TrackerDateTimeFieldEditor.cpp:432 `backPressed = IsKeyPressed(ImGuiKey_Escape)` with repeat enabled and no WantTextInput guard; SmatchetBugReportUi.cpp:344 and SmatchetQuickCreateIssueUi.cpp:313 close on bare Esc while typing; AnnotateAnalysisUi_Window.cpp:1186 window-focused Esc.
- **Today:** Same key, different outcomes: one Esc closes the date picker or quick-create (while a field is active), but the field viewer needs two, and 19 modals ignore it. The date picker uses IsKeyPressed with key repeat, so a held Esc can also close the dialog beneath.
- **Change:** Define the rule once in the shared modal helper (finding 1) and delete the per-TU IsKeyPressed(Escape) copies (CommandPaletteUi.cpp:242,344, BugReportUi:344, QuickCreate:313, DateTime:432). Pass repeat=false everywhere.

#### #92 — The worklog dialog can be saved or cancelled only by mouse; there is no Enter, Ctrl+Enter or Esc handling

**Severity: Medium** · Effort S · keyboard · daily · `worklog-modal-no-keyboard-submit`

- **Evidence:** Source/Core/src/TicketFieldEditor_Worklog.cpp:443-518: BeginPopupModal("TimeTrackingPopup") with only `Button("Save", ImVec2(80,0))` (:500) and `Button("Cancel")` (:507), no IsKeyPressed anywhere in the body; five fields (time, remaining, date, description) are tabbed to reach Save.
- **Today:** After typing the time spent the user must tab through the date picker and description to reach Save, or reach for the mouse; Esc is ignored. Worklogs are logged daily.
- **Change:** Reuse the Long-text footer pattern (Modal.cpp:496-497): `ctrlEnter = io.KeyCtrl && IsKeyPressed(ImGuiKey_Enter,false)` -> HandleWorklogSave when !SubmitInFlight; Esc -> Cancel. SetKeyboardFocusHere on the time field on IsWindowAppearing. Another auditor owns the worklog field parsing; this is only the dialog keys.

#### #93 — Esc Esc exits Zen mode even when the user is dismissing a popup or typing, so two quick Esc presses can change the layout unexpectedly

**Severity: Low** · Effort S · keyboard · rare · `zen-esc-esc-ignores-context`

- **Evidence:** Source/Core/src/Ui/SmatchetUI.cpp:994-1007 `if (d.cfg.ZenMode) { ... IsKeyPressed(ImGuiKey_Escape,false) ... zenEscCount >= 2 -> ZenMode=false }`, with no WantTextInput or popup check (contrast the chord at :981 which does check WantTextInput).
- **Today:** In Zen mode, Esc to close an autocomplete then Esc to close a dialog also counts toward the Zen exit, so the second press exits Zen as well.
- **Change:** Count Esc only when !io.WantTextInput && !IsPopupOpen(AnyPopupId|AnyPopupLevel) && no item active, and reset the counter otherwise. Zen is documented in docs/guides/keyboard-shortcuts.md:119, so keep that behaviour but state the condition there.

#### #94 — Only two of eight destructive confirms use the red button, and the safe-button wording varies (Cancel, Keep, Keep editing, Keep my shortcuts)

**Severity: Low** · Effort S · discoverability · occasional · `destructive-styling-and-wording-inconsistent`

- **Evidence:** SmatchetPushDestructiveButtonColors used only at SmatchetViewsDashboardUi.cpp:1008, SmatchetPreferencesUi_General.cpp:362 (and the Views header delete button :404). Unstyled destructive primaries: SmatchetOfflineQueueUi.cpp:1461 `Discard rows`, SmatchetViewsDashboardUi.cpp:978 `Discard & switch`, SmatchetAiAssistantUi.cpp:1347 `Clear`, SmatchetPreferencesUi_Keybindings.cpp:182, SmatchetPreferencesUi.cpp:1475 `Discard`, SmatchetUI_MainMenu.cpp:516. Safe button labels: Cancel / Keep / Keep editing / Keep my shortcuts.
- **Today:** A user cannot tell by colour which button loses data; the safe button is called four different names.
- **Change:** Fold styling into the shared confirm footer (finding 3) with an explicit destructive flag; use Cancel for safe unless the verb needs a counterpart (Keep editing for dirty-close guards).

#### #95 — The keyboard contract document covers rebinding only; no Esc/Enter/Tab conventions for dialogs and no shortcut hints on dialog buttons

**Severity: Low** · Effort S · discoverability · occasional · `no-modal-contract-in-docs`

- **Evidence:** docs/guides/keyboard-shortcuts.md:111-122 lists only `Ctrl+M, Z`, `Esc Esc`, and palette `Up/Down/Enter/Esc` as system shortcuts. Dialog buttons carry no hints: SmatchetViewsDashboardUi.cpp:1009,1019; TicketFieldEditor_Worklog.cpp:500-507 `Save`/`Cancel` (only the long-text footer prints `Ctrl+Enter to save · Esc to cancel`, Modal.cpp:499).
- **Today:** Users cannot learn which keys work in a dialog; the contract is undocumented and so uncheckable by tests.
- **Change:** Add a Dialogs section to docs/guides/keyboard-shortcuts.md once the shared helper exists, and have the helper append a dim `Esc` / `Enter` hint to the Cancel and primary labels, using the hotkey-label code already used by menus.

### 7.9 AI assistant composer, dictation / Whisper, Lua console and other free-form inputs

The AI composer is a fixed 4-line InputTextMultiline with Enter=send and Ctrl+Enter=newline; a Cancel button appears only while a turn is in flight. Dictation is a global push-to-talk hotkey (hold/release handlers) that splices transcribed text into the focused InputText via DictationInsertionRouter. A top-right overlay shows REC, a level bar and a Cancel button. The Lua console is a script file editor (TextEditor) with Run buttons and manual Ctrl+Space autocomplete, not a REPL. The Annotate callstack box is a bare multiline InputText with a separate Process button. The checkpoint file was written to build/input-ux-audit/results/audit-assistant-voice-console.md.

**Already good here:**

- Enter sends, Ctrl+Enter inserts a newline, there is a visible hint, and focus returns to the input after a send (Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1163,1220,1258 (CtrlEnterForNewLine; SetKeyboardFocusHere(-1); "Enter sends, Ctrl+Enter = newline"))
- An over-cap paste is detected and reported by a toast instead of being clipped silently (Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1182-1189 ("Paste truncated" / "%d KB dropped"))
- A failed Submit or the consent modal keeps the draft and gives cause-and-fix copy (Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1057-1058 ("No active AI provider — open Preferences..."), 1025-1037)
- The dictation splice bridge keeps the caret after the dictated text and keeps its target while the panel is a hidden dock tab (Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:943-970 (ReloadUserBufAndMoveToEnd), 1594-1601)
- The Lua console unsaved-switch flow offers Save/Discard/Cancel and loads scripts asynchronously with a visible cue (Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:847-877, 903-907 ("Loading..."))

#### #96 — Opening the assistant panel does not put the caret in the chat input, so the user must click the box every time

**Severity: High** · Effort S · focus · every session · `assistant-no-focus-on-open`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1607-1610 (`ImGui::SetWindowFocus()` only); SetKeyboardFocusHere appears only at 1220 and 1233, after a send
- **Today:** Ctrl+Shift+A opens or focuses the panel window, but no widget is active. Typing goes nowhere, and dictation has no active target until the user clicks the input.
- **Expected:** Linear, Raycast and ChatGPT-style composers focus the input on open, and a hotkey to open it lets you type immediately.
- **Change:** In SmatchetDrawAiAssistantPanel, when d.requestAssistantFocus is consumed or on the first frame after assistantPanelOpen flips true, set a pending-focus flag. In DrawInputAndButtons, call ImGui::SetKeyboardFocusHere() before InputTextMultiline when the flag is set, then clear it.

#### #97 — Esc does nothing in the assistant: an in-flight response can only be stopped with the mouse, and Esc does not return focus

**Severity: High** · Effort S · keyboard · daily · `assistant-esc-does-nothing`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1247-1254 (mouse-only `ImGui::Button("Cancel")`); no ImGuiKey_Escape reference anywhere in the file
- **Today:** During a stream the only stop control is a small Cancel button. Esc neither cancels the turn nor leaves the input.
- **Expected:** Claude, ChatGPT and VS Code Chat: Esc stops generation, and a second Esc blurs the input and returns focus to the previous surface.
- **Change:** In DrawInputAndButtons, after the InputText call, if ImGui::IsWindowFocused(RootAndChildWindows) and IsKeyPressed(ImGuiKey_Escape): if d.assistantInFlight, call ctrl->Cancel(); else ImGui::ClearActiveID() and restore the previous focus. Add "(Esc)" to the Cancel button label or tooltip.

#### #98 — When dictation fails (consent off, mic error, transcription error, empty result, busy), the user sees nothing and the key press just does nothing

**Severity: High** · Effort S · validation · daily · `dictation-failures-silent`

- **Evidence:** Source/Plugins/Whisper/WhisperPlugin.cpp:836-837 (LOG_WARN consent denied), 845-848 (LOG_ERROR capture start failed), 950 (LOG_DEBUG "dropping new audio"), 973-982 (LOG_WARN transcription failed / empty); none of these push a toast
- **Today:** The REC overlay appears and vanishes, or never appears. No toast or status message says why. The user cannot tell a muted mic from a disabled consent, a network failure or silence.
- **Expected:** Superhuman, macOS Dictation and Whisper-based tools show an inline failure message that names the fix ("Mic access off - enable in Preferences > Whisper").
- **Change:** At each failure exit, PostToMainThread a SmatchetToastManager::Instance().Push(...) with cause-and-fix copy, reusing the pattern at SmatchetAiAssistantUi.cpp:1185. For empty text, show a short "Didn't catch that" toast. For the busy-drop case, show "Still transcribing previous clip".

#### #99 — The dictation overlay says "Cancel (Esc)" but Esc is not wired, and cancelling may still transcribe and insert the audio

**Severity: High** · Effort M · keyboard · occasional · `dictation-esc-cancel-label-lie`

- **Evidence:** Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp:70 ("Cancel (Esc)"), 72 (`SetRecording(false)`), 48 (ImGuiWindowFlags_NoNav); Source/Plugins/Whisper/WhisperPlugin.cpp:938-965 (release path drains PCM and transcribes)
- **Today:** No Esc handling exists (overlay is NoNav, SmatchetWhisperOverlayUi.cpp:48; label at :70). The Cancel click only calls SetRecording(false) (:72), which hides the overlay. captureActive stays true and nothing reads IsRecording to gate release, so the mic keeps capturing, and on key release RunHotkeyRelease_Worker (WhisperPlugin.cpp:932-995) drains PCM, transcribes and inserts the text. The overlay vanishes while capture continues, which is worse than the finding states.
- **Expected:** Esc during recording discards the audio. The label must match the behaviour.
- **Change:** Add a discard flag to PhaseEState or the router. In overlay Render, handle IsKeyPressed(ImGuiKey_Escape) while IsRecording() by setting discard. In RunHotkeyRelease_Worker, skip transcription and clear state when discard is set. Until then, drop "(Esc)" from the label.

#### #100 — Pressing Enter while a reply is streaming is silently ignored, and there is no way to queue the next message

**Severity: Medium** · Effort M · steps · daily · `assistant-enter-while-streaming-dropped`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1210-1216 (`sendDisabled` includes assistantInFlight; `if (enterSubmitted && !sendDisabled)`)
- **Today:** The user types a follow-up while the model streams and presses Enter. Nothing happens, no feedback appears, and the Send button is greyed out.
- **Change:** Either queue the submitted text in a pending-send slot that DispatchAiSend drains when assistantInFlight clears, or show a one-line hint on the strip ("Wait for reply or Esc to stop"). Make the Send button double as Stop while in flight.

#### #101 — There is no Up-arrow recall of earlier prompts, so repeating or tweaking the last prompt means retyping it

**Severity: Medium** · Effort M · bulk · daily · `assistant-no-history-recall`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1082-1083 (buffer cleared on send); no ImGuiKey_UpArrow in the file
- **Today:** After a send the input is empty. To resend a variant the user must copy from the history list or retype.
- **Change:** Keep a ring of sent prompts built from d.assistantHistory user messages. Use an ImGuiInputTextFlags_CallbackHistory callback on the InputTextMultiline (add it to inputFlags at 1163) and replace the buffer via data->DeleteChars/InsertChars. Gate it on an empty buffer so multiline caret movement still works.

#### #102 — The composer is a fixed 4-line box with an 8 KiB cap; long prompts scroll inside a small window and an over-cap paste loses its tail

**Severity: Medium** · Effort M · responsiveness · daily · `assistant-fixed-height-no-grow`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:73-74 (kInputRowsTall = 4.0f, kInputBufCap = 8 \* 1024), 1151, 1166
- **Today:** A pasted log or ticket body shows 4 lines. The user cannot see what they are sending. An over-cap paste is truncated, with a toast only after the fact.
- **Change:** Compute inputH from the line count of s_inputCharBuf (clamped to about 10 lines and panel height). Show a dim "n / 8 KB" counter from roughly 80 percent of the cap. Consider raising the cap through the existing CallbackResize path with std::string storage.

#### #103 — An unsent assistant draft survives closing the panel but is lost on restart or crash, and dictation into a closed panel is dropped

**Severity: Medium** · Effort M · lost-work · occasional · `assistant-draft-not-persisted`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:86 (static s_inputCharBuf), 1174-1176 (mirror into d.assistantInputBuf); assistantInputBuf appears only in SmatchetUiSession.h, SmatchetAiAssistantUi.cpp and AiAssistantInputSeedDecision.h; 1565 (UnregisterInputText when panel closed)
- **Today:** The draft lives only in process memory. A crash or app restart drops a long prompt, though history is persisted via chat_persist. Dictating while the panel is closed discards the text.
- **Change:** Debounce a write of d.assistantInputBuf into the existing chat_persist worker or config (about every 2 s when dirty) and rehydrate it in HydrateFromConfigOnce. Optionally open the panel and insert when a transcription arrives with no target.

#### #104 — Dictation is hold-to-talk only, with no toggle mode, and the auto-send-on-punctuation option can send a transcript before the user reviews it

**Severity: Medium** · Effort M · steps · daily · `dictation-hold-only-and-autosend`

- **Evidence:** Source/Plugins/Whisper/WhisperPlugin.cpp:816 (RunHotkeyPress_Worker), 916 (RunHotkeyRelease_Worker), 999-1001 (auto-send on punctuation); Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp:57 ("hold hotkey")
- **Today:** The user has to hold a chord for the whole utterance, which is awkward for long prompts and for accessibility. If the preference is on and the transcript ends in . ? or !, it is sent immediately with no undo or review window.
- **Change:** Add a cfg.WhisperToggleMode: a quick tap (release under about 300 ms) latches recording, and the next press stops it. In the auto-send path, show a brief "Sending... (Esc to undo)" strip for about 1.5 s before TriggerAiAssistantSend.

#### #105 — Lua console autocomplete appears only on Ctrl+Space from a static candidate list, and Run has no keyboard shortcut

**Severity: Medium** · Effort M · discoverability · occasional · `lua-autocomplete-manual-only`

- **Evidence:** Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:798-801 (IsKeyChordPressed Ctrl+Space), 526-540 (Selectable list), 699 and 741 (Run is a button only), 805 (hint text)
- **Today:** Autocomplete opens only on Ctrl+Space (LuaConsolePlugin.cpp:798-801) with a static candidate list filtered by case-insensitive prefix, not fuzzy (:528 AsciiCaseInsensitivePrefix). Run buttons at :699 and :741 have no key chord, and the hint at :805 mentions only Ctrl+Space. The claim 'no keyboard-selected row visible' is not supported by the code I read, so drop it.
- **Change:** Open the popup automatically after 2+ identifier characters (debounced), rank with the shared fuzzy matcher, support Up/Down/Tab/Esc, and bind Ctrl+Enter or F5 to the same handler as the Run button in DrawRunButtonRow.

#### #106 — The composer has no slash commands, @ mentions or placeholder hint; context is chosen with checkboxes and a per-turn model row

**Severity: Low** · Effort L · discoverability · occasional · `assistant-no-slash-or-mentions`

- **Evidence:** Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:1634 (DrawContextBlockCheckboxes), 1621 (DrawPerTurnModelEffortRow), 1166 and 1258 (help is a separate TextDisabled, no in-field hint)
- **Today:** The user cannot type /clear, /model or @ticket to steer a turn. An empty input shows no example.
- **Change:** Draw a placeholder via the standard empty-buffer overlay (AddText at the input rect when strlen == 0). Prototype /clear and /model via CallbackCompletion or an InputText callback that opens a popup list.

#### #107 — The Annotate callstack paste box is a plain 120 px multiline with no submit shortcut or paste feedback

**Severity: Low** · Effort S · keyboard · occasional · `annotate-callstack-box-bare`

- **Evidence:** Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:314-315 (`InputTextMultiline("##callstackpaste", ..., ImVec2(-1.f, 120.f), 0)`), 267 (Process is a separate button)
- **Today:** The callstack paste box is InputTextMultiline with flags 0 (AnnotateAnalysisUi_Window.cpp:314-315). Process is a separate button (:267) and no Ctrl+Enter binding exists in the file. The before-CL field is a plain InputText (:198). Whether CharsDecimal fits it is unverified because the field may accept non-numeric text, so drop that part of the recommendation.
- **Change:** Bind Ctrl+Enter while the callstack box is active to RunAnnotateProcessFromBuffers, and show a live recognised-frame count. Do not add CharsDecimal unless the before-CL field is confirmed numeric-only.

### 7.10 Offline queue conflict resolver (merge editor)

The offline-queue conflict resolver is a modal popup "ResolveMergeConflict" in SmatchetOfflineQueueUi.cpp, with four panes chosen by the parsed conflict_context_json kind: text (rich multiline, seeded with git-style conflict markers), scalar (single-line, seeded with "mine"), unverified (Force Mine / Discard) and unknown (read-only, Discard / Close). It opens only by mouse, either a double-click on the row state cell or the right-click "Resolve merge conflict..." item. Everything inside is button-driven: Use Mine, Use Theirs, Save, Cancel. There is no keyboard handling at all (no Esc, Enter, Ctrl+Enter or focus calls in the file). The Resolved text lives in a fixed-size std::vector buffer on UiDrawSession (64K for text, 8K for scalar). Cancel clears it, and reopening reseeds it.

**Already good here:**

- Failed re-queue keeps the modal open and the user's resolution intact, with an explanatory message; scalar error text tells the user how to fix the value (SmatchetOfflineQueueUi.cpp:1188-1195 HandleConflictRequeueResult; :1303 "your resolution is still here. Enter one of the field's values")
- Malformed or unknown conflict context degrades to a non-actionable pane, so a corrupt row cannot be resolved to empty content (SmatchetOfflineQueueUi.cpp:1143-1144 and :1362 DrawConflictPaneUnknown)
- Text pane blocks saving while &lt;&lt;&lt;&lt;&lt;&lt;&lt; markers remain and shows an inline warning in the same pane (SmatchetOfflineQueueUi.cpp:1231-1236 "Resolve all &lt;&lt;&lt;&lt;&lt;&lt;&lt; markers before saving.")
- Scalar resolution is prefilled with the user's own value, so the likely path is edit-or-click (SmatchetOfflineQueueUi.cpp:1405-1408 memcpy of seedCtx.Mine)
- Mine and Theirs panes are selectable and copyable (ReadOnly input widgets), and the discard path writes an audit entry (SmatchetOfflineQueueUi.cpp:1213-1214 ImGuiInputTextFlags_ReadOnly; :1347)

#### #108 — I cannot open a merge conflict from the keyboard; it needs a double-click or a right-click menu

**Severity: High** · Effort S · keyboard · occasional · `conflict-open-mouse-only`

- **Evidence:** SmatchetOfflineQueueUi.cpp:985 "IsMouseDoubleClicked(0) && row.hasMergeConflict"; :936 "MenuItem(\"Resolve merge conflict...\")"
- **Today:** The row state cell is an ImGui::Selectable. Only a double-click or a right-click context menu sets showConflictResolveModal. There is no Enter-on-row, no shortcut, and no 'next conflict' control. With several conflicts I reopen each one by mouse.
- **Expected:** Linear and VS Code merge flows: Enter on the focused row opens the resolver, and the next conflict loads on completion.
- **Change:** Nav is already on, so skip that step. Open the resolver when the row Selectable is activated, via the Selectable return value or IsItemFocused plus Enter, not gated by IsItemHovered. Optionally auto-open the next hasMergeConflict row in FinishConflictModal (:1176).

#### #109 — Conflict dialog opens with nothing focused, and Enter or Ctrl+Enter do not submit

**Severity: High** · Effort S · focus · occasional · `conflict-no-keyboard-in-modal`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1420 BeginPopupModal(...) with no SetKeyboardFocusHere or IsKeyPressed anywhere in the file; :1228 InputTextMultiline with only AllowTabInput
- **Today:** The modal opens with no caret. Ctrl+Enter does nothing in ##CRResolved. Enter in ##SCResolved does not save. Esc closes through ImGui default behaviour with no policy attached.
- **Expected:** Linear and VS Code: caret in the primary field on the first frame, Enter or Ctrl+Enter submits, Esc cancels, and focus returns to the row.
- **Change:** On the frame the popup opens, call ImGui::SetKeyboardFocusHere() before ##SCResolved or ##CRResolved. Use ImGuiInputTextFlags_EnterReturnsTrue on the scalar field. On the text pane, treat Ctrl+Enter, when not blocked by hasConflictMarkers, as Save resolved. Make Esc follow one explicit policy (see conflict-cancel-loses-edit).

#### #110 — Cancel or Esc throws away my partly merged text, and reopening resets it to the marker template

**Severity: High** · Effort M · lost-work · occasional · `conflict-cancel-loses-edit`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1262-1264 "conflictResolveBuf.clear(); CloseCurrentPopup();"; :1412-1417 reseed on every open
- **Today:** Merging a long rich-text conflict by hand and then clicking Cancel clears the buffer with no confirm. Reopening reseeds the marker template. A stray Esc or an accidental modal dismissal after editing loses the merge work.
- **Expected:** Drafts survive close; destructive dismissal asks or offers undo (VS Code keeps merge editor state; Notion and Linear keep drafts).
- **Change:** Keep a per-dbId draft map (e.g. std::unordered_map&lt;int64_t,std::string> on UiDrawSession). On Cancel or Esc, store the buffer if it differs from the seed. On open, restore the draft instead of reseeding, and offer a 'Reset to markers' button. Alternatively, confirm Cancel only when the buffer is dirty.

#### #111 — Choosing one side is a mouse-only click, with no keys or hints for Use Mine, Use Theirs or Save

**Severity: Medium** · Effort S · keyboard · occasional · `conflict-no-pick-hotkeys`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1245-1257 buttons "Use Mine", "Use Theirs", "Save resolved" with no tooltips or shortcuts
- **Today:** The only fast resolution is clicking a button. Tab can reach the buttons, but there are no accelerators, mnemonics or shortcut hints. Use Mine and Use Theirs resolve instantly and irreversibly, with no confirm or undo.
- **Change:** Add Alt+M, Alt+T and Ctrl+Enter accelerators when the popup is focused, and add shortcut text to the button labels or tooltips. Put the Mine and Theirs actions in a shared helper used by the text and scalar panes.

#### #112 — A long pasted or seeded resolution is silently cut off at the fixed buffer size (64K text, 8K scalar)

**Severity: Medium** · Effort S · validation · rare · `conflict-buffer-truncates`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1226 "assign(64 \* 1024"; :1294 "assign(8 \* 1024"; :1415 "(std::min)(seed.size(), static_cast&lt;size_t>(64 \* 1024 - 1))"
- **Today:** If mine + theirs + markers exceed 64K, the seed is truncated, possibly dropping the closing >>>>>>> theirs. Pasted text beyond the capacity is dropped by ImGui with no warning. Use Mine and Use Theirs use the untruncated cc.Mine and cc.Theirs, so the behaviours differ.
- **Change:** Switch ##CRResolved to ImGuiInputTextFlags_CallbackResize with a std::string so the buffer grows. At minimum, show a 'x / 64K' counter or a warning when the length is near capacity, and size the seed to fit mine + theirs.

#### #113 — Save is disabled with no explanation until I delete git-style marker lines from the prefilled text myself

**Severity: Medium** · Effort S · steps · occasional · `conflict-markers-prefill-friction`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1413 seed "&lt;&lt;&lt;&lt;&lt;&lt;&lt; mine\n" + Mine + "\n=======\n" ...; :1253-1260 BeginDisabled on Save; :1231 only checks for "&lt;&lt;&lt;&lt;&lt;&lt;&lt;"
- **Today:** The Resolved box starts as a marker template. To keep both versions I must manually delete three marker lines. Save resolved is greyed out, and the explanation is only the orange text line above. There is no tooltip on the disabled button. Only &lt;&lt;&lt;&lt;&lt;&lt;&lt; is checked, so a leftover ======= or >>>>>>> passes validation and gets saved into the issue.
- **Change:** Add an 'Accept both' button that concatenates mine and theirs. Check for all three markers. Show a hover tooltip on the disabled Save (ImGuiHoveredFlags_AllowWhenDisabled) saying what to remove. Consider seeding with mine instead of markers, to match the scalar pane.

#### #114 — Mine and Theirs are plain read-only boxes with no diff highlighting or copy/insert helper, so I compare by eye

**Severity: Medium** · Effort M · discoverability · occasional · `conflict-readonly-panes-no-diff`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1213 "##CRMine" and :1219 "##CRTheirs" ReadOnly InputTextMultiline; :1163 out.Base parsed but never drawn
- **Today:** Two side-by-side text areas, with the base version parsed but unused. There are no changed-line highlights and no 'insert this side into Resolved' action. Each box is const_cast over a std::string, which is fragile.
- **Change:** Draw a line-based diff of Base vs Mine/Theirs with ImDrawList highlights, as a read-only child instead of InputTextMultiline. Add per-pane 'Copy to resolved' buttons.

#### #115 — A scalar value that is not valid for the field is only rejected after Save, with a generic message

**Severity: Medium** · Effort M · validation · rare · `conflict-scalar-errors-after-the-fact`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1303-1305 "That value cannot be used for this field... for a sprint, its name or id; for an estimate, a value such as 3d."
- **Today:** I type a value in ##SCResolved and press Save value. Only then does ResolveFieldEditConflict fail and one shared message appear at the bottom of the popup, listing hints for all field types. Nothing indicates the field type up front or as I type.
- **Change:** Pass a field-type hint in the conflict context, show a placeholder via ImGui hint text, and validate with the same parser the replay uses in each frame. Disable Save value with a reason while invalid.

#### #116 — Conflict-modal Discard my edit hard-deletes queued work in one click, bypassing the P2-M1 confirm

**Severity: Low** · Effort M · lost-work · rare · `conflict-discard-no-undo`

- **Evidence:** SmatchetOfflineQueueUi.cpp:1346-1351 "Discard my edit" -> DeletePendingFieldEdits then FinishConflictModal
- **Today:** A single click permanently removes the row, with no confirm and no undo toast, only an audit entry. The 150px Discard button sits adjacent to Force Mine.
- **Change:** Soft-delete or hold the row and show an Undo toast in the panel status, or move Discard away from the primary buttons and style it as destructive.

### 7.11 Custom toolbar button editor

The toolbar editor is a modal popup ("Customize Toolbar") opened from the bar's right-click menu or per-button "Edit...". It has a Global/Current-tracker scope selector, an action row (Add command / Add Lua / Add separator / Duplicate / Delete / Up / Down), a 240px Selectable list with drag-drop reorder, and a field pane. The field pane has Kind, Icon name + Pick..., Tooltip, Command id, a "Filter commands..." box over a 90px Selectable list, Args (JSON), and Lua code. Edits go to a buffer (editBuf\_) that is committed only by Save, and Cancel just closes. Every field is plain mouse-driven InputText with no validation, no Enter/Esc handling and no focus management. The only keyboard-reachable paths are Tab between widgets and ImGui's default nav. Reorder/delete is via buttons, drag, or the bar's right-click menu.

**Already good here:**

- Text fields are std::string-backed via CallbackResize, so long tooltip, command id, args and Lua are never truncated (Source/Core/src/Ui/SmatchetToolbarUi.cpp:34-59 InputTextStdString; L641-669)
- Edits are buffered and Save is explicit, with a scope selector and a stable popup id (SmatchetToolbarUi.cpp:412 `editBuf_ = cfg.Toolbar;` and L709 `cfg.Toolbar = editBuf_;`)
- Per-button right-click menu offers Edit, Set shortcut, Move Left/Right, Insert Separator and Delete. It also works on disabled buttons (SmatchetToolbarUi.cpp:302-315, 352-395)
- Icon picker, plus a live glyph preview next to the icon name (SmatchetToolbarUi.cpp:623-638 `iconPicker_.Open()`)
- Drag-drop reorder works with touch, and the bar tooltip flags a missing command (SmatchetToolbarUi.cpp:574 MarkLastItemAsTouchDragSurface; L296 "command not found")

#### #117 — Typing invalid JSON in Args gives no feedback; the button later runs with empty args and only a log warning

**Severity: High** · Effort S · validation · occasional · `toolbar-args-json-unvalidated`

- **Evidence:** SmatchetToolbarUi.cpp:665 `InputTextStdString(..."Args (JSON)", b.ArgsJson)` with no check; L138-141 `// invalid args JSON → dispatch with empty object` + LOG_WARN
- **Today:** User types args such as {'a':1} or a trailing comma, saves, clicks the button. The command runs with {} (possibly doing the wrong thing) and the only trace is a log line. Save is never blocked.
- **Expected:** VS Code settings / Raycast: inline red parse error as you type, with the error position; Save disabled or warned.
- **Change:** In DrawEditorFieldEditor after the Args InputText, call smatchet::json_safe::ParseBounded on b.ArgsJson each frame (cache by string compare). Draw ImGui::TextColored error text with parseErr below the field and a red frame via PushStyleColor(ImGuiCol_Border). Optionally disable Save with BeginDisabled plus a tooltip when any button has invalid args.

#### #118 — The command filter is case-sensitive substring, so typing 'refresh' misses 'Refresh'. There is no fuzzy match

**Severity: High** · Effort S · parsing · occasional · `toolbar-filter-case-sensitive`

- **Evidence:** SmatchetToolbarUi.cpp:655 `n.find(q) == std::string::npos` over cmdNames\_ (L444-448 c.Name)
- **Today:** The list is empty unless the case and contiguous substring match exactly. There is no word-order or typo tolerance. Whitespace is not trimmed.
- **Expected:** Command palettes (VS Code, Raycast, Linear) are case-insensitive, subsequence/fuzzy and ranked.
- **Change:** Reuse the palette's matcher (the Command palette auditor owns it) in place of n.find(q). Fall back to a case-insensitive subsequence match plus trim, and sort the matches by score.

#### #119 — The command filter and list have no keyboard commit: Enter in the filter does nothing, and there is no arrow-key selection

**Severity: Medium** · Effort M · keyboard · occasional · `toolbar-filter-no-enter-pick`

- **Evidence:** SmatchetToolbarUi.cpp:649-651 InputTextWithHint with no EnterReturnsTrue; L658 Selectable is the only way to fill CommandId
- **Today:** After filtering, the user must grab the mouse and click a row to fill Command id. Nothing is highlighted as the top hit.
- **Change:** Use ImGuiInputTextFlags_EnterReturnsTrue on ##cmdsearch. Keep a highlighted index, move it with Up/Down via IsKeyPressed, and on Enter set b.CommandId to that entry. Draw the highlight with Selectable(…, highlight). Or enable ImGuiConfigFlags_NavEnableKeyboard on the child.

#### #120 — Opening the editor or clicking Add command puts the caret nowhere, so the user must click a field before typing

**Severity: Medium** · Effort S · focus · daily · `toolbar-no-focus-management`

- **Evidence:** SmatchetToolbarUi.cpp:507-513 Add command pushes button, sets selected\_ only; no SetKeyboardFocusHere / SetItemDefaultFocus in L598-678
- **Today:** Add command creates a button with the tooltip 'New button' and an empty Command id. Nothing is focused. The user must click the filter or Command id box.
- **Change:** Set a newlyAdded\_ flag in the Add handlers. In DrawEditorFieldEditor call ImGui::SetKeyboardFocusHere() before the ##cmdsearch input for Command kind (Tooltip for Lua). On editor open, focus the list or filter.

#### #121 — Enter and Esc do not Save or Cancel the editor, and Cancel throws away all edits with no prompt

**Severity: Medium** · Effort M · lost-work · occasional · `toolbar-no-enter-esc-save-cancel`

- **Evidence:** SmatchetToolbarUi.cpp:690 Save button, L715-717 `Cancel` → CloseCurrentPopup(); popup opened without a close-flag handler (L724)
- **Today:** Esc closes the modal (ImGui default) and silently drops the whole editBuf\_. A mis-click on Cancel or Esc after many edits loses everything. Ctrl+Enter does not save.
- **Change:** Compare editBuf\_/trackerEditBuf\_ to the originals (a dirty flag). On Esc or Cancel while dirty, show a small 'Discard changes?' popup. Bind Ctrl+Enter to the Save branch with IsKeyChordPressed. Show the shortcut in the button label.

#### #122 — The button list cannot be reordered or deleted from the keyboard. Delete has no key, and Up/Down are mouse buttons only

**Severity: Medium** · Effort M · keyboard · occasional · `toolbar-list-no-keyboard-reorder-delete`

- **Evidence:** SmatchetToolbarUi.cpp:568-573 Selectable only; L542-555 Delete/Up/Down are Buttons; no IsKeyPressed in file's editor path
- **Today:** Selecting rows needs Tab/nav plus Space. The Delete key, Alt+Up and Alt+Down do nothing. Delete is immediate with no undo, so a mis-click loses the button.
- **Change:** When the ##tblist child is focused (IsWindowFocused), handle ImGuiKey_Delete, Alt+Up/Down and Ctrl+D by calling the same lambdas as DrawEditorActionRow. Add a one-step undo (stash the erased ToolbarButton and offer an 'Undo delete' link) instead of a confirm.

#### #123 — Args (JSON) has no placeholder or example, and the editor never says which args the chosen command accepts

**Severity: Medium** · Effort S · discoverability · occasional · `toolbar-args-no-hint`

- **Evidence:** SmatchetToolbarUi.cpp:665 plain InputText (no InputTextWithHint) vs L649 filter uses a hint
- **Today:** An empty box labelled 'Args (JSON)'. The user must know the schema from docs or guess keys.
- **Change:** Switch to ImGui::InputTextWithHint using a std::string callback with hint `{}` / an example. If the command registry exposes arg metadata, show it in a TextDisabled line under the field.

#### #124 — Command id is a free-text box with no check, so a typo only shows up later as a disabled button with 'command not found'

**Severity: Medium** · Effort S · validation · occasional · `toolbar-command-id-freetext`

- **Evidence:** SmatchetToolbarUi.cpp:646 InputTextStdString Command id; L658-659 `b.CommandId = n`; L296 'command not found' on bar tooltip only
- **Today:** User can type any id and Save with no check; unknown ids show 'command not found' only on the toolbar tooltip (L296). Ids picked from the list are valid because the list holds the same Name strings the id is set from (L447, L658-659).
- **Change:** After the Command id input, test b.CommandId against cmdNames\_ and show a red TextDisabled 'Unknown command – pick from the list' with a click-to-apply closest match. Also disable Save, or warn, on unresolved ids.

#### #125 — Icon name is a fixed 64-byte buffer with no feedback when the name matches no icon. Typing a name relies on the picker

**Severity: Low** · Effort S · validation · rare · `toolbar-icon-name-buffer`

- **Evidence:** SmatchetToolbarUi.cpp:615-621 `char nameBuf[64]` + `SmatchetToolbarIconGlyph(b.IconName)`
- **Today:** A mistyped icon name sets an empty IconGlyph (L620) and the editor shows no preview and no message (L634-638); the claim about tooltip fallback on the bar was not verified.
- **Change:** Show 'No such icon' via TextDisabled when b.IconGlyph is empty and b.IconName is not. Optionally open the picker with the typed text as a filter.

#### #126 — New buttons start with hard-coded English tooltip 'New button', Add buttons append at the end instead of after the selection, and no last-values are remembered

**Severity: Low** · Effort S · steps · daily · `toolbar-default-labels-and-no-clone-defaults`

- **Evidence:** SmatchetToolbarUi.cpp:510 `b.Tooltip = "New button"`, L511 `buttons.push_back(b)`, L518 'New Lua button'
- **Today:** Tooltip and icon are placeholders the user must overwrite. A button added while another is selected goes to the end, so a second reorder step is needed. The label is not localized, unlike every other string.
- **Change:** Insert at selected\_+1. When a command is picked from the list and the Tooltip is still the default or empty, fill Tooltip from the command name. Wrap the defaults in SmatchetLocalization::T.

### 7.12 Annotate / analysis settings and path remaps

Annotate settings are split across two Preferences bodies. Annotate > Analysis (DrawAnnotateAnalysisFields) holds Max frames, ignore keywords (multiline), changelist cache size, and the AI chat URL. Connections > Perforce (DrawAnnotatePerforceFields) holds the p4/p4vc executables, timelapse and changelist command templates, and a path-remap row editor (DrawAnnotatePathRemaps). All text fields are plain ImGui::InputText into fixed char buffers (260/260/1024/512/512, remap 512 each). Each field is committed to config only when the item deactivates after an edit (IsItemDeactivatedAfterEdit) and is saved through an off-thread coalescing worker. There is no validation, no browse/auto-detect, and no test button. The remap list uses per-row from/to inputs with Remove and Add buttons, all mouse-driven.

**Already good here:**

- Edits commit on deactivation, so closing Preferences or tabbing away never loses a changed setting; saves run off the UI thread via a coalescing worker (Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:25-31 CommitTextField uses IsItemDeactivatedAfterEdit then PersistAnnotateCfg; :20 ScheduleAnnotateConfigSaveDetached)
- Numeric settings are clamped to sane ranges and the clamp is described in a tooltip (cache size) (AnnotateAnalysisUi_Preferences.cpp:105 "clamped 16..8192" and :108 std::max(16, std::min(8192,...)))
- Path-remap semantics are explained in-field and the engine is robust (longest prefix wins, empty From skipped) (AnnotateAnalysisUi_Preferences.cpp:205 "Longest matching prefix wins; add rules in any order."; CallstackParser.cpp:464 if (from.empty()) continue)
- Custom command launch is hardened against quote/backslash/newline injection (Source/Core/src/Ui/P4vLaunch.cpp:185 "custom command rejected because a {file}/{cl} value contains a double-quote")
- Timelapse field shows a placeholder legend under the field, and the Jira field-mapping combos have explainers and autoselect by name (AnnotateAnalysisUi_Preferences.cpp:138 "Placeholders: {file} {line} {cl}"; AnnotateAnalysisUi_Config.cpp:90-107 MaybeAutoselect\*)

#### #127 — A command template that is rejected or fails to parse does nothing visible; the user only gets a log line

**Severity: Medium** · Effort M · validation · occasional · `annotate-template-reject-silent`

- **Evidence:** Source/Core/src/Ui/P4vLaunch.cpp:185 LOG_WARN("custom command rejected because a {file}/{cl} value contains a double-quote..."); :199 LOG_WARN("custom command parse failed. Expected: &lt;exe> [args]"); :206 ShellExecuteW failure only LOG_WARN
- **Today:** User types a timelapse/changelist template, clicks Timelapse on a row, and nothing opens when the template is malformed, the exe is missing, or a path ends in a backslash. No toast, no status text, no hint what is wrong.
- **Change:** Use the bool that LaunchP4VcLike already returns. Have the three callers (Window.cpp:383,463,640) set State().lastUiStatus to 'Error: ...' (red, per P2-M17) when it is false, and add a short reason string from the helper. Reference P2-M17 as the pattern to reuse, not as an open overlap.

#### #128 — p4/p4vc executable paths and the AI chat URL are never checked, so typos surface only when a run fails later

**Severity: Medium** · Effort M · validation · occasional · `annotate-no-path-validation`

- **Evidence:** Source/Core/src/Ui/AnnotateAnalysisUi_Preferences.cpp:126-127 InputText("p4 executable") then CommitTextField; :115-116 aiUrl with CommitTextField; Window.cpp:64 app.OpenUrl(std::string(State().aiUrl))
- **Today:** Any text is accepted for p4/p4vc/AI URL. A wrong p4 path fails later. A scheme-less or non-http(s) AI URL is rejected inside AppController::OpenUrl (HostIntegration.cpp:125,141) with only a LOG_WARN, so 'Ask AI' copies to the clipboard and silently opens nothing. There is no Browse, Detect or Test.
- **Change:** After each frame compare the buffer to a cached std::filesystem::exists result (re-evaluated only on edit, not per frame) and draw a small TextColored 'not found' hint beneath; add a 'Detect' button that probes PATH for p4/p4vc and a 'Test' button running 'p4 -V'. For aiUrl, require an http(s):// prefix and show 'Expected https://...' inline.

#### #129 — The changelist command template has no placeholder legend and the timelapse legend sits below the field only

**Severity: Medium** · Effort S · discoverability · occasional · `annotate-change-template-undocumented`

- **Evidence:** AnnotateAnalysisUi_Preferences.cpp:141-144 changeTpl InputText with no hint, vs :138 "Placeholders: {file} {line} {cl}" only under Timelapse; P4vLaunch.cpp:189-191 substitutes {file},{line},{cl}
- **Today:** Changelist command field is a bare empty box; the user must guess which tokens exist, that the first token is the exe, and that quotes matter. Neither field has a placeholder example, and the 'optional' fields do not say what the default behaviour is.
- **Change:** Use ImGui::InputTextWithHint with a concrete example (e.g. p4vc.exe timelapse "{file}"), add the same legend under the Changelist field (only {cl} and {file} are meaningful there), and add small buttons that insert tokens through an InputText callback at the cursor.

#### #130 — Path remap rows cannot be added, advanced or removed from the keyboard, and Remove has no undo

**Severity: Medium** · Effort S · keyboard · occasional · `annotate-remap-no-keyboard-flow`

- **Evidence:** AnnotateAnalysisUi_Preferences.cpp:244 ImGui::Button("Add path remap") appends an empty rule with no SetKeyboardFocusHere; :228 ImGui::Button("Remove"); :235-243 erase + PersistAnnotateCfg("remove_remap") immediately
- **Today:** Add path remap creates an empty row but focus stays on the button, so the user must click into 'from'. Enter in 'to' does not add the next row. Remove deletes the rule at once and persists it, with no undo.
- **Change:** On Add call ImGui::SetKeyboardFocusHere on the new row's 'from' (track a pendingFocusRow in State()); use ImGuiInputTextFlags_EnterReturnsTrue on 'to' to add the next row when the current one is filled; keep a one-slot lastRemoved rule and show an 'Undo remove' TextDisabled link for a few seconds.

#### #131 — Remap prefixes match raw, case-sensitively and slash-sensitively, so a rule that looks right silently never applies

**Severity: Medium** · Effort M · parsing · occasional · `annotate-remap-exact-match-no-normalize`

- **Evidence:** Source/Core/src/CallstackParser.cpp:466 path.compare(0, from.size(), from) == 0 (no case folding, no separator normalisation); no trimming in Preferences.cpp:231 remaps[i].FromPrefix = State().remapBufs[i].from
- **Today:** A rule 'D:\\Work\\Game' does not match 'd:/work/game/...' frames from a crash report. A pasted prefix with trailing space or a missing trailing slash fails or produces a double/missing separator. No indication that a rule matched nothing.
- **Change:** Trim both fields on commit, compare in ApplyPathRemaps with a normalised (lowercased, '/' folded) copy on Windows while replacing on the original length, and add a small 'Preview' input in the remap section that shows the rewritten result for a sample path.

#### #132 — Long paths, templates or URLs stop accepting input or truncate on paste with no warning

**Severity: Low** · Effort M · validation · rare · `annotate-buffer-silent-truncation`

- **Evidence:** AnnotateAnalysisUi_Internal.h:90-94 char p4Exe[260]; char timeTpl[1024]; char changeTpl[512]; char aiUrl[512]; :54-55 remap from/to[512]; Preferences.cpp:121 InputText(...sizeof(...))
- **Today:** ImGui clips input at the buffer size. A p4 path over 259 bytes (long UNC / non-ASCII UTF-8 paths cost more bytes) or a long URL with query is cut mid-string and saved as the truncated value, with no indicator.
- **Change:** Switch these fields to ImGuiInputTextFlags_CallbackResize with std::string (ImGui misc/cpp/imgui_stdlib), or at minimum draw a red 'limit reached' hint when strlen == sizeof-1.

#### #133 — Remap rows give no feedback for an empty From, duplicate prefixes, or an unfilled new row

**Severity: Low** · Effort S · validation · occasional · `annotate-remap-no-row-validation`

- **Evidence:** AnnotateAnalysisUi_Preferences.cpp:245 remaps.push_back(PathRemapRule{}) persisted immediately at :247; CallstackParser.cpp:464 empty from silently `continue`
- **Today:** An empty rule is saved and silently ignored. Duplicate prefixes are accepted. Rows have bare 'from'/'to' labels with no hint text. The section header does carry a one-line explanation of longest-prefix-wins.
- **Change:** Use InputTextWithHint ("from: /build/agent/src", "to: D:/Work/Game"), color the row's from field red/warn when empty or duplicated, and skip persisting a rule until From is non-empty.

#### #134 — The ignore-keywords box has no hint text or example, and the cap/format is invisible

**Severity: Low** · Effort S · discoverability · occasional · `annotate-ignore-keywords-hints`

- **Evidence:** AnnotateAnalysisUi_Preferences.cpp:93 InputTextMultiline("Ignore keywords (comma or newline)", ... ImVec2(-1, 60)); Internal.h:84 ignoreBuf 4096
- **Today:** Empty multiline box labelled with the separators only; no sample ('Engine/Source, FMemory'), no mention that matching is case-insensitive substring over the whole raw line, function and path (CallstackParser.cpp:489-491), so users can accidentally filter far more than intended. The 4096-byte cap is silent.
- **Change:** Add a TextDisabled line 'Case-insensitive substring match on the frame text', show 'N of M frames hidden' using the existing keyword matcher against State().callstackBuf, and consider a chip-style editor later.

#### #135 — Out-of-range numeric values are silently rewritten on deactivate, without saying the range up front (Max frames)

**Severity: Low** · Effort S · validation · rare · `annotate-max-frames-silent-clamp`

- **Evidence:** AnnotateAnalysisUi_Preferences.cpp:84-88 ImGui::InputInt("Max frames") then std::max(1, std::min(500, ...)) with no tooltip, unlike the cache size at :105
- **Today:** Typing 1000 becomes 500 after focus leaves; the 1..500 range is never shown. InputInt also shows +/- buttons and applies on deactivate, so a value is not live while typing.
- **Change:** Add the same DelayShort tooltip ('clamped 1..500') as cache size at Preferences.cpp:103-106. No flag change is needed because Enter already commits via deactivation.

### 7.13 File drop, drag gestures and touch-only input paths

File drop is handled by a single GLFW drop callback, registered at Source/Standalone/main.cpp:613 and StandaloneAppBootstrap.cpp:567. It keeps only .md/.markdown paths and hands them to the Plan Docs viewer, which opens and takes focus. Other files are counted and logged. Panels are resized and hidden by mouse gesture: the bottom grip, the side reveal strip, and release-to-hide when a splitter drag ends in a band at the edge. The only keyboard paths I found are toggles: Ctrl+B, Ctrl+Alt+B and Ctrl+J. Grid header reorder is mouse-drag with edge auto-scroll. Touch input is translated into mouse events, with long-press mapped to right-click and a progress ring.

**Already good here:**

- Touch long-press is a defined equivalent of right-click, with a visible progress ring. The long-press duration is configurable and clamped (Source/Core/src/Ui/TouchInputRouter.cpp:161 SetLongPressSeconds; :327 ActionType::RightDown -> io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); :457 LongPressProgress)
- Grid header drag-reorder auto-scrolls at the strip edge, keeps frozen columns in place, and resolves its target from column centres (Source/Core/src/Ui/SmatchetGridColumnInteraction.cpp:31-88 DriveGridHeaderDragReorder, GridHeaderDragAutoScrollSpeed)
- The panel hide gesture gives live feedback (a release-to-hide hint). The reveal gesture sets a resize cursor, highlights the strip, and yields to active text fields, scrollbars and column drags (Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp:290 DrawReleaseToHideHint; :346 ShouldYieldToActiveItem(... g.InputTextState.ID == g.ActiveId ...); :397 SetMouseCursor(ResizeEW))
- Side bars and the bottom panel have rebindable keyboard toggles (docs/guides/keyboard-shortcuts.md:128-130 Primary Side Bar Ctrl+B / Secondary Ctrl+Alt+B / Panel Ctrl+J)
- Touch hit targets scale with a density setting. A drop of a document already in the scan list is deduplicated instead of duplicated (Source/Core/src/Ui/SmatchetMobileShellUi.cpp:37-38 'scaled by the touch-density factor so a Comfortable shell gets larger hit targets'; Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:584 FindCombinedIndex(s, normalized))

#### #136 — Dropping a non-markdown file does nothing visible, and a mixed drop silently loses the non-markdown files

**Severity: High** · Effort S · discoverability · occasional · `drop-rejected-silent`

- **Evidence:** Source/Standalone/StandaloneBoot_detail.cpp:124-127 LOG_INFO("file drop: ignored %d non-markdown file(s) - only .md/.markdown open in the Plan Docs viewer")
- **Today:** The user drags a screenshot, CSV or log onto the window. The callback counts it as ignored and writes one log line. Nothing appears in the UI, so the app looks like it ignored the gesture. If a batch mixes .md and other files, only the .md files open.
- **Expected:** Linear, Notion and Jira show a drop overlay while dragging and a toast on rejection that says what is accepted, or they accept the file as an attachment.
- **Change:** In MarkdownFileDropCallback, queue a toast (the existing notification path) such as 'Ignored N file(s): only .md opens here' when ignored > 0. Do this from a flag on g_ui that the next frame drains, since the callback runs outside the frame. Separately, track hover with a platform drag-enter hook where GLFW or the backend allows it, and draw a 'Drop .md to open' overlay.

#### #137 — Files cannot be dropped onto an issue draft, a comment or an attachment area. Any drop anywhere only opens the Plan Docs viewer

**Severity: High** · Effort M · steps · occasional · `drop-global-markdown-only`

- **Evidence:** Source/Standalone/main.cpp:613 glfwSetDropCallback(window, MarkdownFileDropCallback); Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:594-599 d.showPlanDocViewer = true; d.requestPlanDocViewerFocus = true
- **Today:** The callback ignores where the cursor is. A drop onto a draft or comment either does nothing (non-markdown) or pops the Plan Docs window and takes focus away from the field the user was working in (markdown). Attaching a file means going through the picker (RequestOpenFilePaths, SmatchetUI.cpp:1333).
- **Expected:** Modern trackers (Jira, Linear, GitHub) accept drag-and-drop attachments and inline images onto the composer or comment box. The target is hit-tested and highlighted.
- **Change:** Make the drop callback route by target. Store the paths plus the cursor position (glfwGetCursorPos) in a pending-drop slot on g_ui. At frame start, compare the cursor with the registered rects of drop zones such as the draft body, the comment composer and the attachment list, and dispatch to that surface's handler (attach or insert path). Fall back to Plan Docs only for .md files that land outside every zone. Zones register their rect with a small DropZone registry during draw. ImGui has no native OS-file drop target, so hit-testing by rect is the mechanism.

#### #138 — Panels cannot be resized, and columns cannot be reordered or resized, from the keyboard

**Severity: Medium** · Effort M · keyboard · daily · `no-keyboard-resize-reorder`

- **Evidence:** Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp:300,374,457 (every resize/reveal path keys on IsMouseReleased/IsMouseClicked/IsItemActive); Source/Core/src/Ui/SmatchetGridColumnInteraction.cpp:38 IsMouseDragging(ImGuiMouseButton_Left); docs/guides/keyboard-shortcuts.md:128-130 lists toggles only
- **Today:** Ctrl+B, Ctrl+Alt+B and Ctrl+J show or hide panels. Setting a panel's size, or moving or widening a column, needs a mouse drag. A grep of Source/Core/src for column move/resize/reorder command ids found none. I did not find a mouse-free route to either action.
- **Change:** Register commands such as view.panel.grow / view.panel.shrink in AppViewCommands.cpp (RegisterPanel, L166). Have them nudge the dock node size through ImGui::DockBuilderSetNodeSize or the existing ExpandEdgePanel/CollapseEdgePanel helpers. Add grid.column.move_left / move_right commands that call ImGui::TableQueueSetColumnDisplayOrder on the focused column. Register them so they also appear in the command palette and the keybinding editor.

#### #139 — A markdown drop immediately steals focus and opens a window, even while the user is typing elsewhere

**Severity: Low** · Effort S · focus · rare · `drop-steals-focus`

- **Evidence:** Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:597-599 s.selectedIdx = firstIdx; d.showPlanDocViewer = true; d.requestPlanDocViewerFocus = true;
- **Today:** A stray or accidental .md drop mid-edit moves focus to the Plan Docs window. Typing is then dropped or lands in the wrong place, and nothing returns focus afterwards.
- **Change:** Skip or defer the focus request when io.WantTextInput is true and show a toast with an Open action; focus restore on close is secondary.

#### #140 — Hidden-panel reveal strip has no resting cue and its tooltip omits the shortcut

**Severity: Low** · Effort S · discoverability · daily · `reveal-grip-undiscoverable`

- **Evidence:** Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp:396-404 strip highlight and tooltip only when inStrip && reachable; :418-419 'A side column that was never hidden by the gesture / a collapse has nothing to restore: no grip (the user reopens it from the menu)'
- **Today:** A hidden panel has a reveal strip (7px, SmatchetBottomPanelDragPure.h:14) that highlights and shows a tooltip on hover (SmatchetBottomPanelDrag.cpp:396-404, tips such as "Drag up to open the panel" at :95 and "Drag left to open the side panel" at :110). It has no resting cue, and the tooltip does not name the shortcut. A menu route exists with the shortcut shown (SmatchetUI_MainMenu.cpp:634 MenuItem("Panel", MenuShortcut(ctx,"view.panel.bottom","Ctrl+J"))), so the panel is not undiscoverable, only the gesture lacks a resting hint.
- **Change:** In DrawRevealGrip, draw a thin, always-visible affordance (a 2-3 px accent line or a small chevron) while the panel is collapsed. Append the bound shortcut to revealTipFallback, using MenuShortcut(ctx, "view.panel.bottom", "Ctrl+J") as the main menu already does at SmatchetUI_MainMenu.cpp:634.

#### #141 — A hide-by-drag gesture has no undo or cancel path

**Severity: Low** · Effort S · lost-work · occasional · `drag-hide-no-undo`

- **Evidence:** Source/Core/src/Ui/SmatchetBottomPanelDrag.cpp:300-305 const bool hide = st.hideArmed && ::ImGui::IsMouseReleased(ImGuiMouseButton_Left); ... CollapseEdgePanel(d, spec);
- **Today:** Release inside the hide band collapses the panel (SmatchetBottomPanelDrag.cpp:300-305), but the pre-drag size is captured at drag start (:280-283 st.restoreExtent) precisely so a reopen restores the size the user worked at. Only the Esc-cancel part is missing, and ImGui's splitter does not cancel on Esc by default; a hint is shown while armed (:289-291 DrawReleaseToHideHint) and moving back out of the band disarms (:287-288, comment :297-299).
- **Change:** Optional: treat Esc during an armed drag as disarm (clear hideArmed). Size restore already exists, so do not add a size-restore step.

#### #142 — A dropped file the viewer cannot read still opens the viewer and shows an error inside it

**Severity: Low** · Effort S · validation · rare · `drop-no-existence-feedback`

- **Evidence:** Source/Core/src/Ui/SmatchetPlanDocViewerUi.cpp:581-582 'No existence check here (that is file I/O on the render thread) ... an unreadable path surfaces the viewer's normal error body'
- **Today:** A drop of a missing, locked or huge .md file adds a picker row and opens the viewer, whose body then shows an error. No toast explains it. The row stays in droppedFiles with no cue to remove it.
- **Change:** When the async load result in the viewer reports failure for a dropped path, raise a toast and remove the entry from s.droppedFiles. Keep the check on the worker thread, as the existing comment requires.

---

## 8. Limits

- The app was never run. Anything that depends on Dear ImGui runtime navigation (which button receives default focus in a confirm dialog, for example) is inferred from the code. Run-and-watch confirmation is the recommended step before acting on the High findings that hinge on it (#86, #85).
- Severity and effort are judgments made from the code, without usage data.
- `file:line` references were confirmed against the tree at the branch head on the report date and will drift.
- The mobile shell and the Unreal-embedded input path were only read where they share code with the desktop client.
- Call-site counts quoted in findings are approximate.
