# Plan — branch-protection config completeness (stop the full-object PUT from silently clearing live gates)
<!-- plan-date: 2026-08-16 -->

> **Slug**: `branch-protection-config-completeness` (matches this file's basename without `.md`).
>
> **Status**: `shipped`
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
3. **Body builder emits them** — extend the Python heredoc in `setup-branch-protection.sh` to project each config flag into the PUT body, with a header comment naming the full-object-replace drop class so the next reader doesn't re-introduce the omission. Same edit switches `required_status_checks` to the `checks: [{context, app_id}]` form: the deprecated `contexts[]` spelling is *also* a silent widening, just one reached through a field name rather than a missing key (§ Risks).
4. **A test pins it** — `tests/bats/setup_branch_protection.bats` asserts the `--dry-run` body carries `required_conversation_resolution: true` (and the other flags), so a future config key added without a matching builder line fails a gate instead of silently vanishing on the next apply.

Trade-off named: the alternative — teach the script to `GET` the live object and merge the config over it — preserves unknown flags automatically but destroys the config-as-code property (live becomes partly authoritative, and drift stops being visible in a diff). Explicit-and-complete config is the choice; the bats suite is what keeps it complete.

## Files to modify

1. [`project.config.json:54`](../../../project.config.json#L54) — § `branch_protection`: add `required_conversation_resolution: true` + the remaining optional protection flags at their live values, plus `required_checks_app_id: 15368` (§ Risks — app-id pinning), and extend the block's `_doc` note to state *why* false-valued flags are written out.
2. [`project.config.schema.json:89`](../../../project.config.schema.json#L89) — § `branch_protection` `properties`: add the new boolean keys + `required_checks_app_id` (`["integer","null"]`). Required because `additionalProperties: false`.
3. [`agents/scripts/core/setup-branch-protection.sh:62`](../../../agents/scripts/core/setup-branch-protection.sh#L62) — body builder: emit the new flags, and switch `required_status_checks` from the deprecated `contexts[]` string array to the `checks: [{context, app_id}]` form so the app-id pin survives the PUT; amend the `# Idempotent:` header comment (line 24) to state the full-object-replace drop class explicitly and to scope the completeness claim to the PUT body surface (§ Risks — `required_signatures`).
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
- **Risk — the omission class has a second face: the *deprecated spelling*, not just the missing key.** `required_status_checks` accepts two forms — `checks: [{context, app_id}]` and the deprecated `contexts: [<string>]`. They are not interchangeable: `contexts[]` carries no app id, so a PUT built from it leaves **every** required context unpinned — GitHub then falls back to auto-selecting whichever app most recently reported that context (any source, for a context no app ever set), with no statement of intent recorded anywhere. Live `develop` pins all 22 contexts to app `15368` (GitHub Actions), and the original body builder emitted `contexts[]` — so the first faithful-looking apply would have silently dropped the pin on every required gate, in the same call that tightened `enforce_admins`. Same failure shape as the `required_conversation_resolution` drop (a loosening smuggled inside a tightening), reached through a *field spelling* rather than a *missing key* — which is why the completeness bats test alone would not have caught it. *Mitigation*: config states `required_checks_app_id`, the builder emits `checks[]`, and a bats case asserts every entry carries the pin **and** that `contexts` is absent from the nested object. Mutation-verified both ways (revert to `contexts[]` → red; wrong app id → red).
- **Risk — "complete" is scoped to the PUT body, not to the GET response.** `required_signatures` comes back from `GET .../protection` (live: `false`) but is **not** a PUT body parameter — GitHub owns it through `POST`/`DELETE .../protection/required_signatures`. It can therefore never be projected, and its absence from the config block is deliberate, not an oversight of the same class this plan fixes. *Mitigation*: named in the script header and in the schema `description` so the next reader does not mistake the enumeration for GET-exhaustive and "fix" it by adding a key the endpoint rejects. *Accepted residue*: signature enforcement stays outside config-as-code; changing it needs its own sub-resource call, exactly as `enforce_admins` did this session.
- **Risk — the completeness bats test is one-directional.** It walks config → body, so it catches *a config flag that stops reaching the wire*. It cannot catch the inverse — the bug that actually happened here, where live was `true` and the config never mentioned the key at all. Nothing in the config can detect a key it does not contain. *Mitigation*: the live-vs-dry-run diff (§ Verification) is the only detector for that direction, and the scheduled drift check in § Out of scope is its durable form. *Accepted* — a one-shot diff at PR time, not a standing gate.
- **Non-goal — changing any live protection value.** This PR states the current live values; it does not tighten or loosen a gate. `enforce_admins: true` was already applied out-of-band this session.
- **Non-goal — teaching the script to reconcile GET → merge → PUT.** Named and rejected above.
- **Non-goal — auditing `setup-locks-ruleset.sh`** for the same full-object-replace class. Flagged in § Out of scope.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: N/A — no C++ changes; the equivalent logic coverage lives in the bats bucket below.
- **Bucket A (bats, new)**: `bash agents/scripts/core/test-setup-branch-protection-bats.sh`. Assertions, all against `--dry-run` with `REPO` stubbed so no network call fires: (a) the body parses as JSON; (b) `.required_conversation_resolution == true`; (c) `.enforce_admins == true`; (d) `.required_pull_request_reviews.required_approving_review_count == 0`; (e) `[.required_status_checks.checks[].context]` is set-equal to the config's `required_contexts`; (f) **the completeness invariant** — every boolean key in the config block appears in the PUT body (the assertion that catches a future config key added without a builder line); (g) **the app-id pin** — every `checks[]` entry carries `app_id == required_checks_app_id`, and the deprecated `contexts` key is absent from the nested object.
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: N/A — no UI surface.
- **Bash-driver scenario / screenshot / sanitizer**: N/A — no product binary involved.
- **Build gate**: N/A — pure agentic-shell + config + docs diff; `bash agents/scripts/core/is-pure-docs-diff.sh` does **not** classify it as pure-docs (it touches `.sh` + `.json`), so the shell-lint and agentic self-test lanes are the binding CI gates rather than the dual-target build.
- **Shell lint**: `bash agents/scripts/core/test-shell-lint.sh` green over the new wrapper (shellcheck). *(Path corrected during implementation — the plan originally cited `scripts/dev/test-shell-lint.sh`, which does not exist.)*
- **Orphan-bats gate**: `bash agents/scripts/core/test-orphan-bats.sh` green — proves the new `.bats` has its wrapper.
- **Config-schema validation**: the `project.config.json` ↔ `project.config.schema.json` validator green (run via `bash scripts/dev/pre-ship.sh`).
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Live re-verification (post-merge, one command, non-mutating)**: `bash agents/scripts/core/setup-branch-protection.sh --dry-run` diffed against `gh api repos/<slug>/branches/develop/protection` — expected delta **over the PUT body surface**: none. The GET additionally returns `required_signatures`, which is not a PUT body parameter and is therefore expected to be absent from the body (§ Risks); normalise it away before comparing rather than treating it as drift. Compare `checks[]` as a `{context, app_id}` set, not the deprecated `contexts[]` array. This is the check that would have caught the original drift — and the only one that catches the live-has-it / config-lacks-it direction the bats suite structurally cannot.
- **Manual residue**: none. Every bullet above is a CLI invocation.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **`setup-locks-ruleset.sh` full-object audit** — the sibling script owns the `refs/locks/*` ruleset. Checked during the grill: it drives the **rulesets** API (`/repos/{slug}/rulesets`), a different endpoint class from branch protection, so it does not inherit this script's bug by construction. Whether *that* API carries its own omission-is-removal semantics is still unaudited; follow-up = a `docs/self-improvement/categories/tooling/` entry, not a code change in this PR.
- **A gate that diffs config-vs-live on a schedule** — the durable fix for *config↔live drift as a class* (the same class that root-caused the #1227 gate escape, per [`docs/plans/coverage-sanitizer-required-contexts.md`](../coverage-sanitizer-required-contexts.md)). This PR makes one PUT faithful; it does not detect drift introduced through the GitHub web UI. Follow-up: a periodic `branch-protection-drift` check in the agentic self-test lane.
- **`required_contexts` ordering** — live is chronological-append, config is curated order. Set-equal is the invariant that matters (the API does not treat the array as ordered); no normalization is proposed.

## Implementation log

- `3978eafd` · `wip(plan): branch-protection-config-completeness` — this plan, committed before implementation per plan-doc safety.
- `33c92f8b` · `fix(agentic): make branch_protection config a complete PUT-body statement` — config + schema + body builder + 10-case bats suite + wrapper, and the plan revision folded into the same commit.
- Out-of-band, no commit: `POST /repos/alexandrosk0/Smatchet/branches/develop/protection/enforce_admins` → `{"enabled": true}`. The sub-resource call, *not* a run of the script — the whole point of this plan is that a full-object PUT was not yet safe to fire.

## Deviations from plan

- **Added — the app-id pin (`required_checks_app_id`, `checks[]` projection, bats case 7).** Not in the original plan; surfaced by `grill-with-docs`. The plan reasoned about omission-is-removal via *missing keys* and missed that the deprecated `contexts[]` **spelling** is the same bug through a different door — it drops `app_id` on all 22 contexts, unpinning them from GitHub Actions. Fixed in-scope rather than deferred, because this is the PR that claims the PUT is a faithful transcription; shipping it with a known loosening would have made its own thesis false. § Risks entry added.
- **Added — `required_signatures` named as a deliberate exclusion** (script header, schema `description`, § Risks). Also from the grill: it is the one live key the body will never carry, and an undocumented absence invites a future reader to "fix" it by adding a key the endpoint rejects. The plan's "COMPLETE desired state" wording was softened to "complete over the PUT body surface" everywhere it appears.
- **Corrected — `_doc_optional_flags` → `_doc`.** The plan named a key that was never created; the note landed in the block's existing `_doc` string instead.
- **Corrected — shell-lint path.** The plan cited `scripts/dev/test-shell-lint.sh`, which does not exist. The real gate is `agents/scripts/core/test-shell-lint.sh`.
- **Corrected — "expected delta: none"** in § Verification was false as written: the GET always returns `required_signatures`, which the body cannot carry. Rewritten to scope the claim to the PUT body surface.
- **Scoped out, logged not fixed** — `test-gate-selftests.sh --selftest` fails all 11 negative fixtures on Windows/msys (the untracked-file mode fallback is `[ -x "$f" ]`, which msys answers TRUE for every temp file). Proven byte-identical to `origin/develop` and green there, so it is a platform divergence, not a regression from this diff. Filed at `docs/self-improvement/categories/tooling/2026-08-16-gate-selftests-untracked-mode-fallback-msys.md` rather than absorbed into this PR's scope.
- **Fixed in passing** — `test-agent-contract` drift on the gitignored `.claude/hooks/agent-token-log.py`, repaired with the gate's own `cp -f` (local-only, no diff).
- **Corrected post-review (CodeRabbit, this PR) — `app_id` has three spellings, not two.** The script comment (`agents/scripts/core/setup-branch-protection.sh:160`), the schema `description` (`project.config.schema.json`), and § Risks all said an omitted `app_id` means "any app may report this check". It does not: omitting the key makes GitHub **auto-select** the app that most recently provided that context (any source only when no app ever set it), and `-1` is the separate, explicit "any app" value. The runtime projection was already correct — `null` omits the key — so this was a documentation defect that would have mis-taught the next reader, plus a schema defect: `minimum: 1` made the `-1` spelling unrepresentable. Schema widened to `anyOf` (`null` / `-1` / positive int); bats case 7's null branch re-worded and given the floor guard it was missing (`any` over an empty array is also `false`).
- **Corrected post-review (CodeRabbit, this PR) — completeness was asserted in one direction only.** Every projected key read through `bp.get(k, False)`, and the completeness bats case diffs the keys *present* in config against the body. Delete a config key and both defences vanish together: the case loses the assertion it was making, the fallback supplies `false`, and the next apply writes that `false` to live protection as a real change — this plan's own failure mode, entered from the config side. Now: the schema's nested `required` names every modeled key (`_doc` excepted), the script hard-fails `exit 2` with a named-key error before building the body, every read is a subscript, and two bats cases cover it (11 pins the expected key set, 12 drives a mutated config through the new `$SMATCHET_BP_CONFIG` test seam and asserts exit 2 with no `gh` call). Emitted body verified byte-identical to the pre-fix script against the live config.

## Verification (actual)

- **Bucket A (bats)** — `bash agents/scripts/core/test-setup-branch-protection-bats.sh` → `Passed: 10  Failed: 0`; re-run after the CodeRabbit round → `Passed: 12  Failed: 0`. **PASSED.**
- **Post-review schema matrix** — `jsonschema.validate` over the live config plus five `required_checks_app_id` variants: `15368` / `-1` / `null` accepted, `0` / `-2` rejected, and a config with `required_conversation_resolution` deleted rejected as a missing required property. **PASSED.**
- **Post-review behaviour-preservation** — the `--dry-run` body from the pre-fix script (`git show HEAD:…`) diffed against the post-fix body over the unchanged live config: **identical**. The `.get(k, False)` → `bp[k]` conversion changes what happens on a *malformed* config, nothing on this one. **PASSED.**
- **Mutation-tested (3 ways), each confirming the suite actually bites** — (a) drop `required_conversation_resolution` from `TOP_LEVEL_FLAGS` → cases 3 + 8 red, rest green; (b) revert `checks` → `contexts[]` → cases 6 + 7 red, rest green; (c) pin a wrong `app_id` (99999) → case 7 red, rest green. Restored green after each. **PASSED.**
- **Test-quality fix found by mutation (b)** — case 7's `jq -e 'has("contexts") | not'` probed the *top-level* body, where `contexts` never lives; it was vacuously true and would have passed against the exact body it exists to reject (the floor guard caught the mutation instead). Re-pointed at `.required_status_checks` and paired with a positive `has("checks")`.
- **Live re-verification (non-mutating)** — `--dry-run` body normalised and diffed against `gh api repos/alexandrosk0/Smatchet/branches/develop/protection`: **delta over the PUT body surface: none** (all 22 `{context, app_id}` pairs, `strict`, `enforce_admins`, the review object, `restrictions`, and all 7 top-level flags equal). GET-only keys: `required_signatures` — exactly the one documented exclusion. **PASSED.**
- **Shell lint** — `shellcheck -x -S style` clean over both touched shell files. The repo-wide `agents/scripts/core/test-shell-lint.sh` sweep outran the foreground tool timeout and was backgrounded; it finished **exit 0** after the implementation commit. **PASSED (touched files + repo-wide).**
- **Orphan-bats gate** — `bash agents/scripts/core/test-orphan-bats.sh` → `PASS — all 91 bats suite(s) have a test-*.sh wrapper`. **PASSED.**
- **JSON validity** — `jq -e .` green on both `project.config.json` and `project.config.schema.json`. **PASSED.**
- **Doc validation / config-schema validation** — via `bash scripts/dev/pre-ship.sh`; see § Deviations for the one standing Windows-local failure (`test-gate-selftests`), which is pre-existing and filed, not introduced here.
- **Plan stress-test — `grill-with-docs`** — run against this plan. Confirmed the load-bearing claims against code (`merge-gates.sh:1580` and `pr-blocked-why.sh:121` do probe `required_conversation_resolution`; `setup-locks-ruleset.sh` drives the separate rulesets API and does not inherit this bug). Produced the two additions and the one-directionality finding recorded above. **PASSED — outcome folded into § Risks + § Deviations.**
- **Manual residue** — none.

