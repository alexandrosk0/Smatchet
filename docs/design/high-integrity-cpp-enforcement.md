# Plan — High-Integrity C++ Enforcement (slim profile)

> **Slug**: `high-integrity-cpp-enforcement`.

## Context

Proposal floated 2026-05-28 to create a "Smatchet High-Integrity C++ Profile" — six phases, a new profile doc, a new scanner, a new CI workflow, a manual triage workflow, a deviation-comment migration, and a five-rule promotion ladder. Estimated effort ~20+ hours.

Audit of existing infrastructure (this plan's § Approach) shows ~70% of the proposal's rule list is already enforced today via `AGENTS.md` § Project rules + `scripts/dev/test-lint-rules.sh` (shipped #468) + cppcheck/clang-tidy PostToolUse hooks + the `Sanitizer (ASAN + UBSAN)` CI job + the existing exemption-comment convention (`// C-ABI handle`, `// custom-deleter`, `// CLI stdout`, `// pre-logger-init`).

The genuine value the full proposal would add lives in four ideas, not the rule list:

1. **Tiered enforcement** — strict zone (parsing / sync / cache / config / command-registry), light zone (UI-heavy files), exempt zone. Current enforcement is uniform with per-site exemptions.
2. **Delta gate** — fail only on **new** violations vs `origin/develop`, grandfather existing. PR #488 used this pattern manually for shell-lint; C++ violator volume makes manual rewrites infeasible.
3. **Two uncovered rules + `deviation-overdue`** — silent narrowing (clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs) and `#define ImGui` macro-alias tricks (one-liner grep). Dynamic-index bounds checks dropped from this PR (see § Out of scope — Risks §2 conceded the rule wasn't ready to gate; ASAN+UBSAN in the existing `Sanitizer` CI job already catches the runtime class). `deviation-overdue` enforces the calendar-marker `revisit=` field from the `SMATCHET_DEVIATION` grammar.
4. **Forward-only `SMATCHET_DEVIATION(rule)` comment format** — `reason / owner / revisit-trigger` triple. Adds an audit-able "review this site by date X" surface that the existing one-line exemption comments lack.

Outcome after this lands: existing strict-zone violators are catalogued in `docs/backlog/high-integrity-cpp-baseline.md` (grandfathered); every NEW violation in a strict-zone file fails CI; the two uncovered rules + `deviation-overdue` join the existing scanner; new deviation sites use the canonical comment format; the tiered scope is named in AGENTS.md with no separate profile doc to drift.

**Effort estimate**: ~7 h (post-grill), up from the initial ~4-6 h estimate. The drift is concentrated in bats coverage (~1.5 h) and CI wiring (~0.5 h) — both costs the initial estimate handwaved. Still well under the full proposal's ~20+ h. Sliced as one PR (not multiple) because the parts are tightly coupled — the scanner, the AGENTS.md doc, the baseline file, and the bats coverage all land together or the gate's behaviour is incoherent. Internal slice breakdown (commit-per-row):

| Slice | Hours |
|---|---|
| `test-lint-rules.sh` — 3 modes (`--diff`, `--catalog`, `--refresh`), zone-glob loader, normalised-snippet hasher, `(rule, basename, hash)` set-diff | 1.5 |
| `test-lint-rules.sh` — 2 new rule checks (clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs; `\b#define\s+ImGui\b` grep) + `deviation-overdue` parser | 1.0 |
| `test-lint-rules.sh` — self-test re-reading AGENTS.md zone globs vs scanner copy; fail on drift | 0.3 |
| CI wiring — PR runner: `--diff origin/develop`; `develop` post-merge: `--catalog --refresh` + **fail-on-drift** (locked — see below) | 0.5 |
| `docs/backlog/high-integrity-cpp-baseline.md` — initial generation + commit | 0.2 |
| `AGENTS.md` § Tiered enforcement subsection + `SMATCHET_DEVIATION` grammar paragraph | 0.3 |
| `tests/bats/lint_rules.bats` — `--diff`, `--catalog`, each new rule, deviation grammar parsing, overdue detection, idempotency | 1.5 |
| `tests/fixtures/lint_rules/` — per-rule known-bad/known-good + `SMATCHET_DEVIATION` overdue/current fixture | 0.5 |
| `docs/agent-rules/process-rules.md` + `agents/coderabbit-triage.md` one-liner edits | 0.2 |
| Buffer (clang-tidy strict-zone-TU runtime + post-merge fail-on-drift validation) | 1.0 |

**CI baseline-refresh policy (locked)**: `develop` post-merge runs `--catalog --refresh` against a worktree and **fails the job on byte-diff vs the committed baseline file** — never auto-commits. Remediation message: `"baseline stale — run 'bash scripts/dev/test-lint-rules.sh --catalog --refresh' locally and commit"`. Trade chosen: clear contributor signal + CI stays read-only against `develop`, vs auto-commit's friendlier UX at the cost of a new CI-writes-to-develop surface. Smatchet has no other such surface today; introducing one for a snapshot file isn't earned.

## Approach

Extend the existing rule infrastructure rather than parallel-shipping new infrastructure. Five concrete edits, in order:

1. **`AGENTS.md` § Project rules § Tiered enforcement** (new subsection, ≤ 20 lines). Names the three zones by directory glob, references the existing rules (no rule restatement) and the scanner. Single source of truth. Zone globs (locked):

   ```
   strict_zone:
     - Source_Core/src/Tracker/**     # tracker-backend parsing/payload
     - Source_Core/src/Sync/**        # offline-queue replay, sync diff
     - Source_Core/src/Persistence/** # SQLite cache + audit-trail
     - Source_Core/src/Config/**      # config-key parsing
     - Source_Core/src/Commands/**    # command registry + scenario parsing
     - Plugins/Mcp/src/**             # MCP JSON-RPC parsing
   light_zone:
     - Source_Core/src/Ui/**          # ImGui-heavy files, render code
     - Target_Standalone/**           # GLFW + GL bootstrap
   exempt_zone:
     - ThirdParty/**, build/**, non-C++ trees
   ```

   The same glob list is asserted at the top of `test-lint-rules.sh` so AGENTS.md and the scanner cannot drift; a self-test on the scanner re-reads AGENTS.md and diffs the two lists.
2. **`scripts/dev/test-lint-rules.sh` extended with `--diff` and `--catalog` modes + the three uncovered rules**.

   - **`--diff <baseline-ref>` mode** (default `origin/develop`). Computes the set of `(rule, file-basename, snippet-hash)` triples on `baseline-ref` and on `HEAD`, fails only when `HEAD \ baseline` is non-empty. `snippet-hash` = SHA1 of the violating line's normalised content (strip leading whitespace + trailing `//` or `/* */` comment). Renames + line moves within a file don't trigger; only genuinely new `(rule, normalised-line)` instances do. Run via `git worktree add` against the baseline ref so the same scanner sees both trees without a checkout dance.
   - **CI is authoritative**: the PR runner invokes `--diff origin/develop` and fails the PR on any new triple. `merge-gates.sh` already polls all required checks. The pre-push hook runs the same gate locally for fast feedback but isn't the source of truth.
   - **`develop` post-merge job**: `--catalog --refresh` only. Never gates; just rewrites the baseline file. No diff-vs-self attempted (would be a no-op anyway).
   - **`--catalog` mode** dumps the current violator set to `docs/backlog/high-integrity-cpp-baseline.md` grouped by `(rule × zone)` — see Q3 for the exact format.
   - **Baseline file is a human-reading snapshot, not a gate input**. The gate re-computes on every run; `docs/backlog/high-integrity-cpp-baseline.md` exists only to make the grandfathered set visible to humans.
   - **Three new rules** — (a) clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs, (b) a one-liner `\b#define\s+ImGui\b` grep, (c) `deviation-overdue` (parses `SMATCHET_DEVIATION` comments, fails when `revisit=` is a calendar marker that has passed). Dynamic-bounds `operator[]` heuristic dropped — see § Out of scope.
3. **`docs/backlog/high-integrity-cpp-baseline.md`** — auto-generated snapshot, never a gate input (gate truth = live scan vs `origin/develop` per §2). Refresh runs on `develop` post-merge via `bash scripts/dev/test-lint-rules.sh --catalog --refresh`. Hand-edits are disallowed; CI re-runs the catalog and fails the post-merge job if the committed file diverges from a freshly-generated one (catches accidental edits).

   Layout (deterministic — `--refresh` produces byte-identical output when nothing changed):

   ```markdown
   # High-Integrity C++ — grandfathered baseline

   _Auto-generated. Do not hand-edit; run `bash scripts/dev/test-lint-rules.sh --catalog --refresh`._
   _Refreshed on `develop` post-merge; gate uses live scan vs `origin/develop`, not this file._

   Snapshot: <develop HEAD sha>  ·  <ISO-8601 timestamp>

   ## strict zone × narrowing-conversions (N entries)
   - `Source_Core/src/Tracker/TrackerFieldValueParser.cpp:142` · `<snippet-hash>`

   ## strict zone × no-printf-stderr (N entries)
   - (none)

   ## strict zone × no-raw-new (N entries)
   - `Plugins/Mcp/src/McpServer.cpp:201` · `<snippet-hash>`

   ## strict zone × define-imgui (N entries)
   - (none)

   ## strict zone × deviation-overdue (N entries)
   - (none — non-empty here would mean CI is broken)

   ## Totals
   - strict-zone violators grandfathered: N
   - last refresh: <timestamp>
   ```

   Section order = fixed rule-id order. Within a section: sorted by `(file path, line)`. No light-zone section (light zone isn't scanned for grandfathering per §5). No per-entry rationale (this is a snapshot; justifications belong in `SMATCHET_DEVIATION` comments at the source).
4. **`SMATCHET_DEVIATION` comment format documented in AGENTS.md § Project rules** alongside the existing one-line exemptions. Accept both forms indefinitely. The new format becomes mandatory only when an existing exemption-comment site is touched for an unrelated reason (a soft promotion path; nothing forces a rewrite pass).

   Canonical grammar (pure comment — zero compile-time / preprocessor cost; no header to include; safe in DX12-only TUs; matches existing exemption-comment vocabulary):

   ```cpp
   // SMATCHET_DEVIATION(rule=<rule-id>; reason=<short>; owner=<handle>; revisit=<trigger>)
   int width = json_obj["width"].get<int64_t>();  // narrowing accepted: tracker payload bounded
   ```

   Rules:
   - `rule` first, semicolon-separated key=value pairs, no leading space inside parens.
   - `rule` = scanner rule-id (matches the names `test-lint-rules.sh` emits — automatic linkage).
   - `reason` = free text, no semicolons.
   - `owner` = github handle or the literal string `unowned` (never blank).
   - `revisit` = calendar marker (`2026-Q3`, `2026-09-01`), slug (`after-tracker-rewrite`), or `never`.
   - The next non-blank source line is the deviation target. A `SMATCHET_DEVIATION` with no following target in the same scope is itself an error (catches stale deviations left after the code was deleted).
   - **Revisit-overdue enforcement (locked)**: when `revisit` is a calendar marker (`YYYY-Qn` or `YYYY-MM-DD`) that has passed, the scanner emits `deviation-overdue` (a strict-zone rule). Forces the audit-loop the comment format exists for. Slug + `never` triggers don't expire.

   Scanner reads the deviation comment and the next non-blank line; rule-id from the comment suppresses the matching rule on that line. CodeRabbit gets one `.coderabbit.yaml` path-instruction (deferred — see Out of scope) to recognise the pattern.
5. **Auto-categorize via `(rule × zone)`** — two outcomes only, no warn tier:
   - **strict zone × any rule** → fail (existing violators grandfathered via `--diff origin/develop`; new ones fail CI).
   - **light zone × any rule** → ignore. The existing exemption-comment vocabulary (`// pre-logger-init`, `// CLI stdout`, `// C-ABI handle`, `// custom-deleter`) continues to apply per AGENTS.md § Project rules. No second enforcement lane.
   - **exempt zone** → not scanned at all (`ThirdParty/**`, `build/**`, non-C++ trees).
   - False-positive cases get added to the scanner's allow-list with a one-line rationale (matches the existing `# shellcheck disable=SC2086 — intentional split` pattern).
   - **Promotion path**: if a future rule turns out to matter in the light zone, promote the relevant subdirectory to strict for that rule via a glob edit in AGENTS.md + `test-lint-rules.sh`. Don't grow a permanent warn tier.

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
- **Dynamic-bounds rule dropped from this PR** (see § Out of scope). The heuristic's false-positive rate against `for (i; i<vec.size(); ...) v[i]` patterns was too high to gate; a "light-zone warn" that never fails CI was scanner code without behaviour change. ASAN+UBSAN in the existing `Sanitizer` CI job already catches the runtime class. Promotion path: a follow-up PR with a parameter-driven-index heuristic scoped to strict-zone files, evidence-driven not pre-scheduled.
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

- **Dynamic-bounds `operator[]` heuristic**. Dropped from this PR. Three options were considered: clang-tidy `cppcoreguidelines-pro-bounds-constant-array-index` (misses `std::vector[i]` — wrong target), a custom "parameter-driven index without prior `idx < vec.size()` check" regex (the actual crash shape, but unproven false-positive rate), and shipping it as a light-zone-warn-only rule (documentation, not enforcement). The strict-zone clang-tidy infrastructure this plan stands up makes adding the next cppcoreguidelines check a ~10-line follow-up; promotion is evidence-driven.
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
