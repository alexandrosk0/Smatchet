# Keyboard shortcuts

Every in-app keyboard shortcut in Smatchet is **rebindable**. Bindings map a key
combo to a *command* (the same command id the palette, CLI, MCP, and Lua all
dispatch) — so rebinding a shortcut changes only the keystroke, never what the
command does. This page covers where to rebind, how conflicts are handled, where
a bound combo shows up, and which shortcuts are *not* rebindable.

## Rebind from Preferences

**Preferences → Keyboard Shortcuts** is the full editor.

- A searchable table lists every rebindable command. Type in the filter box to
  narrow by action name or current combo.
- Each combo on a row is a **chip**. Click a chip to **capture** a replacement:
  press the keys you want (e.g. `Ctrl+Shift+G`), or press **Esc** to cancel. The
  capture widget shows the live combo as you hold modifiers.
- **On** toggles a binding off without forgetting its combos — an off binding
  never fires but keeps its assignments for when you turn it back on.
- **Clear** removes **every** combo on the row, leaving the command unbound.
- **Reset all to defaults** restores the shipped default set (table below).

### Several combos for one command

A command can carry any number of **alternative** combos — press any one of them
and the command runs. Zoom In ships this way, because "Ctrl and +" is a different
physical keystroke from `Ctrl+=` on most layouts and the numeric keypad `+` is a
third.

- **+ Add** on a row captures another combo and appends it.
- The **x** beside a chip drops just that combo; the others stay.
- **Clear** (in the row's right-hand column) drops all of them at once.

Menus and the command palette show only the **first** combo — their shortcut
columns are too narrow for a list. Hover a toolbar button to see the full set.

Changes save automatically (no Apply button) via the same off-thread config
write the rest of Preferences uses.

## Quick-bind from where you use a command

You don't have to open Preferences. **Right-click → Set shortcut…** is available
on:

- **Toolbar buttons** that run a command.
- **Command-palette rows** (`Ctrl+Shift+P`, then right-click any entry).

The quick-bind popup captures one combo for that command, warns if the combo is
already taken, and offers **Set** / **Clear** / **Cancel**. **Set** makes the
captured combo the command's *only* combo — if the command already has several,
the popup lists them first so you know what you are replacing. Use the Preferences
editor to add a combo without dropping the others.

## Conflicts are warned, not blocked

A combo bound to two commands is flagged inline — the editor shows
*"conflicts with <other action>"* and the quick-bind popup shows *"Already bound
to: <other action>"*. The bind is still allowed: Smatchet **warns, it does not
block**. The last-pressed-wins behaviour is intentional so you can deliberately
move a combo from one command to another in two steps (bind the new one, the old
one surfaces as the conflict, then clear it).

Several combos per command are supported — a command can carry any number of
alternative combos, and all of them fire it. Multi-key **chords** (press-then-press
sequences like `Ctrl+M` then `Z`) are still not user-rebindable.

## Writing a combo by hand

The editor is the normal route, but `smatchet_config.json` is readable and the
grammar is worth knowing:

- Modifiers: `Ctrl` (or `Control`), `Shift`, `Alt`, `Super` (or `Win` / `Cmd`),
  joined to the key with `+` — `Ctrl+Shift+F`.
- Main keys: `A`–`Z`, `0`–`9`, `F1`–`F12`, the punctuation keys
  `= - , . / ; ' \` [ ] \`, and the named keys `Space`, `Enter`/`Return`, `Tab`,
  `Backspace`, `Delete`/`Del`, `Escape`/`Esc`, `Insert`/`Ins`, `Home`, `End`,
  `PageUp`/`PgUp`, `PageDown`/`PgDn`, `Up`, `Down`, `Left`, `Right`.
- **The `+` key itself**: write it last — `Ctrl++` — or spell it `Ctrl+Plus`.
  Both mean the same keystroke as `Ctrl+Shift+=`, because on a US/ANSI layout the
  `+` character *is* `Shift` and `=`. Smatchet stores the canonical
  `Ctrl+Shift+=` form, so that is what you will see written back. `_` / `Underscore`
  works the same way for `Shift+-`. A `+` anywhere other than the end is still the
  separator, so `Ctrl++B` means `Ctrl+B`.
- **Numeric keypad** keys are punctuation-free tokens (a `Num+` token would split
  on the `+`): `Num0`–`Num9`, `NumAdd`, `NumSubtract`, `NumMultiply`, `NumDivide`,
  `NumDecimal`, `NumEnter`, `NumEqual`. These are layout-independent, which is why
  the zoom defaults include them alongside the main-row combos. With NumLock off,
  some keyboards report `Num0`–`Num9` as navigation keys instead; the `NumAdd` /
  `NumSubtract` keys are unaffected.
- **Mouse wheel**: `MouseWheelUp` / `MouseWheelDown` (aliases `WheelUp` /
  `WheelDown`). These match the current frame's wheel delta sign (not a key
  press). The zoom defaults bind `Ctrl+MouseWheelUp` / `Ctrl+MouseWheelDown`.
  Bare wheel stays unbound so ordinary scrolling is unchanged; capture requires
  Ctrl, Alt, or Win like any other non-F-key combo.

An unparseable combo is skipped with a warning in the log — the rest of your
bindings still load.

## Where a bound combo shows up

Once a command has a binding, its combo surfaces automatically at every place
that command appears:

- **Menu items** show the combo on the right (View, Tools, Help menus, etc.).
- **Toolbar buttons** show it in their hover tooltip.
- **Command-palette rows** show it on the right of the row.

These read live from your bindings, so a rebind is reflected everywhere the next
frame — no restart.

## System shortcuts (not rebindable)

A few modal / sequence shortcuts stay fixed and appear in a read-only **System
shortcuts** section of the editor:

| Shortcut | Action |
|---|---|
| `Ctrl+M`, then `Z` | Toggle Zen mode |
| `Esc` `Esc` | Exit Zen mode |
| `Up` / `Down` / `Enter` / `Esc` | Command-palette navigation |

These are multi-key or modal-local and are out of scope for v1 rebinding.

The **Preferences** window has fixed keys of its own, active only while it has
keyboard focus:

| Shortcut | Action |
|---|---|
| `Ctrl+S` | Save & Sync, and save any unsaved Assistant settings |
| `Enter` in a tracker connection field | Test connection |
| `Enter` in the settings search box | Put the caret on the first matching setting |
| `Esc` in the settings search box | Clear the search and leave the box |

The settings search is also a rebindable command, `preferences.search.focus`
(`Ctrl+Shift+,` by default): it opens Preferences, or raises it, with the caret in the
search box, and the box's placeholder names the key. Closing Preferences clears the search.

Opening Preferences puts the keyboard in the first blank connection field while the
tracker is still being set up, and in the settings search box afterwards.

The dialogs and panels you type into share fixed keys, active while they have keyboard
focus:

| Dialog | Shortcut | Action |
|---|---|---|
| Log work | `Enter` in **Time spent** | Save |
| Log work | `Ctrl+Enter` in any field | Save |
| Log work | `Esc` | Cancel. With the caret in a field and text typed, the first `Esc` only leaves the field. What you typed is kept and comes back when you open Log work on that issue again (**Clear** drops it) |
| Comments | `Ctrl+Enter` in the comment box | Post the comment |
| Comments | `Esc` | Leave the comment box, keeping the text; then close (asks before discarding a comment). In an empty box, close at once. A comment you did not discard is kept for that issue and comes back when you open Comments on it again |
| Long-text editor | `Ctrl+Enter` | Save |
| Long-text editor | `Esc` | Cancel (asks before discarding unsaved changes). Unsaved changes are kept per issue and field, and come back when the editor opens on it again (**Use saved text** drops them) |
| Long-text editor | `Ctrl+P` | Cycle Edit / Split / Preview |
| Assistant | `Esc` while a reply is coming in | Stop the reply. The message box keeps the caret and what you typed |
| Assistant | `Esc` in the message box | Leave the box, keeping the text, and go back to where you were |
| Offline queue | `Enter` on a **Conflict** (or **Held**) row | Open the merge-conflict resolver |
| Merge-conflict resolver | `Enter` in the value field | Save the value |
| Merge-conflict resolver | `Ctrl+Enter` | Save (a text conflict: only once every `<<<<<<<` marker is gone) |
| Merge-conflict resolver | `Esc` | Leave the field, keeping the text. With nothing typed, close. `Esc` never discards a typed resolution: use **Cancel** |
| Merge-conflict resolver | `Alt+M` / `Alt+T` | **Use Mine** / **Use Theirs** |
| Report a bug | `Ctrl+Enter` | Submit |
| Annotate | `Ctrl+Enter` in the callstack box | Process |
| Scripts & Actions | `Ctrl+Enter` or `F5` | Run the open script (the row's **Run** / **Run on selected** button) |
| Dictation overlay | `Esc` while recording | Cancel: the clip is dropped, nothing is transcribed or typed |
| Customize Toolbar | `Ctrl+Enter` | Save |
| Customize Toolbar | `Esc` | Cancel, when nothing is changed. With an edit made (a field, a move or a removal), `Esc` only leaves the field, and with no field to leave it does nothing: **Cancel** discards the edits |
| Save view as new | `Enter` in the name, or `Ctrl+Enter` | Save |
| Save view as new | `Esc` | Cancel. With a name of your own typed and the caret in it, the first `Esc` only leaves the field |

A submit button names its key at its right edge (**Post Comment** `Ctrl+Enter`, **Save**
`Ctrl+Enter`), the way a menu item names its shortcut, and a composer's placeholder sits
inside the empty box.

Typed text that has not been sent is saved as you type, off the UI thread, in
`smatchet_drafts.json` beside your settings (comments, long-text edits, Log work, the
Assistant message box and a typed merge resolution), so a restart or a crash does not
lose it. Sending, saving or choosing **Discard** erases it.

A confirmed action can be taken back from its notification for a few seconds: clearing
the Assistant conversation. The notification has an **Undo**
button, and `Ctrl+Z` does the same while no text field has the caret and no dialog is
open. Dismissing the notification, or letting it go, makes the action final. Clear is
unavailable while a reply is coming in.

Dragging a side bar or the bottom panel into the edge of the window shows "release to hide";
`Esc` while dragging cancels the hide.

Log work opens with the caret in **Time spent**, Comments opened from a cell with the
caret in the comment box, and the long-text editor with the caret in the text (when it
opens in Preview, the caret goes there once the editor pane is shown).
Asking for the Assistant (`Ctrl+Shift+A`, **View → Assistant**, or revealing its side bar)
puts the caret in the message box. A panel that was already open when the app started
leaves the keyboard where it is.

### Confirm dialogs

Every confirm dialog (Delete view, Discard changes, Discard queued work, Delete local
database, Reset keyboard shortcuts, Reset window layout, Clear conversation, Unsaved
changes in Preferences or the Lua console, and the custom-endpoint and outbound-context
consent prompts) uses the same keys:

| Shortcut | Action |
|---|---|
| `Enter` | Run the button that has the keyboard |
| `Esc` | Cancel: the same as the last button (**Cancel**, **Keep**, **Keep editing**, **Keep my shortcuts**) |
| `Tab` / arrows | Move between the buttons |

When a confirm opens, the keyboard is on its safe button if the action deletes or
replaces something, or if it is a consent prompt. Otherwise it is on the action
(**Save & switch**, **Save & Sync**, **Save**). So pressing `Enter` straight away never
deletes anything. A destructive action button is red.

The **Pick Icon** dialog opens with the caret in its search box. `Enter` picks the icon
when exactly one matches, and `Esc` closes it.

**About**, **Update Available**, the Annotate assignee and Jira user profile windows, the
bulk-import project picker and the date-time picker close on `Esc`. About and the Annotate
windows open with the keyboard on **Close**. **Update Available** opens on **Later**: it can
appear while you type, so pressing `Enter` straight away installs nothing and skips nothing.
**Customize Toolbar** opens with the keyboard on its button list, and **Add command** or
**Add Lua** puts the caret in the new button's first field.

When the last open dialog, or the command palette, closes, the caret goes back to the text
field that had it before, with the text and the cursor as you left them. That includes a
dialog that opened by itself while you were typing. Running a command from the palette, or
clicking into another field while it is open, leaves the keyboard where it is.

While any modal dialog is open, the shortcuts in **Default bindings** below do nothing, except the
zoom shortcuts, **Full Screen** and the list and divider keys below (`Alt+Up`, `Alt+Down`, `Delete`,
`Shift+Alt+arrows`), which act only on the list or divider that has the keyboard. A shortcut that ran behind a
confirm could change or remove what the confirm is asking about.

## Dialog key rules

The dialogs and composers follow one set of keys, so a key means the same thing in every
window. Where a dialog's row in the tables above differs, the row describes what it does today.

| Where | Enter | Ctrl+Enter | Esc | Tab |
|---|---|---|---|---|
| Single-line field | Commit | Same as Enter | Revert to the previous value | Commit, move to the next control |
| Multiline composer (comment, worklog description, callstack) | New line | Submit | Leave the field; a second `Esc` closes, and typed text is kept or asked about | Next control, never a tab character |
| Chat composer (Assistant) | Send | New line | Stop the reply in flight, else leave the field | Next control |
| Picker or filter popup | Pick the highlighted row; nothing on an empty list | — | Close, change nothing | Accept the highlighted row |
| Form dialog | Field-level commit | Save | Cancel, asking first when something is typed | Next field |
| Confirm dialog | Activate the focused button | — | Cancel | Move between buttons |
| Search box | Go to the first result | — | Clear the search and leave the box | Next control |

- A button that has a key names it, dim, at its right edge.
- A key that belongs to a window (the tables above) acts only while that window has keyboard
  focus, so it cannot fire a dialog hidden behind another one.
- A global shortcut (the table below) fires from anywhere, unless a text field has the caret
  and the combo is one typing can produce. While a modal dialog is open they do nothing, except
  the zoom shortcuts, **Full Screen** and the list and divider keys (see **Confirm dialogs** above).

## Default bindings

| Action | Default | Command id |
|---|---|---|
| Primary Side Bar | `Ctrl+B` | `view.sidebar.primary` |
| Secondary Side Bar | `Ctrl+Alt+B` | `view.sidebar.secondary` |
| Panel (bottom) | `Ctrl+J` | `view.panel.bottom` |
| Assistant | `Ctrl+Shift+A` | `view.assistant` |
| Performance | `Ctrl+Shift+F` | `view.toggle.performance` |
| Plan docs | `Ctrl+Shift+D` | `view.toggle.plan_doc_viewer` |
| Bulk Import | `Ctrl+Shift+I` | `view.toggle.bulk_import` |
| Bulk Export | `Ctrl+Shift+X` | `view.toggle.bulk_export` |
| MCP Server | `Ctrl+Shift+K` | `view.toggle.mcp_server` |
| Scripts & Actions | `Ctrl+Shift+L` | `view.toggle.scripts` |
| Preferences | `Ctrl+,` | `view.toggle.preferences` |
| Dock-debug overlay | `Ctrl+Alt+D` | `app.dock_debug.toggle` |
| Full Screen | `F11` | `app.fullscreen.toggle` |
| Command Palette | `Ctrl+Shift+P` | `ui.command_palette` |
| Report a bug | `Ctrl+Shift+B` | `app.bug_report.open` |
| Zoom In | `Ctrl+=`, `Ctrl+Shift+=` (i.e. `Ctrl++`), `Ctrl+NumAdd`, `Ctrl+MouseWheelUp` | `ui.zoom.in` |
| Zoom Out | `Ctrl+-`, `Ctrl+NumSubtract`, `Ctrl+MouseWheelDown` | `ui.zoom.out` |
| Reset Zoom | `Ctrl+0`, `Ctrl+Num0` | `ui.zoom.reset` |
| Move the selected row up (focused list) | `Alt+Up` | `ui.list.move_up` |
| Move the selected row down (focused list) | `Alt+Down` | `ui.list.move_down` |
| Remove the selected row (focused list) | `Delete` | `ui.list.remove` |
| Move the focused splitter down / right | `Shift+Alt+Down`, `Shift+Alt+Right` | `ui.splitter.grow` |
| Move the focused splitter up / left | `Shift+Alt+Up`, `Shift+Alt+Left` | `ui.splitter.shrink` |
| Make the focused panel bigger | `Ctrl+Alt+PageUp` | `view.panel.grow` |
| Make the focused panel smaller | `Ctrl+Alt+PageDown` | `view.panel.shrink` |
| Quick-create issue | `Ctrl+Shift+T` | `issue.quick_create.open` |
| Search settings | `Ctrl+Shift+,` | `preferences.search.focus` |
| Next view | `Ctrl+PageDown` | `view.next` |
| Previous view | `Ctrl+PageUp` | `view.previous` |
| Search issues (focused grid pane) | `Ctrl+F`, `/` | `grid.search.focus` |

`/` on its own does nothing while you are typing in a field, so it never takes a
typed slash; `Ctrl+F` works from anywhere. In the search box, **Esc** clears the
search and leaves the box.

## Keyboard versions of mouse gestures

Every drag has a command you can bind, so none of these needs the mouse:

- **Lists and splitters** (`ui.list.*`, `ui.splitter.*`): they act only on the list or divider in the window that has the keyboard. With none focused, the key does nothing. They still run while a dialog is open, because the lists and dividers they serve sit inside dialogs. So far: the Customize Toolbar button list (select a row with `Space` or `Enter`, then `Alt+Up`, `Alt+Down` or `Delete`), and the divider in the long-text editor's Split view, the MCP panel and the Scripts window. The Up, Down and Delete buttons in the toolbar editor name the command in their tooltips, so a rebind never leaves a stale hint.
- **Panels** (`view.panel.grow`, `view.panel.shrink`): resize the bottom panel or a side bar by two text lines per press. They take a `panel` argument, `focused` (the default: the panel holding the focused window), `bottom`, `primary` or `secondary`, so a binding can name one. The size is clamped and saved like a splitter drag.
- **Date pickers**: the popup opens with the keyboard on the chosen day. `PageUp` / `PageDown` move a month (with `Shift`, a year), `Home` / `End` go to the first / last day, `T` goes to today, and the arrows move between days. `Enter` picks the focused day, and `Ctrl+Enter` applies in the full picker. The **Today**, **Tomorrow** and **In a week** buttons set the day at once.
- **Path remaps** (Preferences, Annotate): **Add path remap** puts the caret in the new rule's *from* field, and `Enter` in the last, filled *to* field adds the next rule.
- **Read-only text** (comments, descriptions): click the text or tab to it, then `Ctrl+A` selects all and `Ctrl+C` copies, with the mouse anywhere.

Not yet: moving and resizing grid columns, the sort-rule list and the filter popups are waiting on other changes to the files they live in, and are listed in `docs/plans/input-ux-w2-gesture-keys.md`.

## Upgrading from an older build

The standalone bug-report shortcut used to live in its own config field
(`bugreport_hotkey`). On first launch after upgrading, that value — including a
custom combo or a disabled state — is folded once into the keybinding registry as
`app.bug_report.open`, so your customisation carries over. After the one-time
fold the registry is authoritative; rebind it from the editor like any other
shortcut.

The zoom shortcuts gained their alternative combos the same way. On first launch
after upgrading, a zoom binding **still sitting on its shipped default** is widened
to the full set in the table above (including `Ctrl+MouseWheelUp` /
`Ctrl+MouseWheelDown`). A zoom binding you had already rebound — or deliberately
cleared — is left exactly as you set it; use **Reset all to defaults** if you want
the alias set instead.

The grid search shortcut (`grid.search.focus`) is added once to a config saved
before it existed. A binding you already gave that command is kept as it is.

The keyboard versions of the mouse gestures (`ui.list.move_up` / `ui.list.move_down` /
`ui.list.remove`, `ui.splitter.grow` / `ui.splitter.shrink`, `view.panel.grow` /
`view.panel.shrink`) are added the same way, once, to a config saved before they
existed (`migrated_gesture_keys_v1`). A command you had already bound keeps your
binding.

The settings-search and view-stepping shortcuts (`preferences.search.focus`, `view.next`, `view.previous`)
are added the same way, once, to a config saved before they existed; a binding you already gave one of
those commands is kept as it is.

Configs that already ran #2276's wheel migration (`migrated_multi_hotkey_zoom_v2`)
but still hold only a *partial* default set — the common case is just `Ctrl+=` /
`Ctrl+-`, which is not an exact match for V2's pre-wheel snapshot — also pick up
the missing aliases (including mouse-wheel) on the next launch. That retry only
runs on a row that holds just the original primary combo. A row you trimmed to
some of the defaults (for example, without the mouse-wheel combo), and a cleared
or customised row, are left alone.

If you run an older Smatchet build against the same config file, it reads the first
combo of each binding and ignores the rest, so shortcuts keep working there. Saving
from that older build drops the extra combos.
