# Plan — High-Integrity C++ Enforcement (slim profile)
<!-- plan-date: 2026-05-28 -->

> **Slug**: `high-integrity-cpp-enforcement`.

## Dependency — directory reorganization (precursor, blocking)

**This plan is blocked on a precursor reorg PR.** Audit during the grill (2026-05-28) found the codebase is *flat*: 154 `.cpp` files live directly under `Source_Core/src/` organized by filename prefix; the only real subdirectory is `Commands/`, and `Plugins/Mcp/` has no `src/`. The directory-based zone globs below (`Source_Core/src/Tracker/**` etc.) therefore reference directories that **do not exist yet**.

Decision (2026-05-28): rather than encode fragile filename-prefix globs, a **precursor reorganization PR** moves the flat files into real directories so the zone globs become valid. The reorg is mechanical (pure file moves + `#include` path fixups across the tree + CMake `GLOB`/source-list updates) and gets its **own plan-doc** (`docs/plans/shipped/source-core-dir-reorg.md`, to be written) and its own PR, ideally driven by the `mechanic` agent. This enforcement plan does not start until the reorg lands on `develop`.

**Target directory taxonomy** the reorg must produce (strict-zone dirs are the contract this plan depends on; exact per-file placement of edge cases is settled in the reorg PR's review):

| New dir | File families that move in | Zone |
|---|---|---|
| `Source_Core/src/Tracker/` | `Tracker*`, `Jira*`, `Plane*`, `GitHub*`, `Jql*`, `Issue*`, `IssueDraft`, `IssueTableSerializer`, `LabelEdit*`, `ProjectResolver`, `DefaultTrackerBackendFactory`, `FieldCatalog*` | strict |
| `Source_Core/src/Sync/` | `OfflineQueueService`, `TicketSyncService`, `NetworkUsageTracker` | strict |
| `Source_Core/src/Persistence/` | `LocalCacheManager`, `BackendAuditTrail`, `FieldEditAuditSource`, `*Cache` | strict |
| `Source_Core/src/Config/` | `ConfigManager*` | strict |
| `Source_Core/src/Commands/` | (already exists) | strict |
| `Plugins/Mcp/src/` | `McpJsonRpc*`, `McpPlugin*` | strict |
| `Source_Core/src/Ui/` | `Smatchet*Ui*`, `*Ui*`, `SmatchetTheme`, `SmatchetToast`, `Blame*`, `CodeColorView`, `Markdown*`, `CppSyntaxHighlight` | light |
| `Target_Standalone/` | (already exists) | light |
| `Source_Core/src/` (residual root + other new dirs e.g. `Ai/`, `Core/`) | everything else | exempt-by-default |

Risk this introduces: the reorg is a large-blast-radius diff (every `#include "Foo.h"` whose target moved must update). It must land as its own reviewed PR with a clean dual-target build (`SmatchetStandalone` + `SmatchetCore_DX12`) before this plan proceeds. If the reorg PR stalls or is rejected, this plan falls back to filename-prefix globs (the alternative considered + deferred during the grill).

## Context

Proposal floated 2026-05-28 to create a "Smatchet High-Integrity C++ Profile" — six phases, a new profile doc, a new scanner, a new CI workflow, a manual triage workflow, a deviation-comment migration, and a five-rule promotion ladder. Estimated effort ~20+ hours.

Audit of existing infrastructure (this plan's § Approach) shows ~70% of the proposal's rule list is already enforced today via `AGENTS.md` § Project rules + `scripts/dev/test-lint-rules.sh` (shipped #468) + cppcheck/clang-tidy PostToolUse hooks + the `Sanitizer (ASAN via MSVC)` CI job (build-and-test.yml:313 — ASAN only; MSVC has no UBSAN) + the existing exemption-comment convention (`// C-ABI handle`, `// custom-deleter`, `// CLI stdout`, `// pre-logger-init`).

The genuine value the full proposal would add lives in four ideas, not the rule list:

1. **Tiered enforcement** — strict zone (parsing / sync / cache / config / command-registry), light zone (UI-heavy files), exempt zone. Current enforcement is uniform with per-site exemptions.
2. **Delta gate** — fail only on **new** violations vs `origin/develop`, grandfather existing. PR #488 used this pattern manually for shell-lint; C++ violator volume makes manual rewrites infeasible.
3. **Two uncovered rules + `deviation-overdue`** — silent narrowing (clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs) and `#define ImGui` macro-alias tricks (one-liner grep). Dynamic-index bounds checks dropped from this PR (see § Out of scope — Risks §2 conceded the rule wasn't ready to gate; ASAN in the existing `Sanitizer (ASAN via MSVC)` CI job already catches the out-of-bounds runtime class). `deviation-overdue` enforces the calendar-marker `revisit=` field from the `SMATCHET_DEVIATION` grammar.
4. **Forward-only `SMATCHET_DEVIATION` comment format** — `rule / reason / owner / revisit` quad. Adds an audit-able "review this site by date X" surface (via `revisit=` + the `deviation-overdue` rule) that the existing one-line exemption comments lack.

Outcome after this lands: existing strict-zone violators are catalogued in `docs/high-integrity/baseline.md` (grandfathered); every NEW violation in a strict-zone file fails CI; the two uncovered rules + `deviation-overdue` join the existing scanner; new deviation sites use the canonical comment format; the tiered scope is named in AGENTS.md with no separate profile doc to drift.

**Effort estimate**: this plan's enforcement work is ~7 h, BUT it is now gated behind the precursor directory-reorg PR (see § Dependency above). The reorg itself is a separate plan + PR — estimate ~3-5 h of mechanical file-move + `#include`-fixup + CMake-source-list work plus dual-target build validation, dominated by blast-radius risk rather than complexity. **Combined: ~10-12 h across two PRs**, still under the full original proposal's ~20+ h but no longer the "slim ~7 h" the title implies — the flat-layout discovery is the reason. Enforcement-PR slice breakdown (commit-per-row; assumes reorg already on `develop`):

| Slice | Hours |
|---|---|
| `test-lint-rules.sh` — 3 modes (`--diff`, `--catalog`, `--refresh`), zone-glob loader, normalised-snippet hasher, `(rule, basename, hash)` set-diff | 1.5 |
| `test-lint-rules.sh` — 2 new rule checks (clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs; `\b#define\s+ImGui\b` grep) + `deviation-overdue` parser | 1.0 |
| `test-lint-rules.sh` — self-test re-reading AGENTS.md zone globs vs scanner copy; fail on drift | 0.3 |
| CI wiring — PR runner: `--diff origin/develop`; `develop` post-merge: `--catalog --refresh` + **fail-on-drift** (locked — see below) | 0.5 |
| `docs/high-integrity/baseline.md` — initial generation + commit | 0.2 |
| `AGENTS.md` § Tiered enforcement subsection + `SMATCHET_DEVIATION` grammar paragraph | 0.3 |
| `tests/bats/lint_rules.bats` — `--diff`, `--catalog`, each new rule, deviation grammar parsing, overdue detection, idempotency | 1.5 |
| `tests/fixtures/lint_rules/` — per-rule known-bad/known-good + `SMATCHET_DEVIATION` overdue/current fixture | 0.5 |
| `docs/agent-rules/process-rules.md` + `agents/core/coderabbit-triage.md` one-liner edits | 0.2 |
| Buffer (clang-tidy strict-zone-TU runtime + post-merge fail-on-drift validation) | 1.0 |

**CI baseline-refresh policy (locked)**: `develop` post-merge runs `--catalog --refresh` against a worktree and **fails the job on byte-diff vs the committed baseline file** — never auto-commits. Remediation message: `"baseline stale — run 'bash scripts/dev/test-lint-rules.sh --catalog --refresh' locally and commit"`. Trade chosen: clear contributor signal + CI stays read-only against `develop`, vs auto-commit's friendlier UX at the cost of a new CI-writes-to-develop surface. Smatchet has no other such surface today; introducing one for a snapshot file isn't earned.

## Approach

Extend the existing rule infrastructure rather than parallel-shipping new infrastructure. Five concrete edits, in order:

1. **`AGENTS.md` § Project rules § Tiered enforcement** (new subsection, ≤ 20 lines). Names the three zones by directory glob, references the existing rules (no rule restatement) and the scanner. Single source of truth. Zone globs (locked — **valid only after the precursor reorg lands**; see § Dependency):

   ```yaml
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
   - **`develop` post-merge job**: `--catalog --refresh` regenerates the baseline in CI, then `git diff --exit-code docs/high-integrity/baseline.md` enforces fail-on-drift vs the committed file. This is not a strict-zone gate against develop's own code (no diff-vs-self attempted — that would be a no-op); it's an integrity check that the committed baseline snapshot still matches reality. Remediation on failure: contributor runs `--catalog --refresh` locally and commits. Identical contract is restated at Context § CI baseline-refresh policy and at §3 below.
   - **`--catalog` mode** dumps the current violator set to `docs/high-integrity/baseline.md` grouped by `(rule × zone)` — see §3 below for the exact format.
   - **Baseline file is a human-reading snapshot, not a gate input**. The gate re-computes on every run; `docs/high-integrity/baseline.md` exists only to make the grandfathered set visible to humans.
   - **Three new rules** — (a) clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs, (b) a one-liner `\b#define\s+ImGui\b` grep, (c) `deviation-overdue` (parses `SMATCHET_DEVIATION` comments, fails when `revisit=` is a calendar marker that has passed). Dynamic-bounds `operator[]` heuristic dropped — see § Out of scope.
3. **`docs/high-integrity/baseline.md`** — auto-generated snapshot, never a gate input (gate truth = live scan vs `origin/develop` per §2). Refresh runs on `develop` post-merge via `bash scripts/dev/test-lint-rules.sh --catalog --refresh`. Hand-edits are disallowed; CI re-runs the catalog and fails the post-merge job if the committed file diverges from a freshly-generated one (catches accidental edits).

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

**Precursor (separate PR + plan-doc, blocking — see § Dependency)**: `docs/plans/shipped/source-core-dir-reorg.md` (new plan) + the reorg PR itself (file moves across `Source_Core/src/**`, `Plugins/Mcp/**`, every dependent `#include`, CMake source lists). None of the rows below start until that lands on `develop`.

1. `AGENTS.md` — add § Tiered enforcement subsection under § Project rules (≤ 20 lines; matches Approach §1) AND the `SMATCHET_DEVIATION` grammar paragraph (per Approach §4). Both edits land together so AGENTS.md is the single source of truth for both zone scope and deviation syntax.
2. `scripts/dev/test-lint-rules.sh` — add `--diff` mode (default for `test-all.sh` invocation), `--catalog` + `--refresh` modes, the three new rule checks (clang-tidy `cppcoreguidelines-narrowing-conversions` on strict-zone TUs, `\b#define\s+ImGui\b` grep, `deviation-overdue` parser), zone-tier classification, and the rule-id contract per § Rule-id contract below (existing rules renamed to `no-printf-stderr` + `no-raw-new`).
3. `docs/high-integrity/baseline.md` (new) — initial dump from `--catalog`; lives under `docs/high-integrity/` — a generated tracking artefact, not a plan (kept out of both design/ and backlog/, which hold plans).
4. `tests/bats/lint_rules.bats` (new) — bats coverage for `--diff` mode, `--catalog` mode, and each of the three new rules (one bats per rule, one all-clean fixture). Pattern mirrors `tests/bats/shell_lint.bats` from #488.
5. `tests/fixtures/lint_rules/` (new) — per-rule known-bad + known-good fixtures.
6. `docs/agent-rules/process-rules.md` — single-line note under § Plan-doc family pointing at the new tiered-enforcement subsection of AGENTS.md (so plan authors are reminded to declare their zone).
7. `agents/core/coderabbit-triage.md` — one-line override-table entry recommending `SMATCHET_DEVIATION` for new strict-zone exemption requests rather than ad-hoc inline comments. No mandatory enforcement; just routing guidance.
8. `.github/workflows/build-and-test.yml` (existing — the repo has no dedicated lint workflow today; the existing `windows-msvc` job's "Run non-UI bucket-A tests" step at line ~110 already runs `bash scripts/dev/test-*.sh` lints alongside other bucket-A scripts). Two edits:
   - **PR + push runs** — append `bash scripts/dev/test-lint-rules.sh --diff origin/develop` to the existing "Run non-UI bucket-A tests" step. Same trigger as the surrounding lints; failure blocks merge via the existing required-check on `windows-msvc`.
   - **`develop` post-merge — new job in the same workflow** gated on `if: github.event_name == 'push' && github.ref == 'refs/heads/develop'`. Runs `bash scripts/dev/test-lint-rules.sh --catalog --refresh` then `git diff --exit-code docs/high-integrity/baseline.md`. Non-zero exit = "baseline stale" remediation message + job fails. Never auto-commits — CI stays read-only against `develop`. New job because the existing `windows-msvc` runs on every PR and the refresh-and-diff is develop-only.

## Rule-id contract

Every rule emitted by `test-lint-rules.sh` carries a stable kebab-case id. The id is the linkage between scanner output, the catalog file's section headers, and the `SMATCHET_DEVIATION(rule=<id>; ...)` suppression key. Locked id set for this PR:

| Rule id | Source | Strict-zone gate? |
|---|---|---|
| `no-printf-stderr` | existing scanner rule (currently emitted as a free-text description; rename to this id) | yes |
| `no-raw-new` | existing scanner rule (same rename) | yes |
| `narrowing-conversions` | new — clang-tidy `cppcoreguidelines-narrowing-conversions` mapped to this id | yes |
| `define-imgui` | new — `\b#define\s+ImGui\b` grep | yes |
| `deviation-overdue` | new — `SMATCHET_DEVIATION` parser fires when calendar-marker `revisit=` has passed | yes |

Convention for future rules: lowercase kebab-case, ≤ 24 chars, namespaced when ambiguous (`cppcoreguidelines-*` for clang-tidy-derived ids when the clang-tidy name itself is already kebab-case and clear). New ids land via the same plan-doc grill loop, never silently.

## Existing utilities reused

- `scripts/dev/test-lint-rules.sh` (shipped #468, ~40 LoC) — existing rule-loop, exemption-comment allowlist, file-walker. Reused as the foundation; this plan extends it rather than forking.
- `scripts/dev/test-shell-lint.sh` pattern — bats fixture layout, `--target` flag, header doc, `SMATCHET_SKIP_*` env-bypass convention.
- `scripts/dev/test-all.sh` discovery glob (`test-*.sh`) — auto-runs the extended script at pre-push gate.
- `Sanitizer (ASAN via MSVC)` CI job (build-and-test.yml:313) — already covers part of the Pillar 3 contract (out-of-bounds memory); no new CI *workflow file* needed (this plan adds steps + one job to the existing workflow, not a new file — see Files §8).
- `cppcheck` + `clang-tidy` PostToolUse hooks — already enforce most of the rule list at C++-edit time.
- Existing inline exemption-comment allowlist (`// C-ABI handle`, `// custom-deleter`, `// CLI stdout`, `// pre-logger-init`) — accepted in perpetuity; `SMATCHET_DEVIATION(rule)` is a forward-only superset.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — the script runs at pre-push, not on the UI thread. clang-tidy on strict-zone TUs adds ~5-15 s to the local lint step; offset against the per-PR runner time the new CI workflow would have added.
- **Pillar 2 (UI never blocks > 100 ms)**: N/A — same.
- **Pillar 3 (never crash)**: **positive marginal impact** — the three new rules (`narrowing-conversions`, `define-imgui`, `deviation-overdue`) close gaps in the current crash-prevention enforcement. Narrowing in particular is a known footgun class (silent int truncation in tracker payload parsers); clang-tidy catches the common shapes. `deviation-overdue` is an indirect contribution — it forces accumulated deviations to be reviewed on calendar cadence rather than rotting.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

N/A (enforcement PR) — diff touches only `AGENTS.md`, `scripts/dev/`, `docs/`, `tests/bats/`, `tests/fixtures/`, `.github/workflows/build-and-test.yml`, `agents/core/coderabbit-triage.md`. The three new rule checks read `Source_Core/` but don't modify it. **The precursor reorg PR is a different story** — it moves `Source_Core/` files wholesale, so the perf-gate applies there and that plan-doc carries its own perf-gate section (a pure file-move with no logic change should be perf-neutral, but the reorg PR validates rather than asserting).

## Risks / non-goals

- **Marketing-weight loss vs full proposal**. The slim version doesn't have a standalone "High-Integrity C++ Profile" doc; the rules live inside AGENTS.md. Contributors browsing the design folder will not see a flag-planting doc that says "this is a high-integrity codebase." Mitigation: the AGENTS.md subsection is title-cased clearly + the baseline doc under `docs/high-integrity/` is hand-linked from the subsection. Accepted as a marketing trade for the one-source-of-truth structural win.
- **Dynamic-bounds rule dropped from this PR** (see § Out of scope). The heuristic's false-positive rate against `for (i; i<vec.size(); ...) v[i]` patterns was too high to gate; a "light-zone warn" that never fails CI was scanner code without behaviour change. ASAN in the existing `Sanitizer (ASAN via MSVC)` CI job already catches the out-of-bounds runtime class. Promotion path: a follow-up PR with a parameter-driven-index heuristic scoped to strict-zone files, evidence-driven not pre-scheduled.
- **`SMATCHET_DEVIATION` not made mandatory** means existing exemption sites won't carry a revisit date. The audit-the-deviation surface this would enable stays partial. Mitigation: forward-only is a deliberate choice to avoid the rewrite-pass cost (estimated ~50 sites × 2 min = ~2 h of zero-behaviour-change churn). The mandatory form can be promoted later if review experience shows it's worth the rewrite.
- **Baseline file as a gate input — not the design** (clarification, not a risk). The grill superseded the original "baseline as gate truth" framing: gate truth = live scan on `HEAD` vs live scan on `origin/develop`, computed every run; `docs/high-integrity/baseline.md` is a human-reading snapshot only. Drift between the file and the live state is normal between refreshes — `develop` post-merge runs `--catalog --refresh` with fail-on-drift so the committed snapshot can never silently diverge from reality on `develop`. PR-time staleness is a non-issue because the gate doesn't consult the file.
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
- **Phase 4 dedicated CI workflow file** (`.github/workflows/high-integrity-cpp.yml`). Distinct from Files §8, which adds steps + one job to the *existing* `build-and-test.yml` — a new *job* in an existing workflow is not a new *workflow file*. A dedicated workflow file is only worth it if the pre-push gate is bypassed (e.g. someone force-pushes). Decide on evidence.
- **Phase 5 promotion ladder** (5 rules in fixed order). Each rule is a separate ~30-min PR when it earns its way in based on observed false-positive rate + bug-catch payoff. Pre-scheduling the order predicts a future we can't see.
- **Phase 6 PR-template checkbox**. Smatchet doesn't gate on checkboxes; merge-gates.sh + CodeRabbit own the merge contract.
- **Phase 6 CodeRabbit path-specific instructions**. CR already cross-references AGENTS.md rules in its reviews; adding `.coderabbit.yaml` path instructions adds maintenance for marginal lift.
- **Mandatory `SMATCHET_DEVIATION` migration**. Forward-only is the explicit choice; an existing-sites rewrite is a separate cleanup PR if/when reviewing experience says it pays off.
- **Categorization of every baseline entry as `fix-now / deviation / needs-refactor / false-positive`**. Auto-derive from `(rule × zone)` and skip the manual-triage workflow.

## Implementation log

- Precursor `source-core-dir-reorg` (#505) landed first — see that plan; this work branched off the reorganized tree.
- `8658993` · scanner: rule-ids, zone_of, SMATCHET_DEVIATION parser, --diff/--catalog/--scan-file/--full/--selftest modes.
- `fdbd8b2` · subprocess-free hot path (~10× faster); `--root` fixes the --diff base scan; narrowing opt-in; AGENTS.md § Tiered enforcement + SMATCHET_DEVIATION grammar.
- _(baseline)_ · byte-deterministic `--catalog` (no timestamp/sha) + initial `docs/high-integrity/baseline.md` (3 grandfathered `define-imgui`).
- _(bats)_ · `tests/bats/lint_rules.bats` (12) + 6 fixtures + `test-lint-rules-bats.sh` wrapper; `--diff=`/`--scan-file=` forms (shell-lint clean).
- _(ci)_ · `build-and-test.yml` PR delta gate + bats in bucket-A; develop post-merge baseline-drift job; process-rules.md + coderabbit-triage.md hooks.
- _(narrowing-ci)_ · `scan_narrowing`: Windows-path-safe parse (greedy match tolerates the `C:\` drive-letter colon `cut -d:` truncated) + first-party/`ThirdParty` filter + pipefail fix (a clean TU no longer aborts the strict scan under `set -e -o pipefail`); new `high-integrity-narrowing` develop-post-merge Windows job (PCH-free clang-cl db, configure-only); fixed `Sync/TicketSyncService.cpp:262-263` `size_t→ptrdiff_t` — the only first-party narrowing. Narrowing now gated (enforced at 0 first-party).
- _(wide-rules)_ · promote `no-raw-new` + `deviation-overdue` from strict-zone-only to **first-party-wide absolute** — enforced at 0 across `Source/Core` + `Source/Plugins` + `Source/Standalone` (the `comment_audit.py` SWEEP_ROOTS; ThirdParty + tests excluded), not just the strict zone. New `compute_wide_violations` + `--scan-wide` mode + an always-on block in `--diff`. Both measured 0 first-party today → absolute (no grandfathering); exemption markers + `SMATCHET_DEVIATION` still suppress. `no-printf-stderr` + `define-imgui` stay strict-only (legit first-party uses: CLI stdout, the ImGui localization alias). +5 bats.

## Deviations from plan

- **`narrowing-conversions` was opt-in / catalogue-only; now gated on a dedicated Windows job** (resolved — see § Implementation log `(narrowing-ci)`). Original blocker: clang-tidy cannot parse the project's MSVC `compile_commands.json` — it errors on the MSVC PCH (`not a valid PCH file`) and can't resolve `<string>` (no clang stdlib paths). Resolution: the `high-integrity-narrowing` job (windows-2022, develop post-merge) configures a **PCH-free clang-cl** db (`ninja-iter-clang -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`) — the unbuilt `cmake_pch.cxx.pch`, not the MSVC-vs-clang split, was the real wall — and fails on any **first-party** strict-zone narrowing. Vendored `ThirdParty/` headers (e.g. `stb_image.h` compiled into a strict TU via `STB_IMAGE_IMPLEMENTATION`) are filtered in `scan_narrowing`. Still excluded from the ubuntu `--diff` delta gate (clang-cl is Windows-only; the base worktree has no clang db). The 4 grep/parser rules (`no-printf-stderr`, `no-raw-new`, `define-imgui`, `deviation-overdue`) remain the cross-platform hard-gated core.
- **Baseline body carries no timestamp/snapshot-sha.** The locked layout showed a `Snapshot: <sha> · <ts>` line, but the develop post-merge fail-on-drift uses `git diff --exit-code`, which a per-run timestamp breaks. Dropped the volatile header so the file is a pure function of the violation set (byte-identical when unchanged); "when it last changed" lives in git history.
- **`--root` flag added** (not in the plan) so the `--diff` base scan runs the *current* scanner against the base worktree — the base tree (origin/develop) ships an older scanner, and the naive `cd $0/../..` jumped back to HEAD (base==head → false PASS). Required for a sound delta gate.
- **`--diff=` / `--scan-file=` value forms added** to satisfy the shell-lint flag-parity rule (the scanner is itself shell-linted).

## Verification (actual)

- **Scanner unit behaviour**: `--scan-file` fires all 4 grep/parser rules on crafted fixtures; `SMATCHET_DEVIATION` suppresses the matching rule on the next line; `deviation-overdue` fires on a passed `revisit=2020-01-01` and not on `2099-12-31` — **PASS**.
- **Delta gate**: injected `new Foo()` + `std::printf` into a strict-zone file → `--diff origin/develop` **FAILs** with the exact new triples; after revert → **PASS**. Grandfathers existing (3 `define-imgui`). **PASS**.
- **Bats** (`tests/bats/lint_rules.bats`): 12/12 **PASS** (rules, deviation, --selftest, --catalog format + byte-determinism, --diff PASS/FAIL via baseline stub).
- **`--selftest`**: AGENTS.md zone globs ⇄ scanner copy in sync — **PASS**.
- **Shell-lint**: `test-lint-rules.sh` + `test-lint-rules-bats.sh` both clean — **PASS**.
- **`--catalog` determinism**: two consecutive `--refresh` runs byte-identical — **PASS** (the develop drift job depends on this).
- **Build gate**: N/A — bash + clang-tidy only; no C++/Source_Core modified.
- **Not run**: live CI develop-post-merge drift job + the new `high-integrity-narrowing` job (both validated locally; first live run is the next develop merge).

### Follow-up: narrowing CI gate (2026-05-31)

- **PCH-free clang db**: `cmake --preset ninja-iter-clang -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON` yields a clang-cl `compile_commands.json` clang-tidy parses with **0 `clang-diagnostic-error`** over the 100 strict TUs — the unbuilt `cmake_pch.cxx.pch` (not the MSVC-vs-clang split the original deviation blamed) was the real wall.
- **Findings**: 118 unique narrowing warnings → 116 in vendored `ThirdParty/stb/stb_image.h` (compiled into `Persistence/SmatchetImageTextureCache.cpp` via `STB_IMAGE_IMPLEMENTATION`; now filtered) + 2 first-party `Sync/TicketSyncService.cpp:262-263` (`size_t→difference_type` iterator offset), both fixed.
- **Scanner**: `SMATCHET_LINT_NARROWING=1 --full` reports **0 first-party narrowing**, full TU sweep, clean exit (pipefail fix verified — earlier a clean TU aborted the scan mid-loop = false PASS). **PASS (local)**.
- **Live CI**: `high-integrity-narrowing` (windows-2022) first validates on the next develop merge — fail-forward, matching baseline-drift's post-merge model. Serial clang-tidy (~minutes); parallelising `scan_narrowing` is a tracked follow-up.

### Follow-up: `no-raw-new` + `deviation-overdue` promoted first-party-wide (2026-05-31)

- **Scope**: both rules now enforced at 0 across ALL first-party C++ (`Source/Core`, `Source/Plugins`, `Source/Standalone` — the `comment_audit.py` SWEEP_ROOTS; ThirdParty + tests excluded), not just the strict zone. `compute_wide_violations` + `--scan-wide` mode + an always-on absolute block in `--diff` (no base scan — HEAD-only).
- **Why absolute, not delta**: measured 0 across all first-party today, so there is nothing to grandfather; the user's intent is "= 0 everywhere". Exemption markers (`// C-ABI handle` / `// custom-deleter` / `// pimpl`) and `SMATCHET_DEVIATION(rule=...)` still suppress.
- **Why only these two**: `no-printf-stderr` (Standalone CLI stdout) and `define-imgui` (the ImGui localization-alias macro in Ui) have legitimate first-party uses outside the strict zone, so they stay strict-only.
- **Verified**: `--scan-wide` on the real tree = 0; fires `no-raw-new` on a non-strict Ui fixture; ignores ThirdParty + `tests/` + an exemption-marked raw new; `--diff origin/develop` emits the wide PASS line; full gate rc=0. 17/17 lint bats (5 new), scanner shellcheck-clean. **PASS**.
