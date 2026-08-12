# Plan — Exempt pure self-improvement doc PRs from CR / Bugbot review + heavy tests

> **Slug**: `self-improvement-pr-review-exemption` (matches this file's basename without `.md`).
>
> **Status**: `shipped` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Self-improvement doc PRs — a PR whose diff is **entirely** under `docs/self-improvement/**` (the per-entry backlog files `categories/<cat>/<date>-<slug>.md`, `postmortems.md`, `applied.md`, `AGENT_SELF_IMPROVEMENT.md`) — are low-risk, never-compiled markdown that change only the **agent system's own** ledger. Today they run the full ship gauntlet:

- **CodeRabbit reviews them.** `.coderabbit.yaml:24-30` enables `auto_review` on `develop`/`main`; the `path_filters` block (`.coderabbit.yaml:33-45`) excludes vendored / generated / lock-render artefacts but has **no** `docs/self-improvement/**` exclusion — so CR spends a review cycle on every backlog note.
- **Cursor Bugbot can block them.** Bugbot has **no in-repo config file** (confirmed `docs/plans/shipped/bugbot-merge-gate.md` § Deviations — "Bugbot has no in-repo config file to probe, unlike CR's `.coderabbit.yaml`"); the repo's only lever is the merge-gate, where an unresolved `cursor[bot]` inline finding **blocks** squash-merge (`merge-gates.sh` gate #4). A markdown nit on a backlog file can therefore wedge a self-improvement PR.
- **Heavy tests already skip** — `build-and-test.yml`'s `changes` job classifies `docs/*` (incl. `docs/self-improvement/*`) as docs-only (`:139`) → `code=false` → `windows-msvc` / `-light` / `comment-noise-gate` / buckets all skip (a skipped required check counts as success for branch protection). `is-pure-docs-diff.sh:53` already lets the local ship-loop skip `test-all.sh` on `docs/**`. So the "thorough test suite" half of the ask is **already satisfied**; only the lightweight `doc-validation.yml` (anchors / md_lint / links — the suite that keeps the docs *valid*) runs, which we keep.

User ask (2026-06-20): self-improvement doc PRs shouldn't be code-reviewed by CR or Bugbot, nor run a thorough test suite. **Decisions locked** (AskUserQuestion, 2026-06-20): **scope** = *pure self-improvement only* (a mixed code+doc PR still gets full review + tests); **mechanism** = *in-repo auto-detect* (no manual label; the gate enforces it itself, plus a one-time Cursor-dashboard path-ignore for Bugbot's *posting*).

**Intended outcome** — after this lands: a PR whose diff is entirely under `docs/self-improvement/**` is **not** reviewed by CodeRabbit, is **not** blocked by a Cursor Bugbot finding, and runs **no** heavy C++ build/test — only the lightweight doc-validation suite. Any PR that also touches one non-self-improvement file gets the full gauntlet unchanged.

## Approach

Three independent levers, smallest-blast-radius each:

1. **CodeRabbit — one YAML line.** Add `!docs/self-improvement/**` to `.coderabbit.yaml` `path_filters`. CR then has nothing reviewable on a pure self-improvement PR and posts its terminal **"Review skipped"** status — which `merge-gates.sh` already fast-passes via the `crReviewSkipped` field (the GraphQL `description` read at `merge-gates.graphql:36-40`, consumed at `merge-gates.sh:562-576` / `:811-819`, whose comment literally cites "docs-only / path-filtered / trivial diff per .coderabbit.yaml"). **No script change is needed for the CR gate** — the path_filter rides the existing fast-pass.

2. **Bugbot — merge-gate auto-detect.** Because Bugbot has no repo config, the in-repo lever is the poller. The gate's GraphQL query does **not** currently fetch the PR file list (`merge-gates.graphql` projects state / checks / reviews / threads / comments, no `files` connection), so add one `files(first: 100)` connection and a derived `$selfImpOnly` boolean (every changed path matches `^docs/self-improvement/`, no file-page overflow). When `$selfImpOnly` is true, the Bugbot decision bucket downgrades its BLOCK to a WARN (sets `bb_block=false`) automatically — the same consume-point the `bugbot-out-of-band` label uses, but keyed on detection, not a label. The CR gate is belt-and-suspendered the same way (harmless given lever 1).

3. **Heavy tests — already exempt; pin it.** No production change; add a regression bats case asserting a pure-self-improvement diff classifies as docs-only, so a future edit to the `changes` job can't silently start running heavy tests on these PRs.

**Trade-off named**: the in-repo gate stops Bugbot from *blocking*, but cannot stop it from *posting* (consuming Cursor spend) — that needs a one-time Cursor-dashboard path-ignore for `docs/self-improvement/**`, flagged as ops residue. Fail-safe direction throughout: any detection uncertainty (file-page overflow, empty file list, parse miss) → **not** exempt → full gates.

## Files to modify

1. `.coderabbit.yaml:33` — append `- '!docs/self-improvement/**'` to `path_filters` (one line; keep the existing vendored/generated excludes).
2. `agents/scripts/core/merge-gates.graphql:8` — add `files(first: 100) { pageInfo { hasNextPage } nodes { path } }` to the `pullRequest { … }` projection (beside `labels`).
3. `agents/scripts/core/merge-gates.sh` GATE_FILTER (`:481-648`) — compute `$selfImpOnly`: `true` iff `($pr.files.nodes | length) > 0` AND `(.pageInfo.hasNextPage | not)` AND `all(.nodes[].path; startswith("docs/self-improvement/"))`; **append it as the new trailing tuple field (index 27)** — always `"true"`/`"false"`, so the command-subst trailing-empty-strip invariant holds (the reason `bbOob` is last today, `:475-479`).
4. `agents/scripts/core/merge-gates.sh:700` — bump the field-count fail-closed assertion `-ne 27` → `-ne 28`; update the field-index inventory comment (`:459-479`) with index 27 (`selfImpOnly`).
5. `agents/scripts/core/merge-gates.sh` (Bugbot decision bucket, `~:640`+ where `bb_state`/`bb_open` are read) — read `self_imp_only="${fields[27]:-false}"` (`-`/parse-miss → `false`, fail-safe = not exempt); when `true`, set `bb_block=false` + emit `INFO: self-improvement doc PR — Bugbot gate auto-skipped` and short-circuit the `BB_GRACE_POLLS` pending-wait; belt-and-suspender the CR gate the same way. Fold `bb_block` into the existing `GATES_PASSED` composite unchanged (the conjunct already exists per bugbot-merge-gate).
6. `agents/scripts/core/merge-gates.sh` header contract (`:11-21` gate list, `:29-40` override block, `:459-479` field inventory) — document the self-improvement auto-exemption + the new field. Net-add only to comments.
7. `docs/agent-rules/merge-gates.md` — document the self-improvement auto-exemption (CR via path_filter + the `crReviewSkipped` fast-pass; Bugbot via `$selfImpOnly` auto-skip) in the per-outcome semantics + the **Cursor-dashboard path-ignore ops note** (the only lever that stops Bugbot *reviewing*).
8. `AGENTS.md` § Merge gates (`:42`) — **net-zero edit**: fold a half-clause ("pure `docs/self-improvement/**` PRs auto-exempt from CR + Bugbot") into the existing condition sentence rather than adding a line — AGENTS.md is at 155 lines (grandfathered over the 150 cap; `agent_size_audit.py --diff` fails on growth, per `docs/plans/shipped/bugbot-merge-gate.md` "held at 154 lines"). If it can't fit net-zero, **defer entirely to #7** (AGENTS.md is navigation-only and already points to `merge-gates.md` for full detail).
9. `tests/bats/merge_gates.bats` + `tests/fixtures/merge_gates_selfimp_*.json` — new cases (see § Verification Bucket A).
10. (optional hardening) `tests/bats/` regression pinning the `changes`-job docs-only classification for a pure-self-improvement diff — or rely on the existing `test-required-context-parity` + doc-validation coverage; decide during impl.

## Existing utilities reused

- `crReviewSkipped` terminal-skip fast-pass — `merge-gates.sh:562-576` + `:811-819` (reads the CR StatusContext `description` from `merge-gates.graphql:36-40`); makes the path_filter sufficient for the CR gate with **zero** script change.
- `bugbot-out-of-band` → `bb_block=false` downgrade branch — `merge-gates.sh` Bugbot bucket (`~:640`+) is the exact structural template for the `$selfImpOnly` auto-skip (same consume-point).
- CI docs-vs-code classifier — `build-and-test.yml` `changes` job (`:118-143`) already skips the heavy suite for `docs/*`.
- `is-pure-docs-diff.sh:53` allow-list (`^(docs/|backlog/|agents/scripts/|.*\.md$)`) — already covers `docs/self-improvement/**` for the local ship-loop.
- `merge_gates.bats` GraphQL-fixture harness + the 4 `merge_gates_bb_*.json` fixtures — the case template for the new self-improvement fixtures.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — agentic shell (bash poller) + YAML config + agent/docs markdown; no product render path touched.
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: N/A — no UI thread; the poller runs out-of-process in CI / the watcher.
- **Pillar 3 (never crash)**: the jq `$selfImpOnly` stays total — defaults `false` when `files` is empty/absent, so a non-self-improvement or Bugbot-free PR yields the existing behaviour, never a parse error; the new field appends at the tuple tail (always-non-empty bool) so existing `fields[0..26]` reads don't shift, and the `-ne 28` count guard catches any miscount fail-closed.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: N/A — no user-facing surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

**N/A — diff is agentic-shell + YAML + docs only** (`agents/scripts/core/merge-gates.{sh,graphql}`, `.coderabbit.yaml`, `docs/`, `AGENTS.md`, `tests/bats/*`, `tests/fixtures/*`). No `Source/Core/` C++, no `project.config.json` perf/lint zone, no `SMATCHET_UI_PERF_SCOPE` marker. PR-fast / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory all N/A.

## Risks / non-goals

- **Bugbot still POSTS on these PRs unless the dashboard path-ignore is set.** The in-repo gate stops *blocking*, not *posting* (Cursor spend continues). *Mitigated*: the ops step (§ Verification) adds `docs/self-improvement/**` to Bugbot's ignore paths; the gate change makes any stray finding non-blocking regardless, so the no-wedge guarantee holds even if the dashboard lapses.
- **Tuple-order regression** — the positional tuple has a `-ne 27`(→`28`) fail-closed count guard + a trailing-non-empty constraint (`merge-gates.sh:475-479`, :700). *Mitigated*: append `$selfImpOnly` (bool, non-empty) **last**; bump the guard + field inventory in the same edit; existing CR/Bugbot/CI bats stay green as the regression canary.
- **File-list pagination** — a PR with > 100 files can't be fully classified from one `files(first:100)` page. *Mitigated*: `pageInfo.hasNextPage == true` → `$selfImpOnly=false` → full gates (a self-improvement PR is 1–3 files; the cap is unreachable in practice and fails safe if not).
- **Mixed-PR leakage** — a code change must never ride along unreviewed. *Mitigated*: scope is **pure** self-improvement — a single non-`docs/self-improvement/` path flips `$selfImpOnly` false → full gates; pinned by bats case (2).
- **Empty-diff edge** — a 0-file PR would vacuously satisfy `all(...)`. *Mitigated*: the `length > 0` conjunct requires at least one file before exemption.
- **Non-goal**: changing CR/Bugbot behaviour on **non**-self-improvement PRs, the user-comments gate, or `SKIP_MERGE_GATES` semantics.
- **Non-goal**: skipping `doc-validation.yml` (md_lint / anchors / markdown-links still run — they keep the backlog docs valid; the ask is about *code review + heavy tests*, not doc-lint).
- **Non-goal**: a new override label — this is auto-detect, so `test-oob-label-impl.sh` (label↔impl coupling) is untouched.
- **Non-goal**: broadening to all `docs/**`-only PRs (the rejected scope option) — self-improvement only.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (bats, `tests/bats/merge_gates.bats`)**: new fixtures — (1) pure `docs/self-improvement/**` diff + an open `cursor[bot]` inline finding on head → **Bugbot gate PASS** (auto-skip, not BLOCK) with the `INFO: self-improvement … auto-skipped` line; (2) mixed diff (`docs/self-improvement/x.md` + one `Source/Core/...cpp`) + same open finding → **BLOCK** (scope canary — leakage guard); (3) pure self-improvement + CR `description="Review skipped"` status → **CR gate fast-pass** (unchanged `crReviewSkipped` path, confirms the path_filter wiring); (4) `files.pageInfo.hasNextPage=true` over self-improvement paths → **not exempt → full gates** (fail-safe canary); (5) a 27-field tuple trips the `-ne 28` assertion (fail-closed canary); (6) existing CR/Bugbot/CI cases stay green (tuple-order regression canary). Run: `bash agents/scripts/core/test-merge-gates.sh` (or via `scripts/dev/test-all.sh`).
- **`.coderabbit.yaml` validity**: yamllint (CR's `tools.yamllint` + doc-validation) parses the new `path_filters` entry; the file stays schema-valid.
- **Build gate**: N/A — no C++ in the diff (pure agentic-shell + YAML + docs); skip the dual-target build per `is-pure-docs-diff.sh`-class scope. (If any guard insists, `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` is a no-op confirmer.)
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint / markdown-links — defer to the script, don't hardcode the sub-steps). A red doc-validation job blocks merge even though non-required. `agent_size_audit.py --diff origin/develop` must stay green after the `AGENTS.md` edit (150-line cap; keep it net-zero or defer the clause to `merge-gates.md`).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: ✅ run 2026-06-20. Outcome — three forks confirmed against the code: **(Q1) CR lever** = path_filter (not a merge-gate special-case), because the repo's *own* `crReviewSkipped` comment already names "path-filtered" as a terminal fast-pass — so the YAML edit is sufficient and the script stays CR-untouched. **(Q2) Bugbot lever** = `$selfImpOnly` auto-skip in the poller (Bugbot has no repo config; confirmed by the bugbot-merge-gate deviation note), with the Cursor-dashboard path-ignore as the *posting* complement (named ops residue, not silent). **(Q3) tests** = already exempt at CI (`changes` job `:139`) — verified, so no production CI change, only a regression pin. Code-grounding refinement: the gate needs the PR file list, which the GraphQL query lacks today → added the `files(first:100)` connection + the tuple-tail field + the `-ne 27→28` guard bump (mirrors the bugbot-merge-gate append-at-tail discipline). No new `CONTEXT.md` term and no ADR warranted (reversible config + a symmetric extension of the documented CR/Bugbot gates — fails the hard-to-reverse + surprising-without-context ADR bar). Required for every plan — do not delete.
- **Ops step (named, not silent)**: in the Cursor dashboard (Bugbot settings → ignore paths), add `docs/self-improvement/**` so Bugbot does not *review* these PRs at all. This is the **only** lever that prevents Bugbot review; the merge-gate change prevents *blocking*. Documented in `docs/agent-rules/merge-gates.md` § gate 4.
- **Manual residue**: the Cursor-dashboard step above is the one unavoidable manual action (third-party UI, no repo file). No other manual residue; if any verification ends up manual, name the deferred-automation plan + add a `docs/self-improvement/categories/tooling.md` entry.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **All docs-only PRs** (the broader rejected scope) — same `$selfImpOnly` pattern generalised to `docs/**`; no-action unless the user later opts in (CI already skips their heavy tests; only CR/Bugbot review would change).
- **`docs/plans/**` and other low-risk doc trees** — deliberately **not** exempt: plan docs are design artefacts that genuinely benefit from CR review.
- **Auto-resolving Bugbot threads on these PRs** — unnecessary once the gate auto-skips (open threads simply don't block); a Bugbot thread-auto-resolve equivalent stays the separate follow-up named in `bugbot-merge-gate.md` § Out of scope.
- **Promoting the exemption into branch protection / required checks** — stays poller-only (CR/Bugbot are PR-advisory, never GitHub-required), same posture as the existing gates.
- **`bb_override`/exemption attribution in the `GATE_SNAPSHOT` ledger** — the auto-skip emits an operator-visible stderr `INFO` line only; threading an "auto-exempt" reason into the ADR-0017 merge-snapshot is the same follow-up `bugbot-merge-gate.md` deferred for `bb_override`.

## Implementation log
*(branch `claude/trusting-pasteur-mk1ebw`; squash-merged to develop as `4685997d` / #1468.)*
- `.coderabbit.yaml` — added `!docs/self-improvement/**` to `path_filters`; CR now has nothing reviewable on a pure self-improvement PR and posts its terminal `Review skipped` status (the `crReviewSkipped` fast-pass — no `merge-gates.sh` change needed for the CR gate proper).
- `agents/scripts/core/merge-gates.graphql` — added the `files(first: 100) { pageInfo { hasNextPage } nodes { path } }` projection (not folded into the global pagination-overflow OR; files overflow only fails `selfImpOnly` safe).
- `agents/scripts/core/merge-gates.sh` — GATE_FILTER computes `$selfImpOnly` (changed-paths all under `docs/self-improvement/`, non-empty, no file-page overflow) as the trailing tuple field (27); field-count guard `-ne 27`→`-ne 28` + field-index inventory; reads `self_imp_only`; Bugbot bucket auto-downgrades a `bb_open>0` block to WARN + short-circuits the STALE-grace wait; a belt-and-suspenders CR downgrade (does NOT set `cr_overridden`); header contract (auto-exemption note + field 27).
- `docs/agent-rules/merge-gates.md` — new **§ Self-improvement doc PR auto-exemption** (CR + Bugbot mechanics + the Cursor-dashboard ops note).
- `AGENTS.md` § Merge gates — net-zero clause appended to gate-condition (4) (held at 154 lines).
- `tests/bats/merge_gates.bats` — +7 self-improvement cases; the Bugbot (9) field-count canary updated 27→28.

## Deviations from plan
- **CR gate downgrade does NOT set `cr_overridden`.** The plan said "belt-and-suspenders the CR gate"; implemented so the self-improvement auto-skip leaves `cr_override=0` in the GATE_SNAPSHOT — it is a documented auto-behaviour, not a label override, so the clean-merge ledger contract + `postmortem-owed` are preserved (ledger attribution deferred per § Out of scope).
- **Bugbot open-findings-free auto-skip is shown only when load-bearing.** The `selfImpOnly` pass branch fires for `bb_state==STALE` only (to short-circuit the grace wait); an `ABSENT`/clean-on-head self-improvement PR falls through to the normal clean-pass print, so the "auto-skipped" label never appears when there is nothing to skip (bats case 7 pins this).
- **No new fixture files.** The 7 new cases inject a `files` node via the existing `fixture_override` helper on the `bb_*` / `cr_changes` / `pass` fixtures, rather than adding 4 JSON fixtures — legacy fixtures have no `files` node, so `selfImpOnly` defaults false and the Bugbot (1)-(12) not-exempt baseline is unchanged.
- **Heavy-test regression-pin (plan item 10, optional) not added.** The heavy C++ suite is already skipped for `docs/*` by `build-and-test.yml`'s `changes` job (verified — no code change), and `doc-validation` already covers these PRs; a dedicated bats pin was judged redundant.
- **Archival deferred (Status stayed `active` at ship time).** Moving this plan to `docs/plans/shipped/` requires regenerating `docs/plans/INDEX.md`, but the ship-time container's `test-plan-index.sh --fix` produced INDEX content that diverged from develop's committed (CI-validated) INDEX (shallow clone — the index's approx-date column comes from git first-commit dates) — committing it would have redded the `test-plan-index` / `autosync-plan-index` checks. Self-references use the tier-less `docs/plans/self-improvement-pr-review-exemption.md` form so the later move can't break them. **Resolved 2026-07-13**: archival performed in a full-history environment where `test-plan-index.sh` validates clean against develop's committed INDEX.

## Verification (actual)
- `bash agents/scripts/core/test-merge-gates.sh` → **Passed: 153 Failed: 0** (146 pre-existing CR/CI/user/Bugbot regression canaries stay green + 7 new self-improvement cases: pure-diff Bugbot auto-skip PASS, mixed-diff BLOCK, files-overflow fail-safe BLOCK, empty-files BLOCK, STALE-grace short-circuit PASS, CR-block belt-and-suspenders PASS, moot-exemption clean PASS with `cr_override=0` + no spurious WARN).
- `bash -n merge-gates.sh` clean; `shellcheck -S warning merge-gates.sh` → only the 2 **pre-existing** SC2034 (`has_tests_oob`/`has_perf_oob`), no new findings; `bash agents/scripts/core/test-shell-lint.sh` → 225/0.
- `.coderabbit.yaml` valid YAML; `test-oob-label-impl` → 9/9 (no new label added); `agent_size_audit.py --diff origin/develop` exit 0 (AGENTS.md 154 lines); `test-doc-anchors` 100/0; `test-markdown-links` (touched docs) 3/0; `test-plan-ref-integrity` 165/0; `test-plan-naming` + `md_lint` (plan-doc) clean.
- **Build gate**: N/A — no C++ in the diff (agentic-shell + YAML + docs).
- **Ops residue (manual, named)**: add `docs/self-improvement/**` to Bugbot's ignore paths in the **Cursor dashboard** — the only lever that stops Bugbot *reviewing* (it has no in-repo config); the gate change only stops it *blocking*. Documented in `docs/agent-rules/merge-gates.md` § Self-improvement doc PR auto-exemption.

