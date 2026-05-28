# Plan — Session-friction fixes (post-backlog-sweep session retro)

> **Slug**: `session-friction-fixes`
>
> **Origin**: End-of-session retrospective on `docs/design/tooling-process-backlog-sweep.md` (9 of 10 slices shipped 2026-05-27/28). User asked "any improvements?" — this plan addresses the 4 highest-leverage friction points surfaced across those 9 PRs.

## Context

The tooling-process-backlog-sweep session shipped 33 P0-P2 backlog items via 9 parallel slices. During execution, four recurring friction patterns emerged that would also hit any future multi-slice plan of similar shape. Each pattern is independently fixable, and each fix is small (≤ 3 h). After this lands, the next backlog sweep should run ~25% faster end-to-end with fewer wasted CR re-review cycles.

## Approach

Four independent slices, each addressing one friction. No dependencies between them — ship in any order. Total effort ~6-8 h. Three of four are pure tooling/automation (no design judgement); the fourth (pre-implementation triage) is a doc-rule edit.

## Slices

### Slice 1 — Shell-script self-review (P1 process)
**Backlog**: 2026-05-28 process · P1 · "Implementer-side self-review didn't catch real shell-script bugs"
**Est**: 3 h

CR caught genuine bugs in 4 of 9 shell-script slices this session — patterns that would have been visible to a structured pre-push read-through but weren't to mine. Ship a pre-push linter + checklist that catches them mechanically.

**Files**:
- `scripts/dev/lint-shell-self-review.sh` (new) — wraps `shellcheck` + project-specific heuristics
- `docs/harness/claude-code/hooks/lint-shell.sh` (new) — pre-push hook wrapper
- `docs/harness/claude-code/settings.json` — wire the hook
- `docs/agent-rules/shell-script-self-review.md` (new) — the checklist

**Assertions in `lint-shell-self-review.sh`** (each one CR caught at least once this session):
1. **Dependency preflight check** — every external command (`curl`, `gh`, `cmake`, `python`, `7z`, …) used in the script body MUST appear in a `command -v X >/dev/null 2>&1 || { ... exit 2; }` block at the top. Catches the `git-janitor.sh` python-missing bug from PR #482.
2. **`shellcheck` clean** — run shellcheck, fail on warnings (SC2086 unquoted expansion, SC2046 word-split, SC2128 array-as-string). Catches the `p4-git-sync-check.sh` unquoted `$git_not_p4` bug from PR #478.
3. **`curl -f` everywhere** — any `curl` invocation without `-f` (or `--fail`) fails the lint. Catches the Font Awesome 404-HTML-as-font bug from PR #477.
4. **sha256 verify on network downloads** — any `curl` that writes to a file path under `assets/`, `build/`, or the repo root must be followed within 10 lines by `sha256sum -c` or `--checksum`. Catches the unpinned supply-chain risk on PR #477.
5. **`--key=value` and `--key value` parity** — when the script has a `--<flag>)` case, it must also have a `--<flag>=*)` case (or vice versa). Catches the `--threshold=` vs `--threshold` asymmetric-validation bug from PR #477.

**Wiring**: `docs/harness/claude-code/settings.json` hook registers `lint-shell.sh` as a PreToolUse Bash hook for `git push`; refuses the push if `scripts/dev/*.sh` is in the diff AND the lint script fails. Bypass via `SMATCHET_SKIP_SHELL_LINT=1` for emergencies (logged).

### Slice 2 — Backlog-archive union merge (P2 tooling)
**Backlog**: 2026-05-28 tooling · P2 · "Backlog files conflict on every parallel-slice merge"
**Est**: 30 min

8 of 9 slices this session hit merge conflicts on `applied.md` / `process.md` / `tooling.md` because git can't auto-merge adjacent deletions even when the entries are independent. Ship a `.gitattributes` union merge driver.

**Files**:
- `.gitattributes` — 3 new lines
- `docs/agent-rules/process-rules.md` — document the union merge + post-merge sort step
- `scripts/dev/sort-applied-md.sh` (new, optional) — post-union sort by date descending

**Diff**:
```
+docs/backlog/agent-self-improvement/applied.md merge=union
+docs/backlog/agent-self-improvement/process.md merge=union
+docs/backlog/agent-self-improvement/tooling.md merge=union
```

**Trade-off**: union merge concatenates both sides verbatim — date order may interleave on the merge commit. The optional `sort-applied-md.sh` re-sorts entries by their first-line date prefix; agents can run it pre-push or it can ship as a pre-commit hook. Acceptable: a temporarily out-of-order applied.md is far cheaper than the manual conflict resolution it replaces.

**Defer**: the more correct fix (one-entry-per-file under `applied.md.d/<date>-<slug>.md` + a build step) is 3 h vs 30 min. Skip until union merge proves insufficient — likely needed when single-line entries replace multi-line ones, or when applied.md crosses ~500 entries.

### Slice 3 — Pre-implementation triage rule (P2 process)
**Backlog**: 2026-05-28 process · P2 · "Pre-implementation triage caught 5 already-resolved backlog items"
**Est**: 15 min (pure doc)

The backlog-sweep plan's § Approach pre-flighted 5 items as "already done" via grep + git history; that 5 min of investigation saved ~3 h of redundant code. Encode this as a rule so every future multi-slice plan inherits the win.

**Files**:
- `docs/agent-rules/process-rules.md` — add § Pre-implementation triage sub-rule under the Plan-doc family bullet

**Wording**:
> **Pre-implementation triage** — for any plan item that says "fix existing tooling" or "extend X", the slice agent's first action is to grep / `Read` the cited code AND check git history (`git log --oneline -- <file>` + `docs/backlog/agent-self-improvement/applied.md` for prior fixes). If the fix has already shipped, archive the backlog entry and skip the slice — don't reimplement. Plan-doc § Approach should pre-flight this triage for items known to overlap recent work; § Pre-implementation triage findings get a per-item bullet in § Approach so the implementer doesn't have to redo the search.

### Slice 4 — bash-side MSVC env wrapper (P2 tooling)
**Backlog**: 2026-05-28 tooling · P2 · "No `vcvars64.bat` wrapper for bash sessions"
**Est**: 1 h

Slice 10's C++ build couldn't run from bash (`Cannot open stdio.h`) because the bash session lacked `vcvars64`'s env. PowerShell has `build_and_run.ps1`; bash has nothing. Without a local build, C++ slices ship blind.

**Files**:
- `scripts/dev/with-msvc-env.sh` (new) — sub-shell wrapper that sources vcvars64
- `BUILD.md` — document the wrapper next to the PowerShell entry
- `agents/build-doctor.md` — cross-link

**Recipe** (the script's body):
```bash
#!/usr/bin/env bash
# Source vcvars64 env into the current shell, then exec the command.
set -euo pipefail
VCVARS="$(ls /c/Program*Files/Microsoft*Visual*Studio/*/Community/VC/Auxiliary/Build/vcvars64.bat 2>/dev/null | head -n 1)"
[ -z "$VCVARS" ] && { echo "with-msvc-env: vcvars64.bat not found" >&2; exit 2; }
# Run vcvars64 in cmd.exe, dump env, export every VS-relevant var into this bash.
eval "$(cmd.exe /c "\"$VCVARS\" >nul && set" 2>/dev/null | awk -F= '/^(INCLUDE|LIB|LIBPATH|PATH|VCINSTALLDIR|WindowsSdkDir)=/ {gsub(/\\/,"\\\\",$2); print "export "$1"=\""$2"\""}')"
exec "$@"
```

Usage:
```bash
bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
```

## Ship order

All four slices are independent — ship in any order or in parallel. Suggested:

```text
Slice 2 (30 min)  ──→  Slice 3 (15 min)  ──→  Slice 1 (3 h)  ──→  Slice 4 (1 h)
```

Front-load the docs/config slices (slices 2/3) since they're cheap and remove friction from slice 1's review cycle.

## Summary

| Slice | Title | Backlog item | Est. |
|---|---|---|---|
| 1 | Shell-script self-review | P1 process · 2026-05-28 | 3 h |
| 2 | Backlog-archive union merge | P2 tooling · 2026-05-28 | 30 min |
| 3 | Pre-implementation triage rule | P2 process · 2026-05-28 | 15 min |
| 4 | bash-side MSVC env wrapper | P2 tooling · 2026-05-28 | 1 h |
| **Total** | | **4 items** | **~5 h** |

## UX Pillar callouts

- **Pillar 1 (perf)**: N/A — no UI changes.
- **Pillar 2 (UI-thread)**: N/A.
- **Pillar 3 (never crash)**: Slice 1 prevents a class of shell-script bugs from reaching CI. Marginal positive.
- **Pillar 4 (accessibility)**: N/A.

## Perf-review-system gates

N/A — diff is target-agnostic (no `Source_Core/` C++ touches).

## Risks / non-goals

- **Risk** (slice 2): union merge may interleave dates if the post-merge sort isn't run. Mitigation: optional sort script + a one-line note in agents/git-janitor.md. Cost of disorder is cosmetic.
- **Risk** (slice 1): pre-push hook adds latency to every shell-script push. Mitigation: only fires when `scripts/dev/*.sh` is in the diff; SMATCHET_SKIP_SHELL_LINT bypass for emergencies.
- **Non-goal**: addressing the remaining backlog-sweep slices (3 — merge-watcher, 7 — bucket-E tests). Those are tracked in their original plan and remain open.

## Verification

- **Per-slice**: ships via the autonomous ship-loop. Pure-docs slices (2/3) skip the dual-target build.
- **Slice 1 self-test**: the lint script runs against itself (it IS a shell script) — must exit 0.
- **Slice 2 verify**: deliberately create a merge conflict on applied.md (concurrent branch + parallel deletion) and confirm git auto-merges via the union driver.
- **Slice 4 smoke**: `bash scripts/dev/with-msvc-env.sh cmake --version` should print the VS-bundled CMake; `bash scripts/dev/with-msvc-env.sh cl /?` should print the MSVC banner.
- **Build gate**: N/A for slices 2/3; slices 1/4 are docs/scripts only.
- **Manual residue**: none.

## Out of scope

- One-entry-per-file backlog refactor (slice 2 stretch goal — deferred).
- Functional-parity test for skill-vs-agent forms (from the original sweep plan's slice 9 — already shipped as shape check).
- Bucket-E test coverage batch (the original plan's slice 7 — separate work).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
