- 2026-08-11 · claude-code · [tooling] · P2 — the documented archival command re-parents a per-entry file one directory UP, silently breaking every relative link in it; the docs gate catches it only after the fact, and only if someone runs it

  Details: [`AGENT_SELF_IMPROVEMENT.md`](../../AGENT_SELF_IMPROVEMENT.md) § workflow
  step 4 prescribes archiving a per-entry file with

  ```
  cat docs/self-improvement/categories/<cat>/<file>.md >> docs/self-improvement/categories/applied.md
  git rm docs/self-improvement/categories/<cat>/<file>.md
  ```

  The entry was authored at `categories/<cat>/` depth; `applied.md` lives one level up
  at `categories/`. Every relative link in the body is therefore off by one directory
  the instant it is appended. Hit live archiving
  `2026-08-06-gate-tooling-run-from-stale-session-branch`: **7 dangling links** in one
  entry — `../../../../agents/...` (correct from `categories/process/`) resolved to
  `../agents/...` from `categories/`, `../../../agent-rules/...` to `agent-rules/...`,
  and a sibling-category link `../tooling/<slug>.md` to `docs/self-improvement/tooling/`.

  Two properties make this worse than a one-off:

  1. **It fails silently at the moment of the mistake.** `cat` cannot fail here. The
     only signal is `test-markdown-links.sh` reporting on `applied.md` later — and a
     PR that archives an entry may not otherwise touch a file that trips the docs gate
     locally, so the author's first notice can be CI.
  2. **It degrades the archive specifically.** `applied.md` is the durable record read
     months later; the whole value of an archived entry is that its cited paths still
     resolve. This mechanically guarantees the opposite for exactly the entries that
     carried the most cross-references.

  **The same command breaks links in the mirror direction too, and that half bites
  harder.** The `git rm` deletes a path other documents cite. Caught live in CI on this
  very branch (PR #1996, `Agentic self-tests (bats)`): archiving
  `2026-08-06-gate-tooling-run-from-stale-session-branch` left **2 inbound dangling
  links** — from [`postmortems.md`](../../postmortems.md) and from the sibling entry
  [`2026-08-06-merge-gates-cr-path-filter-skip-false-block.md`](2026-08-06-merge-gates-cr-path-filter-skip-false-block.md),
  both of which had correctly linked a file that existed when they were written.

  Inbound is the worse half for two reasons. The outbound breakage is confined to the
  archived entry and is repairable by re-depthing, mechanically, inside one file. The
  inbound breakage is scattered across files the archiver never opened, has no
  mechanical fix — `applied.md` is a 3000-line append-only ledger with no per-entry
  anchors, so there is no equivalent link to rewrite *to*, only prose to restate — and
  it is invisible to any check scoped to the diff, because the referring files are
  unmodified. Only a repo-wide `--all` sweep sees it. That is exactly why this surfaced
  as a red required check rather than as a local pre-ship failure.

  The 550+ legacy entries already in `applied.md` were largely moved from monolith
  `categories/<cat>.md` files, which sit at the SAME depth as `applied.md` — so the
  bug is new-ish, arriving with the one-file-per-entry convention, and will recur on
  every per-entry archival from here.

  Concrete next action: replace the raw `cat` in the § workflow step with a small
  `archive-backlog-entry.sh` that appends AND re-depths — mechanically, `../../../../`
  → `../../../` and `../../../` → `../../` for links leaving `docs/`, and
  `../<sibling-cat>/` → `<sibling-cat>/` — then `git rm`s the source and re-runs
  `test-markdown-links.sh` as a self-check. A script is the right shape rather than a
  documented sed: the transform depends on the source entry's depth, which is exactly
  the detail a human copying a command from a doc will not re-derive. The same script
  must also handle the inbound half **before** the `git rm`: grep the repo for the
  entry's slug and rewrite each referring link to prose naming the entry plus a link to
  `applied.md`, refusing to delete while any inbound reference remains. Until it exists,
  the doc should at minimum say "re-run `test-markdown-links.sh --all` after archiving" —
  that one line would have caught both halves, and the `--all` is load-bearing: default
  mode is diff-scoped and sees neither the re-parented body nor the orphaned referrers.

  Status: open
  Last-reviewed: 2026-08-11
