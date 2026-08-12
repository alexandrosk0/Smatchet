# Plan — Solo-workflow review gate (drop required approval, codify protection)

> **Slug**: `solo-merge-review-gate` (matches this file's basename without `.md`).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template.

## Context

**Re-scoped 2026-06-02 (grill against live state):** the original "deadlock" premise is **stale**. Live `develop` protection already reports `required_pull_request_reviews: null` (0 required reviews) — the review requirement was dropped at some point via the GitHub UI, which is why PRs merge cleanly today. So the plan's *core change* (set the count to 0) is **already done in the live state**; what remains valid is the **codify-so-it-can't-drift** half (a reproducible script + a config block + an ADR). The historical premise is kept below for the rationale.

*(Historical premise.)* `develop` branch protection had required **1 approving review** (`required_approving_review_count: 1`, `require_code_owner_reviews: false`, `enforce_admins: false`). On a solo repo that was a **deadlock**: GitHub forbids approving your own PR, so the sole maintainer (`alexandrosk0`) could never satisfy it — every PR sat in `mergeStateStatus: BLOCKED` even when CI + CodeRabbit were fully green (observed on #747).

This requirement also **contradicts the repo's own merge model**. `agents/scripts/core/merge-gates.sh:460` treats `reviewDecision ∈ {APPROVED, NONE/null}` as a **pass** — the autonomous merge-watcher and the orchestrator ship-loop deliberately do **not** require a human approval; CodeRabbit (hard-blocking) + the required CI checks are the real gates. So GitHub branch protection is enforcing a gate the harness already decided it doesn't want, and the only ways past it today are admin-merge (manual, every PR) or a second identity (none exists).

Two secondary facts shape the fix:
- **`enforce_admins: false`** — the maintainer can already bypass the review via admin merge, so the "1 review" provides essentially **no real protection today**; it only adds friction.
- **Branch protection is not codified.** No in-repo script sets it (`setup-locks-ruleset.sh` covers the file-lock ruleset, not branch protection); the config lives only in GitHub's UI/API and can drift silently.

**Intended outcome — one sentence:** after this lands, `develop` requires **0 approving reviews** (CodeRabbit + the four required CI contexts remain the gates), the desired protection state is **codified** in a reproducible script sourced from `project.config.json`, and the decision + its revisit condition are recorded in an ADR.

## Approach

Align GitHub branch protection with the harness's existing merge model — **review not required** — while keeping every other gate intact, and codify the result so it can't drift.

**The change:** set `required_approving_review_count: 0` on `develop` (keep the `required_pull_request_reviews` object so `dismiss_stale`/`code_owner` knobs stay available; do not DELETE it). Leave untouched: the four required status contexts (`Test-delta gate`, `Windows + MSVC`, `Windows + MSVC (Smatchet light …)`, `Shell lint`), `strict: false`, `enforce_admins: false`, and the entire CodeRabbit merge-gate path. CodeRabbit stays hard-blocking — this removes a *human-approval* gate that is both impossible to satisfy solo and already redundant with the bot review, **not** any correctness gate.

**Codify it:** add `agents/scripts/core/setup-branch-protection.sh`, an idempotent script that `PUT`s the full desired protection object for `develop`, reading the required-context list and review count from a new `branch_protection` block in `project.config.json` (same config-as-code pattern as the rest of the portable layer — a reused project rewrites the config, the script re-targets). This makes the live state reproducible and reviewable, and closes the silent-drift gap.

**Record the trade-off:** an ADR (`docs/adr/`) capturing *why* a normally-important gate was removed, the safety argument (CR + CI remain; admin bypass already existed), and the **revisit trigger** — if the repo ever takes outside contributions, re-introduce review enforcement for non-maintainer PRs (via a ruleset / CODEOWNERS bypass list), because the "solo, trusted author" premise no longer holds.

## Files to modify

1. `agents/scripts/core/setup-branch-protection.sh` (new) — idempotent `gh api -X PUT repos/<owner>/<repo>/branches/develop/protection` with the desired object built from `project.config.json`: `required_status_checks.contexts` = the four required names, `strict: false`; `enforce_admins: false`; `required_pull_request_reviews.required_approving_review_count: 0`; `restrictions: null`. Passes `test-shell-lint.sh` (5 rules). `--dry-run` prints the JSON without applying.
2. `project.config.json` (edit) — add a `branch_protection` block: `{ "branch": "develop", "required_contexts": ["Test-delta gate", "Windows + MSVC", "Windows + MSVC (Smatchet light — AI/Whisper/MCP off)", "Shell lint (shellcheck)"], "required_review_count": 0, "enforce_admins": false, "strict": false }`. Single source for the script.
3. `docs/adr/NNNN-solo-no-required-review.md` (new) — the decision: drop the human-approval requirement for the solo workflow; safety argument; revisit-on-external-contributors trigger. Per `agents/_shared/skills/grill-with-docs/ADR-FORMAT.md`.
4. `docs/agent-rules/ci-required-check-pattern.md` (edit) — one-line cross-link: the required *status checks* documented here are gated by `setup-branch-protection.sh`; the review-count decision lives in the ADR.

**Impl-time action (not a file):** run `bash agents/scripts/core/setup-branch-protection.sh` once with an admin-scoped token to apply the new state. Surfaced as residue (needs `repo`-admin auth; the maintainer runs it or authorizes the run).

## Existing utilities reused

- `agents/scripts/core/merge-gates.sh` (`reviewDecision ∈ {APPROVED, NONE}` → pass, line 460) — the existing harness merge model this change aligns GitHub to; unchanged.
- `project.config.json` config-as-code seam + `scripts/dev/project-config.sh` — the script sources the new `branch_protection` block, never hardcodes.
- `agents/scripts/core/setup-locks-ruleset.sh` — the idempotent `gh api` setup-script precedent the new script mirrors.
- `agents/scripts/core/test-shell-lint.sh` + the `guards.doc_validation` group — gate the new script + ADR links.
- `agents/_shared/skills/grill-with-docs/ADR-FORMAT.md` — the ADR shape.

## UX Pillar callouts

- **Pillar 1–4**: no runtime impact — repo config + one setup script + docs. Zero product code.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`)

N/A — no Source/Core code; no C++. Adds a shell script + project.config.json + docs. Not pure-docs (script + json deny-listed), but no compiled change, so build/ctest/perf gates are no-ops; verification is shell-lint + the live-state check.

## Risks / non-goals

**Risks:**
- **Security: removes a human-review gate.** This is a deliberate reduction. Mitigation / why it's acceptable: (a) CodeRabbit stays hard-blocking and the four required CI contexts remain, so no *correctness* gate is lost; (b) the review was **impossible to satisfy** solo (self-approval forbidden) — a dead requirement, not active protection; (c) `enforce_admins: false` already let the maintainer admin-merge past it, so it added friction, not safety. **Revisit trigger (in the ADR):** the moment outside contributions are accepted, re-enforce review for non-maintainer PRs — the "solo trusted author" premise that justifies this no longer holds.
- **Drift / wrong-state apply.** A bad `PUT` could loosen more than intended. Mitigation: `--dry-run` prints the exact object first; the script writes only the fields in the config block; verification re-reads the live state and asserts the four required contexts are still present and `enforce_admins`/`strict` unchanged.
- **Token scope.** Changing protection needs `repo`-admin auth. Mitigation: surfaced as impl residue; the maintainer runs the apply step. CI never applies protection (read-only there).

**Non-goals:**
- Touching CodeRabbit gating, the required CI contexts, `enforce_admins`, or `strict` — only the review count moves (1 → 0).
- Adding external-contributor rulesets / CODEOWNERS bypass lists — future work, gated on the repo opening to outside PRs (named in the ADR revisit trigger).
- Auto-merging open PRs (e.g. #747) — out of scope; this only removes the wall, it does not merge anything.

## Verification

- **Bucket A / E**: N/A — no code.
- **Live-state check**: after `setup-branch-protection.sh`, `gh api repos/<owner>/<repo>/branches/develop/protection/required_pull_request_reviews --jq .required_approving_review_count` returns `0`; `…/required_status_checks --jq .contexts` still lists the four required names; `…/protection --jq .enforce_admins.enabled` still `false`.
- **End-to-end**: a fresh CR-clean, CI-green docs PR reaches `mergeStateStatus` other than `BLOCKED` (i.e. `CLEAN`/`UNSTABLE`) with no approval — confirming the deadlock is gone.
- **Idempotence**: re-running `setup-branch-protection.sh` is a no-op (same state in → same state out); `--dry-run` matches the applied object.
- **Shell lint**: `test-shell-lint.sh` on the new script.
- **Doc integrity**: ADR + cross-link resolve in the `guards.doc_validation` group.
- **Build gate**: N/A — no compile.
- **Manual residue**: the one-time apply needs admin auth — named above, run by the maintainer; not silent.

## Out of scope (flagged, not designed)

- **External-contributor review policy** — re-enabling review enforcement for non-maintainer PRs once the repo opens up (ADR revisit trigger).
- **Codifying the rest of branch/ruleset config** (the file-lock ruleset already has `setup-locks-ruleset.sh`; other repo settings remain UI-managed) — broaden config-as-code coverage separately if desired.
- **Bot-approver identity** — an alternative to count=0 (a second identity approves); heavier, not pursued while solo.

## Implementation log

- Wave-1.3 of `agentic-harness-campaign`. `agents/scripts/core/setup-branch-protection.sh` (new) — idempotent `gh api -X PUT …/branches/<branch>/protection`, body built from `project.config.json` § `branch_protection`, `--dry-run` prints the object. `project.config.json` + `project.config.schema.json` gain the `branch_protection` block (branch + 4 required contexts + `required_review_count: 0` + `enforce_admins:false` + `strict:false`). `docs/adr/0013-solo-no-required-review.md` (new) records the decision + revisit-on-external-contributors trigger. `docs/agent-rules/ci-required-check-pattern.md` cross-links the script + ADR.

## Deviations from plan

- **Core change was already live** (grill finding): `develop` already had `required_pull_request_reviews: null` (0 reviews), so the count-flip was not needed — this PR only **codifies** the existing state. The live-apply is therefore an idempotent *confirmation*, not a fix. § Context rewritten to reflect this.
- ADR number is 0013 (next free), not the plan's placeholder `NNNN`.

## Verification (actual)

- `setup-branch-protection.sh --dry-run` emits the exact desired protection object (verified: 4 contexts, review_count 0, enforce_admins false, restrictions null). `shellcheck` clean. config + schema parse + round-trip OK. `test-docs` 7/7. **Residue (maintainer):** run `bash agents/scripts/core/setup-branch-protection.sh` once with a repo-admin token to confirm the live state matches the codified object (no-op if already aligned).
