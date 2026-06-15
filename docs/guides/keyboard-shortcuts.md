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
- Click a row's shortcut to **capture** a new combo: press the keys you want
  (e.g. `Ctrl+Shift+G`), or press **Esc** to cancel. The capture widget shows
  the live combo as you hold modifiers.
- **On** toggles a binding off without forgetting its combo — an off binding
  never fires but keeps its assignment for when you turn it back on.
- **Clear** removes the combo, leaving the command unbound.
- **Reset all to defaults** restores the shipped default set (table below).

Changes save automatically (no Apply button) via the same off-thread config
write the rest of Preferences uses.

## Quick-bind from where you use a command

You don't have to open Preferences. **Right-click → Set shortcut…** is available
on:

- **Toolbar buttons** that run a command.
- **Command-palette rows** (`Ctrl+Shift+P`, then right-click any entry).

The quick-bind popup captures one combo for that command, warns if the combo is
already taken, and offers **Set** / **Clear** / **Cancel**.

## Conflicts are warned, not blocked

A combo bound to two commands is flagged inline — the editor shows
*"conflicts with <other action>"* and the quick-bind popup shows *"Already bound
to: <other action>"*. The bind is still allowed: Smatchet **warns, it does not
block**. The last-pressed-wins behaviour is intentional so you can deliberately
move a combo from one command to another in two steps (bind the new one, the old
one surfaces as the conflict, then clear it).

One combo per command — multi-key **chords** (press-then-press sequences) are not
user-rebindable in this release.

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

## Upgrading from an older build

The standalone bug-report shortcut used to live in its own config field
(`bugreport_hotkey`). On first launch after upgrading, that value — including a
custom combo or a disabled state — is folded once into the keybinding registry as
`app.bug_report.open`, so your customisation carries over. After the one-time
fold the registry is authoritative; rebind it from the editor like any other
shortcut.
