# Bucket-C goldens silently depend on whether the gitignored icon font resolves

- **Category**: test
- **Priority**: P2
- **Date**: 2026-08-06
- **Source**: PR #1952 (bucket-C determinism) — cost a full misdiagnosis round

## What

`assets/fonts/fa-solid-900.ttf` is **gitignored**. Toolbars render icon glyphs
when it resolves and a text fallback ("Refresh View", "Filter", …) when it does
not — a whole-row pixel difference, far past the L_inf 4 tolerance.

Two things make that environment-dependent rather than constant:

- CI has no font (nothing fetches it), so every checked-in golden is a
  *text-fallback* capture.
- PR #1948's `smatchet_resolve_font_asset()` falls back to the **main worktree**,
  so a linked worktree under `.claude/worktrees/<id>/` finds the dev's local copy
  and captures *icon* glyphs.
- The build's link step copies the font next to the exe whenever it resolves, so
  "I moved it aside" does not survive the next `cmake --build`.

Net effect: `bash scripts/dev/test-screenshot-diff.sh` fails on a dev machine
that has the font, passes in CI, with a diff that looks like a real UI
regression. During #1952 this masked a genuine, unrelated capture bug for a
whole debugging round (moving the TTF aside took the suite 9/6 → 11/4).

## Fix options

1. **Pin the font into the capture path** — have the screenshot driver force the
   text fallback (an env knob the font resolver honours, e.g.
   `SMATCHET_DISABLE_ICON_FONT=1`, exported by `test-screenshot-diff.sh`). Makes
   local and CI captures identical by construction; preferred.
2. **Vendor the font** — un-ignore it and check it in, so every environment
   including CI renders icons. Larger blast radius (licence + repo size + every
   existing golden regenerates), but removes the whole class.
3. **Assert, don't guess** — at minimum, make the driver detect a resolved font
   and print a loud banner naming it as a likely diff source. Cheap; strictly a
   diagnostic, not a fix.

Option 1 plus the option-3 banner is the recommended pair.
