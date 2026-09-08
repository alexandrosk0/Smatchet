- 2026-09-07 · code-review · [tooling] · P2 — `SmatchetLocalization::T(key, fallback)` returns the call-site fallback for en-US, not the table's English column — editing one without the other is a silent no-op

  Details: `TranslateEntryLocked` (`Source/Core/src/SmatchetLocalization.cpp`) returns
  `fallback ? fallback : entry.English` for the active en-US language — the `kEntries` table's English
  column is used only as an *overrides lookup key* (`overrides.find(entry.English)`), never rendered
  directly. A diff that edits only `kEntries`'s English string (e.g. renaming a label) changes nothing
  user-visible in English; the call-site `T("key", "...")` literal is what actually renders. This bit twice
  in one session on the same PR (#2198): the author renamed "Story group" -> "Parent group" by editing only
  the table row, and a review pass initially read the table as the source of truth too before tracing the
  function. The mismatch also silently broke the feature's only bucket-E UI-test coverage — the test's item
  ref matched the (unwritten) new label, so `ClickSortByCheckbox` never resolved it.
  Found during adversarial review of docs/plans/shipped/parent-issue-hierarchy.md.

  Concrete next action: document the fallback-wins-for-en-US contract in `docs/agent-rules/cpp-rules.md`
  § Conventions (near any existing localization guidance), and add a cheap delta-gated check: for every
  changed `kEntries` row `{"<key>", "<english>", ...}` in `SmatchetLocalization.cpp`, grep the repo for
  `T("<key>"` call sites and flag if none of their literal fallback strings match the new `<english>` value.
  Enumerator: the `kEntries` array in `Source/Core/src/SmatchetLocalization.cpp`; rule id sketch
  `localization-english-fallback-drift`.
  Status: open
  Last-reviewed: 2026-09-07
