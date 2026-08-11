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
  the detail a human copying a command from a doc will not re-derive. Until it exists,
  the doc should at minimum say "re-run `test-markdown-links.sh` after archiving" —
  that one line would have caught this.

  Status: open
  Last-reviewed: 2026-08-11
