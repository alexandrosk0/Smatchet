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
| Log work | `Esc` | Cancel. With the caret in a field and text typed, the first `Esc` only leaves the field |
| Comments | `Ctrl+Enter` in the comment box | Post the comment |
| Comments | `Esc` | Leave the comment box, keeping the text; then close (asks before discarding a comment). In an empty box, close at once |
| Long-text editor | `Ctrl+Enter` | Save |
| Long-text editor | `Esc` | Cancel (asks before discarding unsaved changes) |
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

A submit button names its key at its right edge (**Post Comment** `Ctrl+Enter`, **Save**
`Ctrl+Enter`), the way a menu item names its shortcut, and a composer's placeholder sits
inside the empty box.

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

While any modal dialog is open, the shortcuts in **Default bindings** below do nothing, except the
zoom shortcuts and **Full Screen**. A shortcut that ran behind a confirm could change
or remove what the confirm is asking about.

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
  the zoom shortcuts and **Full Screen** (see **Confirm dialogs** above).

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
| Quick-create issue | `Ctrl+Shift+T` | `issue.quick_create.open` |
| Search settings | `Ctrl+Shift+,` | `preferences.search.focus` |
| Next view | `Ctrl+PageDown` | `view.next` |
| Previous view | `Ctrl+PageUp` | `view.previous` |
| Search issues (focused grid pane) | `Ctrl+F`, `/` | `grid.search.focus` |

`/` on its own does nothing while you are typing in a field, so it never takes a
typed slash; `Ctrl+F` works from anywhere. In the search box, **Esc** clears the
search and leaves the box.

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

The settings search, next view and previous view shortcuts
(`preferences.search.focus`, `view.next`, `view.previous`) are added the same way,
once, to a config saved before they existed. A binding you already gave any of
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
