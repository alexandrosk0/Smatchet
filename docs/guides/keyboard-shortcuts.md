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
| Zoom In | `Ctrl+=`, `Ctrl+Shift+=` (i.e. `Ctrl++`), `Ctrl+NumAdd` | `ui.zoom.in` |
| Zoom Out | `Ctrl+-`, `Ctrl+NumSubtract` | `ui.zoom.out` |
| Reset Zoom | `Ctrl+0`, `Ctrl+Num0` | `ui.zoom.reset` |

## Upgrading from an older build

The standalone bug-report shortcut used to live in its own config field
(`bugreport_hotkey`). On first launch after upgrading, that value — including a
custom combo or a disabled state — is folded once into the keybinding registry as
`app.bug_report.open`, so your customisation carries over. After the one-time
fold the registry is authoritative; rebind it from the editor like any other
shortcut.

The zoom shortcuts gained their alternative combos the same way. On first launch
after upgrading, a zoom binding **still sitting on its shipped default** is widened
to the full set in the table above. A zoom binding you had already rebound — or
deliberately cleared — is left exactly as you set it; use **Reset all to defaults**
if you want the alias set instead.

If you run an older Smatchet build against the same config file, it reads the first
combo of each binding and ignores the rest, so shortcuts keep working there. Saving
from that older build drops the extra combos.
