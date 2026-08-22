# Smatchet bundled fonts

## fa-solid-900.ttf — committed

The Font Awesome 6 Free **Solid** TTF lives at:

    assets/fonts/fa-solid-900.ttf  (~426 KB)

It is committed to the repo (since 2026-08) so fresh clones and linked
worktrees render icons out of the box. Sourced from the official Font Awesome
6 Free release at
[github.com/FortAwesome/Font-Awesome/tree/6.x/webfonts](https://github.com/FortAwesome/Font-Awesome/tree/6.x/webfonts) — file `fa-solid-900.ttf`.

### License

SIL Open Font License 1.1 — redistribution is permitted with the license
notice. See [`THIRD_PARTY_LICENSES.md`](../../THIRD_PARTY_LICENSES.md) for the
full notice.

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

Now that the TTF is committed, every worktree checks it out directly.
`smatchet_resolve_font_asset`
([`cmake/SmatchetFontAssets.cmake`](../../cmake/SmatchetFontAssets.cmake))
still falls back to the main worktree's copy, located via `git rev-parse
--git-common-dir`, for checkouts predating the committed font. Regression
coverage: `tests/bats/font_asset_resolve.bats`.

An already-configured build directory keeps its old decision — re-configure
(`cmake --preset <name>`) once for the fallback to take effect.
