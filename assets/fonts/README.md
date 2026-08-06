# Smatchet bundled fonts

## fa-solid-900.ttf — **drop-in required**

The Font Awesome 6 Free **Solid** TTF must be dropped at:

    assets/fonts/fa-solid-900.ttf  (~430 KB)

### Where to get it

Download from the official Font Awesome 6 Free release at
[github.com/FortAwesome/Font-Awesome/tree/6.x/webfonts](https://github.com/FortAwesome/Font-Awesome/tree/6.x/webfonts) — file `fa-solid-900.ttf`.

### License

SIL Open Font License 1.1. The TTF is **not** redistributed in this repo (CI
fetches it on release-tag builds; dev workstations drop it manually). See
[`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md) for the full notice.

### What happens at runtime when the TTF is missing

`SmatchetApplyImGuiFont` logs `LOG_WARN` and sets `g_FaIconsLoaded = false`. The
AI chat hover action row + pin strip fall back to short text labels (`Copy`,
`Pin`, `×`) — same hit area, no icon glyphs. The build does NOT fail.

### CMake POST_BUILD copy

When the TTF resolves at configure time, `CMakeLists.txt` adds an explicit
`copy_if_different` step to the `SmatchetStandalone` POST_BUILD chain so the
shipped exe has the font next to it. Missing-at-configure: the POST_BUILD step
is skipped and a `STATUS` message tells the user to drop the file then
re-configure.

### Linked git worktrees

`assets/fonts/*.ttf` is gitignored, so a worktree created with `git worktree
add` (or `nsc <slug>`) starts with an **empty** `assets/fonts/` even when the
main worktree has the TTF. `smatchet_resolve_font_asset`
([`cmake/SmatchetFontAssets.cmake`](../../cmake/SmatchetFontAssets.cmake))
therefore falls back to the main worktree's copy, located via `git rev-parse
--git-common-dir`, so no per-worktree drop-in (and no duplicated 430 KB binary)
is needed. Regression coverage: `tests/bats/font_asset_resolve.bats`.

An already-configured build directory keeps its old decision — re-configure
(`cmake --preset <name>`) once for the fallback to take effect.
