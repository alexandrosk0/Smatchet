- 2026-11-17 · dropdown-recents-w1 · [debt] · P3 — Shared recents store: sync disk I/O on save, three popup families still off the shared dropdown, perf compare not run

Details:
Gaps left after PR #2432 (shared "last 5 picks" store + `FilterDropdown`):

- `CommandRegistry::SaveRecents` does synchronous file I/O. If it ever runs on the UI thread this breaks Pillar 2
  (no UI-thread I/O); move it off-thread or batch it.
- The settings, preferences and annotate Jira combos are not converted to the shared dropdown.
- Palette recents and `RecentViewLru` are separate stores; fold them into `smatchet::recents`.
- Perf comparison (`scripts/dev/perf-compare.py`) was not run locally for this change; the PR relied on CI Perf PR-fast.
- `tools/repo-health/facts.json` and the historical-review ledger are stale (separate SessionStart nudges, listed here only so they are not lost).

Concrete next action:
Move `SaveRecents` off the UI thread first (Pillar 2), then convert the three Jira combos, then fold the other recents stores in.

Status: open
Last-reviewed: 2026-11-17
