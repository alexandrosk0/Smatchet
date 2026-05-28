# Plan — Shell-script self-review lint

> **Slug**: `shell-script-self-review-lint`.

## Context

Closes the 2026-05-28 P1 entry in `docs/backlog/agent-self-improvement/process.md` ("Implementer-side self-review didn't catch real shell-script bugs CR found in 4 of 9 slices this session"). The triggering session shipped shell scripts in slices 5, 6, 8, 10 and CodeRabbit caught **five genuine, non-stylistic** bugs across them:

| # | PR | Bug |
|---|---|---|
| 1 | #482 (`git-janitor.sh`) | `python` dependency used without preflight `command -v` guard; silent empty PR_STATE on python3-only hosts |
| 2 | #478 (`p4-git-sync-check.sh`) | unquoted `$git_not_p4` / `$p4_not_git` in `printf` — word-splitting on paths with spaces |
| 3 | #477 (Font Awesome download) | `curl -sSL` without `-f`/`--fail` — a 404 HTML page lands as the .ttf font file |
| 4 | #477 (Font Awesome download) | no sha256 verify on the network download — supply-chain risk + can't detect the corrupt 404 case either |
| 5 | #477 (CLI parser) | `--threshold=VALUE` bypassed validation that `--threshold VALUE` had — asymmetric `case` branches |

Each of those five findings consumed a CR re-review cycle (~30 min round-trip). All visible to a structured pre-push read-through, none caught by mine. Outcome after this lands: every shell-script slice ships through `scripts/dev/lint-shell-self-review.sh` as part of the existing pre-push test gate; the same five classes of bug fail the lint locally instead of CR's review queue.

Smatchet ships ~1–2 shell-script PRs per week, so the win compounds.

## Approach

Single shell script + fixture-driven bats coverage + checklist doc. Wired into the existing `scripts/dev/test-all.sh` discovery loop so it auto-runs in the pre-push gate — no new hook surface (rationale below).

**Assertions** in `scripts/dev/lint-shell-self-review.sh`, one function per bug class:

1. **Dependency preflight** — for every external command in a **closed allowlist** (`curl`, `gh`, `cmake`, `python`, `python3`, `jq`, `7z`, `cppcheck`, `clang-format`, `clang-tidy`, `bats`, `p4`) that appears in the script body, assert a `command -v <name>` (or `which <name>` / `type -p <name>`) guard exists. Allowlist-based, not body-scan, so bash builtins (`echo`, `printf`, `mapfile`, `comm`, `set`, `local`, `eval`, `exec`) and ubiquitous POSIX tools (`awk`, `sed`, `grep`, `head`, `cut`, `tr`) are not false-flagged. Catches bug #1.
2. **`shellcheck` clean** — run `shellcheck -S warning` against the script. Fail on SC2086 (unquoted expansion), SC2046 (word-split), SC2128 (array-as-string), SC2155 (declare-and-assign masks return), SC2068 (`$@` unquoted in array context). Catches bug #2.
3. **`curl -f` everywhere** — any `curl` invocation in the script body must carry `-f` or `--fail` (short-or-long flag check). Catches bug #3.
4. **sha256 verify on network downloads (diff-scoped)** — any **newly-added** `curl` line that writes to a path under `assets/`, `build/`, or the repo root must be followed within 10 lines by `sha256sum -c` or `--checksum`. Scoped to `git diff origin/develop...HEAD` so existing scripts are grandfathered (e.g. the Mesa 7z download in `.github/workflows/build-and-test.yml` predates this rule). Catches bug #4.
5. **`--key=value` ↔ `--key value` parity** — when the script has a `--<flag>)` case-branch, it must also have a `--<flag>=*)` case (or vice versa). Catches bug #5.

Each assertion emits `<path>:<line>: <rule-id>: <message>` so the implementer can jump to the fix. Bypass via `SMATCHET_SKIP_SHELL_LINT=1` (logged when used — emergency-only).

**Why Option A (`test-all.sh` integration), not Option B (PreToolUse hook)**: the hook surface is untested for `git push` vs `git push -u origin x` vs `git push --force-with-lease`, etc. — and getting the matcher wrong means the lint silently no-ops on some push variants. `test-all.sh` is already on the pre-push path (`scripts/dev/test-all.sh` is the canonical pre-push gate per BUILD.md), so the wiring is one block in an existing discovery loop. Hook config would also live in `.claude/settings.json` which is harness-adapter-gitignored; `test-all.sh` is checked in, so every clone gets the lint for free.

**Why a closed allowlist (not auto-discovery)**: auto-discovery would false-flag every `awk`/`sed`/`grep` in every script and the implementer ends up maintaining an exclusion list anyway. Twelve entries is small enough to commit to; extending the allowlist is a documented one-line change.

## Files to modify

1. `scripts/dev/lint-shell-self-review.sh` (new) — the five-assertion lint script.
2. `scripts/dev/test-all.sh` — wire `lint-shell-self-review.sh` into the discovery / run loop alongside the existing `test-*.sh` entries.
3. `tests/bats/shell_lint.bats` (new) — fixture-driven coverage (six fixtures, one per assertion plus one all-good).
4. `tests/fixtures/shell-lint/known-bad-1-deps.sh` (new) — uses `python` without `command -v`.
5. `tests/fixtures/shell-lint/known-bad-2-shellcheck.sh` (new) — has SC2086 unquoted expansion.
6. `tests/fixtures/shell-lint/known-bad-3-curl-fail.sh` (new) — `curl -sSL` without `-f`.
7. `tests/fixtures/shell-lint/known-bad-4-no-sha.sh` (new) — `curl … -o assets/x` without checksum follow-up.
8. `tests/fixtures/shell-lint/known-bad-5-flag-parity.sh` (new) — `--threshold)` case but no `--threshold=*)` case.
9. `tests/fixtures/shell-lint/known-good.sh` (new) — all five assertions clean.
10. `docs/agent-rules/shell-script-self-review.md` (new) — human-readable checklist. One section per assertion: rationale + example bad code + example fix + cross-link to the originating CR finding + lint rule id.
11. `BUILD.md` — single-line cross-link under § Dev scripts.
12. `agents/code-review.md` — cross-link the new checklist under the shell-script review section (read-only update — informs the agent's coverage rules).
13. `docs/backlog/agent-self-improvement/process.md` — archive the 2026-05-28 P1 entry on slice close.
14. `docs/backlog/agent-self-improvement/applied.md` — add the corresponding applied entry.

## Existing utilities reused

- `scripts/dev/test-all.sh` discovery loop — assumes the `test-*.sh` shape so the new lint script's filename must match (`test-shell-lint.sh` is the alternative if the discovery regex is strict on the `test-` prefix; check before naming).
- `shellcheck` — already a documented dep in `BUILD.md` § CLI prereqs (used elsewhere for ad-hoc validation but not yet gated).
- `bats` — existing test-runner; `tests/bats/` already houses 4 bats files (`lock_claim.bats`, `merge_gates.bats`, `merge_watcher.bats`, `merge_watcher_integration.bats`); new file fits the pattern.
- `git diff origin/develop...HEAD` — same diff invocation as `scripts/dev/perf-pr-fast-set.json` uses for diff-scoped scenario selection. Reuse the helper if extracted; otherwise inline.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — bash lint script, runs only at pre-push gate, not on the UI thread.
- **Pillar 2 (UI never blocks > 100 ms)**: N/A — same.
- **Pillar 3 (never crash)**: positive marginal impact — each of the five lint rules prevents a class of shell-script bug from reaching CI / production (e.g. corrupt-font landing, unquoted-path word-split crashing on spaces).
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff touches `scripts/dev/`, `tests/bats/`, `tests/fixtures/`, `docs/`, `BUILD.md`. No `Source_Core/` impact.

## Risks / non-goals

- **Pre-existing scripts may fail lint immediately**. Mitigation: run the lint against the whole `scripts/dev/*.sh` set as the final implementation step; either fix the violations in the same PR or grandfather via `# shellcheck disable=...` / explicit allow-list entries in the checklist with a cross-linked follow-up backlog entry. Don't ship lint that flags > 5 existing files without a remediation plan — that's noise the implementer will start `SMATCHET_SKIP_SHELL_LINT=1`-ing.
- **Diff-scoped rule #4 goes dormant** if no new `curl-to-file` lands for months — a regression could slip in alongside an unrelated rewrite. Mitigation: ship a follow-up cleanup PR that adds checksums to existing grandfathered callers (Mesa 7z is the known one), then flip rule #4 to repo-wide. Tracked as out-of-scope below.
- **Closed allowlist drift** — new external tool (e.g. `node`, `npm`, `ripgrep`) gets used in a script and isn't on the lint allowlist → preflight check not enforced. Mitigation: the lint emits a `INFO: tool '<name>' not in allowlist; consider adding` line so the implementer at least sees the gap; extending the allowlist is a documented one-line change in `lint-shell-self-review.sh`.
- **shellcheck-not-installed** on a contributor's host. Mitigation: lint exits 0 with a `WARN: shellcheck not on PATH; install via …` line — don't block push on toolchain-absence (matches the existing `cppcheck` / `clang-tidy` behaviour in the C++ lint pipeline).
- **Non-goal**: linting the shell heredocs embedded in Python / C++ source. Out of scope; would require a parser that pulls heredoc bodies out, which is brittle. Stays manual.
- **Non-goal**: linting bash inside `.github/workflows/*.yml`. Different review path (workflow files already go through the GH Actions linter at PR time). Stays manual.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — bash lint script, no C++ surface.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario** — `tests/bats/shell_lint.bats` covers all six fixtures; bats invokes `lint-shell-self-review.sh` against each fixture and asserts the expected exit code + rule id in stderr. Runs as part of `bash scripts/dev/test-all.sh` (existing bats discovery loop). Self-test of the lint script itself is **deliberately not done** — the lint script uses `shellcheck`, `awk`, `sed`, `grep` which are not on the preflight allowlist, so self-test would falsely pass or fail depending on whether the implementer added a special-case for `lint-shell-self-review.sh` itself. The fixture approach is cleaner.
- **Build gate**: N/A — no C++ touched.
- **Manual residue**: zero — five lint rules + six fixtures cover every CR finding cited in the backlog entry. The follow-up of remediating grandfathered scripts (risk #2 above) is tracked separately and does not block this slice.

## Out of scope (flagged, not designed)

- **Flipping rule #4 (sha256) from diff-scoped to repo-wide**. Requires adding checksums to existing grandfathered callers first (Mesa 7z in `build-and-test.yml`; any other `curl-to-file` already on the tree). Defer to follow-up `docs/design/shell-lint-grandfather-cleanup.md` plan.
- **Per-script `# lint-allow: <rule>` escape valves**. The bypass env var (`SMATCHET_SKIP_SHELL_LINT=1`) is the only escape hatch in this slice; per-script ignores would let one stale ignore comment defeat the whole gate. Add only if a real recurring false-positive surfaces.
- **Linting workflow YAML or embedded heredocs**. Separate review surface; not addressed here.
- **PreToolUse hook integration** (Option B from § Approach). Evaluate after this lands if the test-all.sh path proves insufficient (e.g. push gate runs too late, agents bypass it). Default assumption: Option A is sufficient.

## Implementation log

*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit)*

## Deviations from plan

*(populated post-ship)*

## Verification (actual)

*(populated post-ship)*
