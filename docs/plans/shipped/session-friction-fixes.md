# Plan — Session-friction fixes (post-backlog-sweep session retro)

> **Slug**: `session-friction-fixes`
>
> **Origin**: End-of-session retrospective on `docs/plans/tooling-process-backlog-sweep.md` (9 of 10 slices shipped 2026-05-27/28). User asked "any improvements?" — this plan addresses the 4 highest-leverage friction points surfaced across those 9 PRs.

## Context

The tooling-process-backlog-sweep session shipped 33 P0-P2 backlog items via 9 parallel slices. During execution, four recurring friction patterns emerged that would also hit any future multi-slice plan of similar shape. Each pattern is independently fixable, and each fix is small (≤ 3 h). After this lands, the next backlog sweep should run ~25% faster end-to-end with fewer wasted CR re-review cycles.

## Approach

Four independent slices, each addressing one friction. No dependencies between them — ship in any order. Total effort ~4.75 h (matches the per-slice rollup in § Summary). Three of four are pure tooling/automation (no design judgement); the fourth (pre-implementation triage) is a doc-rule edit.

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
1. **Dependency preflight check** — for every external command in a **closed allowlist** (`curl`, `gh`, `cmake`, `python`, `python3`, `jq`, `7z`, `cppcheck`, `clang-format`, `clang-tidy`, `bats`, `p4`) that appears in the script body, assert a `command -v X >/dev/null 2>&1 || { ... exit 2; }` (or equivalent) preflight block exists. Allowlist-based, not script-body scan, so bash builtins (`echo`, `printf`, `mapfile`, `comm`, `set`, `local`, `eval`, `exec`) and ubiquitous POSIX tools (`awk`, `sed`, `grep`, `head`, `cut`, `tr`) aren't false-flagged. Catches the `git-janitor.sh` python-missing bug from PR #482.
2. **`shellcheck` clean** — run shellcheck, fail on warnings (SC2086 unquoted expansion, SC2046 word-split, SC2128 array-as-string). Catches the `p4-git-sync-check.sh` unquoted `$git_not_p4` bug from PR #478.
3. **`curl -f` everywhere** — any `curl` invocation without `-f` (or `--fail`) fails the lint. Catches the Font Awesome 404-HTML-as-font bug from PR #477.
4. **sha256 verify on network downloads** (diff-scoped only) — any **newly-added** `curl` that writes to a file path under `assets/`, `build/`, or the repo root must be followed within 10 lines by `sha256sum -c` or `--checksum`. Scoped to `git diff` against develop to grandfather existing scripts (e.g. the Mesa 7z download in `build-and-test.yml` already lacks a checksum and isn't blocking this work). Catches the unpinned supply-chain risk on PR #477.
5. **`--key=value` and `--key value` parity** — when the script has a `--<flag>)` case, it must also have a `--<flag>=*)` case (or vice versa). Catches the `--threshold=` vs `--threshold` asymmetric-validation bug from PR #477.

**Wiring**: Two delivery options — pick at implementation time:
- **Option A (recommended)**: extend `scripts/dev/test-all.sh` to discover and run `lint-shell-self-review.sh` like every other `test-*.sh`. Auto-runs in the existing pre-push test gate. Simplest, no new hook surface.
- **Option B**: add a Claude Code PostToolUse Bash hook in `.claude/settings.json` matching `Bash(git push:*)`. Needs verification that the hook surface fires reliably on bare `git push` (vs `git push --force`, `git push -u`, etc.) — surface untested for this use case.

Bypass for either: `SMATCHET_SKIP_SHELL_LINT=1` env var (logged when used).

### Slice 2 — Backlog-archive union merge (P2 tooling)
**Backlog**: 2026-05-28 tooling · P2 · "Backlog files conflict on every parallel-slice merge"
**Est**: 30 min

8 of 9 slices this session hit merge conflicts on `applied.md` / `process.md` / `tooling.md` because git can't auto-merge adjacent deletions even when the entries are independent. Ship a `.gitattributes` union merge driver.

**Files**:
- `.gitattributes` — 3 new lines
- `docs/agent-rules/process-rules.md` — document the union merge + post-merge sort step
- `scripts/dev/sort-applied-md.sh` (new, optional) — post-union sort by date descending

**Diff** (corrected after plan review — apply ONLY to `applied.md`):

```text
+docs/self-improvement/categories/applied.md merge=union
```

**Why scoped to applied.md only**: `merge=union` concatenates both sides of a conflict. That's the right behaviour ONLY when both sides are prepending new entries (the applied.md parallel-archive case). For `process.md` / `tooling.md`, parallel branches DELETE different entries; union would wrongly preserve both deleted entries. For `AGENT_SELF_IMPROVEMENT.md`'s count line, parallel branches MODIFY the same line with different counts; union would garble it. Initial plan draft applied union to all three files — that was wrong and would have introduced bugs.

**Trade-off**: union merge concatenates both sides verbatim — date order may interleave on the merge commit. The optional `sort-applied-md.sh` re-sorts entries by their first-line date prefix; agents can run it pre-push or it can ship as a pre-commit hook. Acceptable: a temporarily out-of-order applied.md is far cheaper than the manual conflict resolution it replaces.

**Process.md / tooling.md / index file still conflict** — ~30% of session conflict time was on those. Defer to one-entry-per-file (below) if pain recurs. Empirically, ~70% of this session's conflict-resolution time was on applied.md, so this is still a real win.

**Defer**: the more correct fix (one-entry-per-file under `applied.md.d/<date>-<slug>.md` + a build step) is 3 h vs 30 min. Skip until union merge proves insufficient — likely needed when single-line entries replace multi-line ones, or when applied.md crosses ~500 entries.

### Slice 3 — Pre-implementation triage rule (P2 process)
**Backlog**: 2026-05-28 process · P2 · "Pre-implementation triage caught 5 already-resolved backlog items"
**Est**: 15 min (pure doc)

The backlog-sweep plan's § Approach pre-flighted 5 items as "already done" via grep + git history; that 5 min of investigation saved ~3 h of redundant code. Encode this as a rule so every future multi-slice plan inherits the win.

**Files**:
- `docs/agent-rules/process-rules.md` — add § Pre-implementation triage sub-rule under the Plan-doc family bullet

**Wording**:
> **Pre-implementation triage** — for any plan item that says "fix existing tooling" or "extend X", the slice agent's first action is to grep / `Read` the cited code AND check git history (`git log --oneline -- <file>` + `docs/self-improvement/categories/applied.md` for prior fixes). If the fix has already shipped, archive the backlog entry and skip the slice — don't reimplement. Plan-doc § Approach should pre-flight this triage for items known to overlap recent work; § Pre-implementation triage findings get a per-item bullet in § Approach so the implementer doesn't have to redo the search.

### Slice 4 — bash-side MSVC env wrapper (P2 tooling)
**Backlog**: 2026-05-28 tooling · P2 · "No `vcvars64.bat` wrapper for bash sessions"
**Est**: 1 h

Slice 10's C++ build couldn't run from bash (`Cannot open stdio.h`) because the bash session lacked `vcvars64`'s env. PowerShell has `build_and_run.ps1`; bash has nothing. Without a local build, C++ slices ship blind.

**Files**:
- `scripts/dev/with-msvc-env.sh` (new) — sub-shell wrapper that sources vcvars64
- `BUILD.md` — document the wrapper next to the PowerShell entry
- `agents/core/build-doctor.md` — cross-link

**Recipe** (the script's body, corrected after plan review — uses `vswhere.exe` instead of a path glob):

```bash
#!/usr/bin/env bash
# Source vcvars64 env into the current shell, then exec the command.
set -euo pipefail

# vswhere.exe ships with every VS 2017+ install at a stable path. It discovers
# every installed edition (Community / Professional / Enterprise / BuildTools)
# regardless of where they're installed — more robust than path globs.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[ -x "$VSWHERE" ] || { echo "with-msvc-env: vswhere.exe not found at $VSWHERE" >&2; exit 2; }

VS_INSTALL="$("$VSWHERE" -latest -property installationPath 2>/dev/null | tr -d '\r')"
[ -n "$VS_INSTALL" ] || { echo "with-msvc-env: no Visual Studio install detected" >&2; exit 2; }

VCVARS="$VS_INSTALL\\VC\\Auxiliary\\Build\\vcvars64.bat"
[ -f "${VCVARS//\\//}" ] || { echo "with-msvc-env: vcvars64.bat not found at $VCVARS" >&2; exit 2; }

# Run vcvars64 in cmd.exe, dump env, import VS-relevant vars into this bash.
# Use printenv-style key=value parsing (no awk regex on backslash-heavy paths).
while IFS='=' read -r key val; do
    case "$key" in
        INCLUDE|LIB|LIBPATH|Path|PATH|VCINSTALLDIR|WindowsSdkDir|VCToolsInstallDir)
            export "$key=$val" ;;
    esac
done < <(cmd.exe /c "call \"$VCVARS\" >nul && set" 2>/dev/null | tr -d '\r')

exec "$@"
```

**Usage**:
```bash
bash scripts/dev/with-msvc-env.sh cmake --build --preset ninja-iter-msvc --target SmatchetStandalone
```

**Verify steps** (quote-safe — `/?` would glob-expand without quotes):
```bash
bash scripts/dev/with-msvc-env.sh cl /help        # prints MSVC banner
bash scripts/dev/with-msvc-env.sh cmake --version # prints VS-bundled CMake
```

## Ship order

All four slices are truly independent — ship in any order or in parallel. Suggested cheap-first ordering (just minimizes wall-clock to first review-feedback):

```text
Slice 3 (15 min, doc)  →  Slice 2 (30 min, config)  →  Slice 4 (1 h, script)  →  Slice 1 (3 h, script + checklist)
```

No correctness ordering — slice N doesn't unblock slice N+1. Pick whichever fits the next free slot.

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

- **Risk** (slice 2): union merge may interleave dates if the post-merge sort isn't run. Mitigation: optional sort script + a one-line note in agents/core/git-janitor.md. Cost of disorder is cosmetic.
- **Risk** (slice 1): pre-push hook adds latency to every shell-script push. Mitigation: only fires when `scripts/dev/*.sh` is in the diff; SMATCHET_SKIP_SHELL_LINT bypass for emergencies.
- **Non-goal**: addressing the remaining backlog-sweep slices (3 — merge-watcher, 7 — bucket-E tests). Those are tracked in their original plan and remain open.

## Verification

- **Per-slice**: ships via the autonomous ship-loop. Pure-docs slices (2/3) skip the dual-target build.
- **Slice 1 verify** — fixture-based, not self-test (the lint script itself uses `shellcheck`, `awk`, `sed`, `grep` which aren't on the preflight allowlist — self-test would falsely pass / fail depending on whether ubiquitous-POSIX-tools are exempt). Ship `tests/fixtures/shell-lint/`: 5 known-bad sample scripts (one per assertion) + 1 known-good script. Lint-test asserts each sample fails the expected check and the known-good passes clean.
- **Slice 2 verify**: deliberately create a merge conflict on applied.md (concurrent branch + parallel prepend) and confirm git auto-merges via the union driver. Verify with non-applied.md files that union does NOT apply (to confirm scope correction).
- **Slice 4 smoke** (quote-safe — `/?` would glob-expand without quotes):
  - `bash scripts/dev/with-msvc-env.sh cmake --version` → prints VS-bundled CMake banner
  - `bash scripts/dev/with-msvc-env.sh cl /help` → prints MSVC compiler help
- **Build gate**: N/A for slices 2/3; slices 1/4 are docs/scripts only.
- **Manual residue**: none.

## Out of scope

- One-entry-per-file backlog refactor (slice 2 stretch goal — deferred).
- Functional-parity test for skill-vs-agent forms (from the original sweep plan's slice 9 — already shipped as shape check).
- Bucket-E test coverage batch (the original plan's slice 7 — separate work).

## Implementation log

All four slices shipped 2026-05-28. Order matched the cheap-first suggestion.

- **Slice 2** — `b715eec1` · `feat(merge): slice 2 — applied.md union merge + sort script (#485)` — `.gitattributes` union driver scoped to `applied.md` only (not `process.md` / `tooling.md`, per plan correction); `scripts/dev/sort-applied-md.sh` added.
- **Slice 3** — pre-implementation triage rule landed in `docs/agent-rules/process-rules.md` (current line 31) as part of `71770c09` · `docs(process): slice 3 — pre-implementation triage rule (#484)`.
- **Slice 4** — `5015147c` · `feat(build): slice 4 — bash-side vcvars64 env wrapper (#486)` — `scripts/dev/with-msvc-env.sh` via `vswhere.exe` install detection.
- **Slice 1** — `8ebd840a` · `feat(shell-lint): test-shell-lint.sh with 5 rules + remediate 21 violators (#488)` — five-rule self-review linter with fixture-based bucket-A tests + 21 in-tree violator fixes.

## Deviations from plan

- **Slice 1 script name**: plan named the file `scripts/dev/lint-shell-self-review.sh`; shipped as `scripts/dev/test-shell-lint.sh` to match the `test-*.sh` discovery pattern in `test-all.sh` (plan's "Option A" wiring). Functionally identical, name follows project convention.
- **Slice 1 bonus**: ship-PR also fixed 21 in-tree shell-script violators caught by the new linter on its first run — out of scope on paper but logical to bundle since the linter was the diagnostic.
- **Slice 1 hook surface**: plan offered two delivery options (Option A: extend `test-all.sh`; Option B: Claude Code PostToolUse hook). Shipped Option A only. Option B remains future work if push-time enforcement proves needed.
- **Slice 2 scope correction**: caught during plan review — initial draft applied `merge=union` to three files; corrected to `applied.md` only because `process.md` / `tooling.md` parallel-delete different entries (union would wrongly preserve both).

## Verification (actual)

- **Slice 1**: `test-shell-lint.sh` ran clean on develop post-merge; five known-bad fixtures each fail the expected rule; `known-good.sh` passes clean. `tests/bats/shell_lint.bats` covers the fixture round-trip.
- **Slice 2**: union merge verified during PR #485 implementation by running deliberate concurrent prepends against `applied.md`.
- **Slice 3**: doc-only — verified by `grep "Pre-implementation triage" docs/agent-rules/process-rules.md`. Rule has been used in subsequent multi-slice plans (this very backfill is an instance — the plan was triaged before implementation began).
- **Slice 4**: smoke-tested via `bash scripts/dev/with-msvc-env.sh cmake --version` and `bash scripts/dev/with-msvc-env.sh cl /help` per plan § Verify steps.
- **Build gate**: N/A — slices 2/3 are pure docs/config; slices 1/4 are pure scripts; no `Source_Core/` C++ touched.
