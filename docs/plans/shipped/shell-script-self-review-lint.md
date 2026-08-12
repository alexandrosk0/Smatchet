# Plan — Shell-script self-review lint
<!-- plan-date: 2026-05-28 -->

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

Each of those five findings consumed a CR re-review cycle (~30 min round-trip). All visible to a structured pre-push read-through, none caught by mine. Outcome after this lands: every shell-script slice ships through `scripts/dev/test-shell-lint.sh` as part of the existing pre-push test gate; the same five classes of bug fail the lint locally instead of CR's review queue.

Smatchet ships ~1–2 shell-script PRs per week, so the win compounds.

## Approach

Single shell script + fixture-driven bats coverage + checklist doc. Wired into the existing `scripts/dev/test-all.sh` discovery loop (which globs `test-*.sh`) — no new hook surface (rationale below). Lint script is named `test-shell-lint.sh` so the existing discovery picks it up unchanged.

**Assertions** in `scripts/dev/test-shell-lint.sh`, one function per bug class. **All five rules repo-wide** (lint every shell script in `scripts/dev/*.sh`; YAML workflows out of scope):

1. **Dependency preflight** — for every external command in a **closed allowlist of 20** (`curl`, `gh`, `git`, `cmake`, `python`, `python3`, `jq`, `7z`, `cppcheck`, `clang-format`, `clang-tidy`, `shellcheck`, `actionlint`, `bats`, `p4`, `cl.exe`, `clang-cl`, `link.exe`, `cygpath`, `tasklist`) that appears in the script body, assert a `command -v <name>` (or `which <name>` / `type -p <name>`) guard exists. Allowlist-based, not body-scan, so bash builtins (`echo`, `printf`, `mapfile`, `comm`, `set`, `local`, `eval`, `exec`) and ubiquitous POSIX tools (`awk`, `sed`, `grep`, `head`, `cut`, `tr`, `find`, `xargs`) are not false-flagged. Tools used but **not** on the allowlist get an `INFO: tool '<name>' not in allowlist; consider adding to scripts/dev/test-shell-lint.sh` line (non-blocking) so drift is visible. Catches bug #1.
2. **`shellcheck` clean** — run `shellcheck -S warning` against the script. Fail on SC2086 (unquoted expansion), SC2046 (word-split), SC2128 (array-as-string), SC2155 (declare-and-assign masks return), SC2068 (`$@` unquoted in array context). Catches bug #2.
3. **`curl -f` everywhere** — any `curl` invocation in the script body must carry `-f` or `--fail` (short-or-long flag check). Catches bug #3.
4. **sha256 verify on network downloads** — any `curl` line that writes to a path (matches `-o <path>` / `--output <path>`) must be followed within 10 lines by `sha256sum -c` or an explicit pinned-checksum compare. Repo-wide; zero existing violators in `scripts/dev/*.sh` at plan time. Catches bug #4.
5. **`--key=value` ↔ `--key value` parity** — when the script has a `--<flag>)` case-branch **AND that branch consumes `$2` / `shift 2`** (i.e. the flag takes a value), it must also have a `--<flag>=*)` case (or vice versa). Boolean flags (`--<flag>)` body has no `$2` consumption, no `shift 2`) are skipped — avoids forcing `--verbose=*)` no-op boilerplate. Catches bug #5.

Each assertion emits `<path>:<line>: <rule-id>: <message>` so the implementer can jump to the fix. Bypass via `SMATCHET_SKIP_SHELL_LINT=1` (logged when used — emergency-only).

**Why Option A (`test-all.sh` integration), not Option B (PreToolUse hook)**: the hook surface is untested for `git push` vs `git push -u origin x` vs `git push --force-with-lease`, etc. — and getting the matcher wrong means the lint silently no-ops on some push variants. `test-all.sh` is already on the pre-push path (per BUILD.md), so the wiring is one block in an existing discovery loop. Hook config would also live in `.claude/settings.json` which is harness-adapter-gitignored; `test-all.sh` is checked in, so every clone gets the lint for free.

**Why a closed allowlist of 20 (not auto-discovery)**: auto-discovery would false-flag every `awk`/`sed`/`grep` in every script. Twenty entries is small enough to commit to; extending the allowlist is a documented one-line change. The 20 cover every external command currently preflight-checked across `scripts/dev/*.sh` (audited at plan time).

**Pre-existing violator remediation in same PR**: shellcheck against the current tree shows ~13 scripts with warnings (`coverage-delta-gate.sh`, `doctor.sh`, `locks-render-markdown.sh`, `p4-git-sync-check.sh`, `p4-reconcile-check.sh`, `perf-marker-inventory.sh`, `tail-agent.sh`, `test-doc-anchors.sh`, `test-doctor.sh`, `test-lint-hook-split.sh`, `test-lint-rules.sh`, `test-setup-harness.sh`, `test-skill-load-log.sh`). One script (`smatchet-notify.sh:70`) has `curl -sS` without `-f`. All get fixed in the same PR so the lint ships green against the existing tree.

## Files to modify

1. `scripts/dev/test-shell-lint.sh` (new) — the five-assertion lint script. Lints itself.
2. `scripts/dev/test-all.sh` — no edit needed; existing `test-*.sh` discovery glob picks up the new lint automatically.
3. `tests/bats/shell_lint.bats` (new) — fixture-driven coverage (six fixtures, one per assertion + one all-good).
4. `tests/fixtures/shell_lint/known-bad-1-deps.sh` (new) — uses `python` without `command -v`.
5. `tests/fixtures/shell_lint/known-bad-2-shellcheck.sh` (new) — has SC2086 unquoted expansion.
6. `tests/fixtures/shell_lint/known-bad-3-curl-fail.sh` (new) — `curl -sSL` without `-f`.
7. `tests/fixtures/shell_lint/known-bad-4-no-sha.sh` (new) — `curl … -o assets/x` without sha256 follow-up.
8. `tests/fixtures/shell_lint/known-bad-5-flag-parity.sh` (new) — `--threshold)` case with `shift 2` but no `--threshold=*)` case.
9. `tests/fixtures/shell_lint/known-good.sh` (new) — all five assertions clean.
10. `docs/agent-rules/shell-script-self-review.md` (new) — human-readable checklist. One section per assertion: rationale + example bad code + example fix + cross-link to the originating CR finding + lint rule id.
11. `BUILD.md` — single new row in the CLI-prereqs table (line ~40): `| \`shellcheck\` | scripts/dev/test-shell-lint.sh (pre-push gate) | \`npm install -g shellcheck\` |`.
12. `AGENTS.md` — single line under § Project rules § Lint: `**Shell lint**: shell scripts go through \`scripts/dev/test-shell-lint.sh\` (5 rules; checklist at \`docs/agent-rules/shell-script-self-review.md\`). Auto-runs via \`scripts/dev/test-all.sh\` at pre-push.`
13. `scripts/dev/coverage-delta-gate.sh` + `doctor.sh` + `locks-render-markdown.sh` + `p4-git-sync-check.sh` + `p4-reconcile-check.sh` + `perf-marker-inventory.sh` + `tail-agent.sh` + `test-doc-anchors.sh` + `test-doctor.sh` + `test-lint-hook-split.sh` + `test-lint-rules.sh` + `test-setup-harness.sh` + `test-skill-load-log.sh` — fix the shellcheck warnings flagged at plan time.
14. `scripts/dev/smatchet-notify.sh` — add `-f` to the localhost-probe `curl` (line 70) — even on a localhost probe, `-f` is the established convention.
15. `docs/backlog/agent-self-improvement/process.md` — archive the 2026-05-28 P1 entry on slice close.
16. `docs/backlog/agent-self-improvement/applied.md` — add the corresponding applied entry.

## Existing utilities reused

- `scripts/dev/test-all.sh` discovery loop (line 46: `find scripts/dev -maxdepth 1 -type f -name 'test-*.sh' ! -name 'test-all.sh' | sort`). Lint script named to match.
- `shellcheck` — documented dep in `BUILD.md` § CLI prereqs (this PR adds the explicit row); installed via npm on the primary dev host.
- `bats` — existing test runner; `tests/bats/` houses 4 bats files; new file fits the pattern.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — bash lint runs at pre-push gate, not on the UI thread.
- **Pillar 2 (UI never blocks > 100 ms)**: N/A — same.
- **Pillar 3 (never crash)**: positive marginal impact — each of the five rules prevents a class of shell-script bug from reaching CI / production (corrupt-font landing, unquoted-path word-split crashing on spaces, etc.).
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff touches `scripts/dev/`, `tests/bats/`, `tests/fixtures/`, `docs/`, `BUILD.md`, `AGENTS.md`. No `Source_Core/` impact.

## Risks / non-goals

- **Pre-existing-script remediation is the bulk of the work** — fixing ~13 shellcheck-warning scripts + the one `curl` regex in `smatchet-notify.sh` is roughly half the slice effort. Mitigation: each fix is independent + scoped to one warning class; can be batched per-file in a deterministic order. If any one fix turns out to require non-trivial refactor, grandfather it via a per-line `# shellcheck disable=SC<N>` with an inline rationale comment + a tooling backlog entry for the proper fix.
- **Closed allowlist drift** — new external tool (e.g. `node`, `npm`, `ripgrep`) gets used in a script and isn't on the allowlist → preflight check not enforced. Mitigation: the lint emits a non-blocking `INFO: tool '<name>' not in allowlist; consider adding` line so the implementer sees the gap.
- **shellcheck-not-installed** on a contributor's host. Mitigation: lint exits 0 with a `WARN: shellcheck not on PATH; install via 'npm install -g shellcheck'` line — don't block push on toolchain absence (matches the existing `cppcheck` / `clang-tidy` behaviour in the C++ lint pipeline).
- **Lint script linting itself** — if a future rule heuristic uses `--key=*)` parsing or other patterns the lint flags, the lint becomes self-blocking. Mitigation: any such case becomes a real bug to fix in the lint, not an ignore — that's the dogfood value.
- **Rule #5 boolean-flag heuristic** (`shift 2` detection) — false positives possible on scripts that consume `$2` via non-shift means (e.g. `--key) value=${1#--key=}`). Mitigation: documented in the checklist; lint emits a precise rule-id so the implementer can rewrite or escape via `SMATCHET_SKIP_SHELL_LINT=1` for the one edge case.
- **Non-goal**: linting shell heredocs embedded in Python / C++ source. Out of scope; would require a parser that pulls heredoc bodies out. Stays manual.
- **Non-goal**: linting bash inside `.github/workflows/*.yml`. Different review path (GH Actions linter at PR time). Stays manual.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — bash lint, no C++ surface.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario** — `tests/bats/shell_lint.bats` covers all six fixtures; bats invokes `scripts/dev/test-shell-lint.sh` against each fixture and asserts the expected exit code + rule id in stderr. Runs as part of `bash scripts/dev/test-all.sh` (existing bats discovery).
- **Self-test** — `test-shell-lint.sh` lints itself as the final assertion in its repo-wide pass; any rule body that violates its own rules surfaces as a real bug to fix, not an ignore.
- **Build gate**: N/A — no C++ touched.
- **Manual residue**: zero — five lint rules + six fixtures + self-lint cover every CR finding cited in the backlog entry.

## Out of scope (flagged, not designed)

- **Per-script `# lint-allow: <rule>` escape valves**. The bypass env var (`SMATCHET_SKIP_SHELL_LINT=1`) is the only escape hatch; per-script ignores would let one stale ignore comment defeat the gate over time. Add only if a real recurring false-positive surfaces.
- **Linting workflow YAML or embedded heredocs**. Separate review surface; not addressed here.
- **PreToolUse hook integration** (Option B from § Approach). Evaluate after this lands if the test-all.sh path proves insufficient (e.g. push gate runs too late, agents bypass it). Default assumption: Option A is sufficient.
- **Extending the allowlist to include `node` / `npm` / `ripgrep` / `docker`**. The non-blocking INFO line surfaces drift; lift only after a real script needs them.

## Effort (revised after grill-with-docs)

- Lint script + 5 rules + self-test: ~2 h.
- Bats + 6 fixtures: ~1 h.
- Checklist doc + BUILD.md / AGENTS.md cross-links: ~30 min.
- Remediation of ~13 shellcheck-warning scripts + 1 `curl -f` fix: ~2 h.
- Backlog archive: ~15 min.

Total: **~6 h** (revised from 3 h after locking repo-wide + same-PR cleanup).

## Implementation log

- Plan committed (`4326726` + `7a4ff94` after grill-with-docs revisions).
- Implementation single commit (this PR) — `scripts/dev/test-shell-lint.sh` (5 rules + self-lint, ~190 LoC); 6 fixtures under `tests/fixtures/shell_lint/`; `tests/bats/shell_lint.bats` (9 tests); `docs/agent-rules/shell-script-self-review.md` (the checklist); BUILD.md row + AGENTS.md one-liner; remediation of 21 pre-existing violators (3 shellcheck word-split annotations + 1 array refactor + 1 `curl -f` + 14 deps preflights + 6 flag-parity twin cases); backlog archive process.md → applied.md.

## Deviations from plan

- **Allowlist shrunk from 20 to 19 — dropped `git`.** Implementation discovered `git` is invoked without preflight in 25 existing scripts; preflighting a tool the dev environment is BUILD.md-required to have produces noise without value. The other 19 entries match real preflight gaps. Reflected in lint source comment + the checklist doc + applied.md narrative.
- **Rule #2 (shellcheck) gate**: original spec said `-S warning`. Implementation found SC2086 is `info`-level by default and `-S warning` filters it out — so `--include=SC2086,SC2046,SC2128,SC2155,SC2068` (no `-S` flag) is the correct invocation. Matches plan intent.
- **Rule #5 (flag parity) detection**: original window was 6 lines after the case-branch; implementation found that scanned into neighboring case-branches in dense `case` statements (false-positiving `--once` / `--diff` / `--files` boolean flags). Fix: `awk -v start=$lno '... {print; if (/;;/) exit}'` so the scan terminates at the branch's own `;;` (sed range `,/;;/p` was tried first but skips the start-line match).
- **Shellcheck stderr regex**: original `^In [^:]+ line N:` broke when shellcheck normalised absolute paths to `C:/...` (the drive-letter colon stopped the `[^:]+`). Fix: `^In .+ line N:`.
- **Effort actual vs estimated** — ~6 h estimated, ~5 h actual (deps preflights batched via awk inserter; flag-parity twin cases were one-line edits per script). No scope cuts.

## Verification (actual)

- Repo-wide lint: `bash scripts/dev/test-shell-lint.sh` → **Passed: 84  Failed: 0**.
- Bats: `bats tests/bats/shell_lint.bats` → **9 / 9** (env-gate, shellcheck-missing fallback, 5 rule fixtures, known-good fixture, self-lint).
- Self-lint: `bash scripts/dev/test-shell-lint.sh --target scripts/dev/test-shell-lint.sh` → clean.
- No `Source_Core/` changes — build gate skipped per project rules.
