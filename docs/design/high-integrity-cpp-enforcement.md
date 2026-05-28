# Plan — High-Integrity C++ Enforcement (slim profile)

> **Slug**: `high-integrity-cpp-enforcement`.

## Context

Proposal floated 2026-05-28 to create a "Smatchet High-Integrity C++ Profile" — six phases, a new profile doc, a new scanner, a new CI workflow, a manual triage workflow, a deviation-comment migration, and a five-rule promotion ladder. Estimated effort ~20+ hours.

Audit of existing infrastructure (this plan's § Approach) shows ~70% of the proposal's rule list is already enforced today via `AGENTS.md` § Project rules + `scripts/dev/test-lint-rules.sh` (shipped #468) + cppcheck/clang-tidy PostToolUse hooks + the `Sanitizer (ASAN + UBSAN)` CI job + the existing exemption-comment convention (`// C-ABI handle`, `// custom-deleter`, `// CLI stdout`, `// pre-logger-init`).

The genuine value the full proposal would add lives in four ideas, not the rule list:

1. **Tiered enforcement** — strict zone (parsing / sync / cache / config / command-registry), light zone (UI-heavy files), exempt zone. Current enforcement is uniform with per-site exemptions.
2. **Delta gate** — fail only on **new** violations vs `origin/develop`, grandfather existing. PR #488 used this pattern manually for shell-lint; C++ violator volume makes manual rewrites infeasible.
3. **Three uncovered rules** — silent narrowing, dynamic-index bounds checks, `#define ImGui` macro-alias tricks. Not currently caught by any scanner.
4. **Forward-only `SMATCHET_DEVIATION(rule)` comment format** — `reason / owner / revisit-trigger` triple. Adds an audit-able "review this site by date X" surface that the existing one-line exemption comments lack.

Outcome after this lands: existing strict-zone violators are catalogued in `docs/backlog/high-integrity-cpp-baseline.md` (grandfathered); every NEW violation in a strict-zone file fails CI; the three uncovered rules join the existing scanner; new deviation sites use the canonical comment format; the tiered scope is named in AGENTS.md with no separate profile doc to drift.

## Approach

Extend the existing rule infrastructure rather than parallel-shipping new infrastructure. Five concrete edits, in order:

1. **`AGENTS.md` § Project rules § Tiered enforcement** (new subsection, ≤ 15 lines). Names the three zones by directory glob, references the existing rules (no rule restatement) and the scanner. Single source of truth.
2. **`scripts/dev/test-lint-rules.sh` extended with `--diff` and `--catalog` modes + the three uncovered rules**. The `--diff` mode is invoked by `test-all.sh` at pre-push and compares strict-zone violations between `HEAD` and `origin/develop`, failing only on new ones. `--catalog` dumps the current violator set to `docs/backlog/high-integrity-cpp-baseline.md` grouped by `(rule × zone)`. The three new rules — clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs, a heuristic `operator[]` scan, and a one-liner `\b#define\s+ImGui\b` grep — slot into the existing rule-loop.
3. **`docs/backlog/high-integrity-cpp-baseline.md`** — first generated via `bash scripts/dev/test-lint-rules.sh --catalog`. Treated as a tracking artefact, not a hand-edited list. Refresh trigger: any PR whose `--diff` mode would have produced a violation but grandfathered it (so the baseline records what was let through).
4. **`SMATCHET_DEVIATION(rule)` comment format documented in AGENTS.md § Project rules** alongside the existing one-line exemptions. Accept both forms indefinitely. The new format becomes mandatory only when an existing exemption-comment site is touched for an unrelated reason (a soft promotion path; nothing forces a rewrite pass).
5. **Auto-categorize the baseline** via `(rule × zone)` rather than manual triage:
   - strict zone × any rule → `needs-fix` (tracked in the baseline; new ones fail CI)
   - light zone × catch-all / new / cerr → `deviation` (warn, not fail; comment recommended)
   - exempt zone → `ignore`
   - false-positive cases get added to the scanner's allow-list with a one-line rationale (matches the existing `# shellcheck disable=SC2086 — intentional split` pattern)

Trade-off: the slim version preserves the existing one-source-of-truth structure (`AGENTS.md` + `test-lint-rules.sh` + sanitizer + cppcheck) instead of adding a parallel profile doc + parallel scanner + dedicated CI workflow. The rhetorical signal "this is a Major Thing" is weaker than a standalone profile doc would carry; the trade is one source of truth vs marketing weight.

## Files to modify

1. `AGENTS.md` — add § Tiered enforcement subsection under § Project rules; ≤ 15 lines.
2. `scripts/dev/test-lint-rules.sh` — add `--diff` mode (default for `test-all.sh` invocation), `--catalog` mode, the three new rule checks (narrowing via clang-tidy, dynamic-index heuristic, `#define ImGui` grep), and zone-tier classification.
3. `docs/backlog/high-integrity-cpp-baseline.md` (new) — initial dump from `--catalog`; lives under backlog rather than design since it's a tracking artefact, not a plan.
4. `tests/bats/lint_rules.bats` (new) — bats coverage for `--diff` mode, `--catalog` mode, and each of the three new rules (one bats per rule, one all-clean fixture). Pattern mirrors `tests/bats/shell_lint.bats` from #488.
5. `tests/fixtures/lint_rules/` (new) — per-rule known-bad + known-good fixtures.
6. `docs/agent-rules/process-rules.md` — single-line note under § Plan-doc family pointing at the new tiered-enforcement subsection of AGENTS.md (so plan authors are reminded to declare their zone).
7. `agents/coderabbit-triage.md` — one-line override-table entry recommending `SMATCHET_DEVIATION` for new strict-zone exemption requests rather than ad-hoc inline comments. No mandatory enforcement; just routing guidance.

## Existing utilities reused

- `scripts/dev/test-lint-rules.sh` (shipped #468, ~40 LoC) — existing rule-loop, exemption-comment allowlist, file-walker. Reused as the foundation; this plan extends it rather than forking.
- `scripts/dev/test-shell-lint.sh` pattern — bats fixture layout, `--target` flag, header doc, `SMATCHET_SKIP_*` env-bypass convention.
- `scripts/dev/test-all.sh` discovery glob (`test-*.sh`) — auto-runs the extended script at pre-push gate.
- `Sanitizer (ASAN + UBSAN)` CI job — already covers Pillar 3 contract; no new CI workflow needed.
- `cppcheck` + `clang-tidy` PostToolUse hooks — already enforce most of the rule list at C++-edit time.
- Existing inline exemption-comment allowlist (`// C-ABI handle`, `// custom-deleter`, `// CLI stdout`, `// pre-logger-init`) — accepted in perpetuity; `SMATCHET_DEVIATION(rule)` is a forward-only superset.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — the script runs at pre-push, not on the UI thread. clang-tidy on strict-zone TUs adds ~5-15 s to the local lint step; offset against the per-PR runner time the new CI workflow would have added.
- **Pillar 2 (UI never blocks > 100 ms)**: N/A — same.
- **Pillar 3 (never crash)**: **positive marginal impact** — the three new rules (narrowing, dynamic-bounds, `#define ImGui`) close gaps in the current crash-prevention enforcement. Narrowing in particular is a known footgun class (silent int truncation in tracker payload parsers); a heuristic + clang-tidy combo would catch the common shapes.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A — diff touches only `AGENTS.md`, `scripts/dev/`, `docs/`, `tests/bats/`, `tests/fixtures/`. The three new rule checks read `Source_Core/` but don't modify it.

## Risks / non-goals

- **Marketing-weight loss vs full proposal**. The slim version doesn't have a standalone "High-Integrity C++ Profile" doc; the rules live inside AGENTS.md. Contributors browsing the design folder will not see a flag-planting doc that says "this is a high-integrity codebase." Mitigation: the AGENTS.md subsection is title-cased clearly + the baseline doc under `docs/backlog/` is hand-linked from the subsection. Accepted as a marketing trade for the one-source-of-truth structural win.
- **Heuristic dynamic-bounds rule false-positive rate**. Detecting "dynamic indexing without bounds check" via grep / clang-tidy is inherently noisy — many safe patterns (e.g. `vec[i]` inside `for (i = 0; i < vec.size(); ...)`) will trip the heuristic. Mitigation: ship as a `light-zone warn` first; promote to strict-zone fail only after observed false-positive rate is in single-digit percent across the first 10 PRs.
- **`SMATCHET_DEVIATION` not made mandatory** means existing exemption sites won't carry a revisit date. The audit-the-deviation surface this would enable stays partial. Mitigation: forward-only is a deliberate choice to avoid the rewrite-pass cost (estimated ~50 sites × 2 min = ~2 h of zero-behaviour-change churn). The mandatory form can be promoted later if review experience shows it's worth the rewrite.
- **Baseline file becomes stale**. If contributors merge violations that the `--diff` gate happens to grandfather (e.g. moved a violation to a different file), the baseline doc gets out of sync with reality. Mitigation: a cron-style `bash scripts/dev/test-lint-rules.sh --catalog --refresh` invocation in CI on `develop` post-merge keeps the baseline refreshed.
- **Non-goal**: MISRA-C/C++ compliance certification. The rule set is curated for Smatchet-specific risk patterns; making a MISRA claim would require formal traceability tooling we don't have and don't need.
- **Non-goal**: refactoring existing strict-zone violators. The whole point of the delta-gate approach is to grandfather existing code; refactoring decisions stay per-file and per-bug as they come up naturally.
- **Non-goal**: gating on cppcoreguidelines as a whole. We're cherry-picking `narrowing-conversions` for now; the full ruleset has too many noisy checks for a Smatchet-tuned profile.

## Verification

- **Bucket A (pure-logic ctest)**: N/A — bash + clang-tidy invocation, no C++ helpers exposed.
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Bash-driver scenario**: `tests/bats/lint_rules.bats` covers `--diff` mode (new violation in fixture fails; unchanged violation grandfathered), `--catalog` mode (output format + group-by zone), and each of the three new rule checks via known-bad + known-good fixtures.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — dual-target. The strict-zone clang-tidy pass runs as part of the existing PostToolUse hook chain; no new dual-target gate.
- **Manual residue**: zero. The three new rules each have a fixture; the `--diff` mode is exercised by bats with a stubbed `git diff` baseline; the categorization auto-derives from path + rule.

## Out of scope (flagged, not designed)

- **Phase 4 dedicated CI workflow** (`.github/workflows/high-integrity-cpp.yml`). Pre-push `test-all.sh` + existing `Sanitizer` + `Windows + MSVC` jobs are enough. Add a dedicated workflow only if the pre-push gate is bypassed (e.g. someone force-pushes). Decide on evidence.
- **Phase 5 promotion ladder** (5 rules in fixed order). Each rule is a separate ~30-min PR when it earns its way in based on observed false-positive rate + bug-catch payoff. Pre-scheduling the order predicts a future we can't see.
- **Phase 6 PR-template checkbox**. Smatchet doesn't gate on checkboxes; merge-gates.sh + CodeRabbit own the merge contract.
- **Phase 6 CodeRabbit path-specific instructions**. CR already cross-references AGENTS.md rules in its reviews; adding `.coderabbit.yaml` path instructions adds maintenance for marginal lift.
- **Mandatory `SMATCHET_DEVIATION` migration**. Forward-only is the explicit choice; an existing-sites rewrite is a separate cleanup PR if/when reviewing experience says it pays off.
- **Categorization of every baseline entry as `fix-now / deviation / needs-refactor / false-positive`**. Auto-derive from `(rule × zone)` and skip the manual-triage workflow.

## Implementation log

*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan

*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)

*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
