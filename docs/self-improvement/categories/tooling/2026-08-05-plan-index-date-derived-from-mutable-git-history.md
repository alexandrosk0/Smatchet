- 2026-08-05 · claude-code · [tooling] · P1 — `test-plan-index.sh` derives shipped-plan index dates from `git log --follow`, which squash-merge rewrites — so a plan archived and merged across a midnight boundary reddens `develop` the instant it lands, with no pre-merge state that could have passed

  Observed on PR #1937 (Help > About dialog). Merged `2026-08-05T11:33:25Z` as
  `fce0951c` with the required `Doc anchors + agent contract` terminal-green. The
  develop tip went RED on that same check immediately after:
  `test-plan-index: DRIFT — shipped-plan index out of sync (182 plans in archive)`.
  Full RCA in [`postmortems.md`](../../postmortems.md) (2026-08-05 entry).

  Mechanism. `agents/scripts/core/test-plan-index.sh:122-143` resolves each row's
  date with `git log --follow --format=%ad --date=short -- <path>` — the file's
  *first-commit* date. `--follow` is what normally makes this stable across the
  `plans/active/` → `plans/shipped/` move. A squash-merge collapses the branch into
  one commit and the per-file pre-merge history is unreachable from `develop`, so
  `--follow` finds exactly one commit and returns the **squash date**:

      $ git log --follow --format='%ad %h %s' --date=short \
          -- docs/plans/shipped/about-dialog-help-menu.md
      2026-08-05 fce0951c feat(about): About Smatchet dialog under Help, ... (#1937)

  Three conditions, all common: the PR archives a plan *and* commits its index row;
  the repo squash-merges; branch work and merge fall on different calendar days.
  Every plan-shipping PR that spans a midnight hits this.

  Why it is P1 rather than P2: the check is **required**, and a red required check
  on the develop tip is inherited by every open PR's own head under block-on-any-red.
  One late-evening merge blocks the whole queue until someone notices, and nothing
  announces it — `postmortem-owed.sh` keys on merge-instant signals (non-SUCCESS
  checks, override labels, `Revert`, overdue deviations) and this class emits none,
  so it reports "no gate escapes owed" for the PR that caused it.

  Proposed fix — **stop deriving the value from mutable git metadata.** Have
  `--fix` write the resolved date into the plan file as an explicit
  `<!-- plan-date: YYYY-MM-DD -->` marker when a plan is archived, and have the
  generator prefer that marker, falling back to `git log --follow` only for legacy
  plans without one. Content survives squash, shallow clone and staged rename
  identically. This is not a fourth special case — it **retires the two already in
  the script**, both of which exist to paper over the same history lookup: the
  shallow-clone guard (`:45`, `:105`) and the staged-rename sibling-tier fallback
  (`:135-143`, whose comment already cites the #1061 / #1092 archive date-drift
  "twice"). Squash-merge is the third way the same lookup moves under the generator;
  the recurring shape is the defect.

  Concrete next action: add the marker read/write to `test-plan-index.sh`, plus two
  `--selftest` cases — (1) a `shipped/` plan whose only commit is the current HEAD
  still resolves a stable date; (2) a marker date disagreeing with its index row
  FAILs. Migrate existing rows by running `--fix` once to stamp markers from the
  currently-committed dates, so no archived plan's date changes on adoption.

  Paired with: the develop-tip required-green assertion proposed in the
  `2026-07-10 · PR #1698` postmortem and never landed. This is its second instance —
  a gate that can only go red *after* the merge needs a detector that looks after
  the merge.

  Status: open
  Last-reviewed: 2026-08-05
