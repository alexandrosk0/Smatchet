# Plan — branch-protection config completeness (stop the full-object PUT from silently clearing live gates)

> **Slug**: `branch-protection-config-completeness` (matches this file's basename without `.md`).
>
> **Status**: `active`
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

PR #2033 flipped `project.config.json` § `branch_protection` → `enforce_admins: true` but could not apply it (that session's `gh` login lacked repo-admin). This session applied it — via the **`enforce_admins` sub-resource** (`POST .../branches/develop/protection/enforce_admins`), *not* via `agents/scripts/core/setup-branch-protection.sh`, because a pre-apply diff of the script's dry-run body against the live protection object surfaced a second, unintended delta:

| key | live (before) | script's PUT body | effect of running the script |
|---|---|---|---|
| `enforce_admins` | `false` | `true` | intended |
| `required_conversation_resolution` | **`true`** | **absent** | **silently reset to `false`** |

`PUT /repos/{owner}/{repo}/branches/{branch}/protection` is a **full-object replace**, not a patch: every optional protection flag omitted from the body is reset to its API default (`false`). The script's body builder ([`setup-branch-protection.sh:62`](../../../agents/scripts/core/setup-branch-protection.sh#L62)) emits exactly four top-level keys — `required_status_checks`, `enforce_admins`, `required_pull_request_reviews`, `restrictions` — so every other flag GitHub supports is a silent removal on each apply. The script header even asserts the opposite invariant: *"Idempotent: a PUT replaces the full protection object; re-running converges."* It converges — on a state that is **not** the live state.

`required_conversation_resolution` is the load-bearing one. It is the server-side gate both [`merge-gates.sh`](../../../agents/scripts/core/merge-gates.sh) and [`scripts/dev/pr-blocked-why.sh`](../../../scripts/dev/pr-blocked-why.sh) probe as ground truth for the silent-`BLOCKED` cause, and it counts **bot** review threads that the merge-gate poller deliberately filters out — so it is strictly *additive* protection the harness has no substitute for. Clearing it would have widened the merge surface in the same breath as tightening admin enforcement.

The drift is **structural, not incidental**: [`project.config.schema.json:89`](../../../project.config.schema.json#L89) declares `branch_protection` with `additionalProperties: false` and only five properties, so the config *cannot express* the key — every future run of the script re-drops it. Live evidence that no full-object PUT has ever run against this repo: the live `required_contexts` array is in strict chronological-append order, not the config's order.

**After this lands**: `project.config.json` § `branch_protection` is a *complete* statement of the desired protection object, the script PUTs every flag it states, and a bats suite fails if a flag present in the config stops reaching the PUT body.

## Approach

Make the config the complete desired state and make the script a faithful transcription of it, in three coupled edits plus a regression test.

1. **Config states every flag explicitly** — add the REST endpoint's optional protection flags to `project.config.json` § `branch_protection` at their current live values (`required_conversation_resolution: true`; the rest at their live `false`). Stating a flag that happens to be `false` is not noise here: because omission ≠ no-op on this endpoint, an unstated flag is an *invisible* assertion of `false`. Stating it converts a silent removal into a reviewable config diff.
2. **Schema admits them** — add matching `properties` entries to `project.config.schema.json` § `branch_protection` (mandatory: `additionalProperties: false` rejects the config otherwise, so this is not optional polish).
3. **Body builder emits them** — extend the Python heredoc in `setup-branch-protection.sh` to project each config flag into the PUT body, with a header comment naming the full-object-replace drop class so the next reader doesn't re-introduce the omission.
4. **A test pins it** — `tests/bats/setup_branch_protection.bats` asserts the `--dry-run` body carries `required_conversation_resolution: true` (and the other flags), so a future config key added without a matching builder line fails a gate instead of silently vanishing on the next apply.

Trade-off named: the alternative — teach the script to `GET` the live object and merge the config over it — preserves unknown flags automatically but destroys the config-as-code property (live becomes partly authoritative, and drift stops being visible in a diff). Explicit-and-complete config is the choice; the bats suite is what keeps it complete.

## Files to modify

1. [`project.config.json:54`](../../../project.config.json#L54) — § `branch_protection`: add `required_conversation_resolution: true` + the remaining optional protection flags at their live values, plus a `_doc_optional_flags` note stating *why* false-valued flags are written out.
2. [`project.config.schema.json:89`](../../../project.config.schema.json#L89) — § `branch_protection` `properties`: add the new boolean keys (+ the `_doc_optional_flags` string). Required because `additionalProperties: false`.
3. [`agents/scripts/core/setup-branch-protection.sh:62`](../../../agents/scripts/core/setup-branch-protection.sh#L62) — body builder: emit the new flags; amend the `# Idempotent:` header comment (line 24) to state the full-object-replace drop class explicitly.
4. `tests/bats/setup_branch_protection.bats` — **new**. Dry-run body assertions (see § Verification).
5. `agents/scripts/core/test-setup-branch-protection-bats.sh` — **new**. Mandatory wrapper; a wrapper-less `.bats` suite trips the orphan-bats gate ([`scripts/dev/pre-ship.sh:495`](../../../scripts/dev/pre-ship.sh#L495)) and `scripts/dev/test-all.sh` discovers only via the `test-*.sh` glob.

## Existing utilities reused

- `resolve_repo` — [`agents/scripts/core/lib/resolve-repo.sh`](../../../agents/scripts/core/lib/resolve-repo.sh) — already sourced by the script; the `$REPO` override is the test seam the bats suite drives so no test ever touches a real repo.
- `test-resolve-repo-bats.sh` — [`agents/scripts/core/test-resolve-repo-bats.sh`](../../../agents/scripts/core/test-resolve-repo-bats.sh) — copied verbatim as the wrapper shape (bats-missing → rc 2 + `Passed: 0  Failed: 0`; TAP counting; zero-run floor guard).
- `tests/bats/resolve_repo.bats` — [`tests/bats/resolve_repo.bats`](../../../tests/bats/resolve_repo.bats) — style reference for the stub-binary-on-narrowed-`PATH` pattern.
- `--dry-run` flag — [`setup-branch-protection.sh:45`](../../../agents/scripts/core/setup-branch-protection.sh#L45) — the existing no-side-effect seam; the suite needs no new affordance in the script under test.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — this plan adds keys and a test; it extracts nothing and splits no over-cap file.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — agentic-shell + JSON config only; no `Source/` code, no frame-path work.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — nothing runs on the UI thread; the touched script is a one-shot admin CLI.
- **Pillar 3 (never crash)**: no impact on the product binary. The adjacent *safety* property is a merge-gate one: this change removes a path by which running a maintenance script would silently weaken a server-side gate.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no user-facing surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

`N/A — the diff is agentic-shell + JSON config + bats only; it touches no file under Source/Core/.` All five gates (PR-fast CI, Pillar 2 scanner, dispatcher drain, bucket-E visible-cue harness, marker inventory) are therefore N/A: no scenario maps to the diff, no sync I/O is introduced, `MainThreadDispatcher::Drain()` is untouched, no new stall path exists, and no `SMATCHET_UI_PERF_SCOPE` markers are added.

## Risks / non-goals

- **Risk — the enumerated flag list is itself incomplete.** GitHub can add a protection flag tomorrow; a flag neither stated nor emitted is still dropped by the next PUT. *Mitigation*: the bats suite pins the flags we know about, and the amended header comment tells the next reader the drop class exists. *Accepted residue*: full coverage would require the GET-and-merge design this plan explicitly rejects (see § Approach trade-off).
- **Risk — someone runs the script before this lands** and clears `required_conversation_resolution` on `develop`. *Mitigation*: recovery is one sub-resource call (`gh api -X PUT .../protection/required_conversation_resolution`); the incident is detectable because `pr-blocked-why.sh` probes the flag. *Accepted* — the window is this PR's lifetime.
- **Risk — schema edit rejects an existing config.** *Mitigation*: adding `properties` under `additionalProperties: false` only ever *widens* what validates; no existing key changes meaning. Covered by the doc-validation suite.
- **Non-goal — changing any live protection value.** This PR states the current live values; it does not tighten or loosen a gate. `enforce_admins: true` was already applied out-of-band this session.
- **Non-goal — teaching the script to reconcile GET → merge → PUT.** Named and rejected above.
- **Non-goal — auditing `setup-locks-ruleset.sh`** for the same full-object-replace class. Flagged in § Out of scope.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ changes; the equivalent logic coverage lives in the bats bucket below.
- **Bucket A (bats, new)**: `bash agents/scripts/core/test-setup-branch-protection-bats.sh`. Assertions, all against `--dry-run` with `REPO` stubbed so no network call fires: (a) the body parses as JSON; (b) `.required_conversation_resolution == true`; (c) `.enforce_admins == true`; (d) `.required_pull_request_reviews.required_approving_review_count == 0`; (e) `.required_status_checks.contexts` is set-equal to the config's `required_contexts`; (f) **the completeness invariant** — every boolean key in the config block appears in the PUT body (the assertion that catches a future config key added without a builder line).
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no product binary involved.
- **Build gate**: N/A — pure agentic-shell + config + docs diff; `bash agents/scripts/core/is-pure-docs-diff.sh` does **not** classify it as pure-docs (it touches `.sh` + `.json`), so the shell-lint and agentic self-test lanes are the binding CI gates rather than the dual-target build.
- **Shell lint**: `bash scripts/dev/test-shell-lint.sh` green over the new wrapper (shellcheck).
- **Orphan-bats gate**: `bash agents/scripts/core/test-orphan-bats.sh` green — proves the new `.bats` has its wrapper.
- **Config-schema validation**: the `project.config.json` ↔ `project.config.schema.json` validator green (run via `bash scripts/dev/pre-ship.sh`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Live re-verification (post-merge, one command, non-mutating)**: `bash agents/scripts/core/setup-branch-protection.sh --dry-run` diffed against `gh api repos/<slug>/branches/develop/protection` — expected delta after this lands: **none**. This is the check that would have caught the drift originally.
- **Manual residue**: none. Every bullet above is a CLI invocation.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **`setup-locks-ruleset.sh` full-object audit** — the sibling script owns the `refs/locks/*` ruleset and may carry the same omission-is-removal class against the rulesets API. Not audited here; follow-up = a `docs/self-improvement/categories/tooling/` entry, not a code change in this PR.
- **A gate that diffs config-vs-live on a schedule** — the durable fix for *config↔live drift as a class* (the same class that root-caused the #1227 gate escape, per [`docs/plans/coverage-sanitizer-required-contexts.md`](../coverage-sanitizer-required-contexts.md)). This PR makes one PUT faithful; it does not detect drift introduced through the GitHub web UI. Follow-up: a periodic `branch-protection-drift` check in the agentic self-test lane.
- **`required_contexts` ordering** — live is chronological-append, config is curated order. Set-equal is the invariant that matters (the API does not treat the array as ordered); no normalization is proposed.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped (empirically ~62% of post-ship plans drifted stale-in-place). Bind it to the impl-log write: in the SAME PR that populates the three sections above —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
