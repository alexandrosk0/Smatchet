# Plan — Wire Cursor Bugbot into the merge-gate poller + triage

> **Slug**: `bugbot-merge-gate` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

Cursor Bugbot (`cursor[bot]`) is **live** on this repo — it posts inline findings + a summary review on recent PRs (confirmed on #1389–#1393). But it is **invisible to every merge gate and to the triage pipeline**:

- **Merge gate** — the user-comments count (`merge-gates.sh:491-493`) filters `.author.__typename != "Bot"`, so all `cursor[bot]` artefacts are excluded; the CodeRabbit gate is login-scoped to `coderabbitai[bot]` (`merge-gates.sh:431-505`). Net: Bugbot findings neither block merge nor get counted anywhere.
- **Triage** — `coderabbit-triage.md:66` hard-filters `user.login == "coderabbitai[bot]"`; Bugbot is named only as a *future* addition ("Cursor Bugbot, Greptile, Sweep get added the same way as they appear in the wild").

No backlog entry, plan, or GitHub Issue covered this gap before now (verified 2026-06-18).

**Bugbot wire shape** (observed on #1393):
- **Inline findings** — `pulls/N/comments` (→ GraphQL `reviewThreads`), line-anchored; body opens `### <title>` then `**<Sev> Severity**` then `<!-- DESCRIPTION START -->`. Severity is in-body, parseable (`Medium Severity` etc.).
- **Summary review** — `pulls/N/reviews`, **always `state=COMMENTED`** (never `CHANGES_REQUESTED`, so it never moves `reviewDecision`), body marker `<!-- BUGBOT_REVIEW -->` + "found N potential issues" + `<!-- BUGBOT_FIX_ALL -->`.
- **Conversation noise** — `issues/N/comments` carries status messages, e.g. `### Bugbot couldn't run - usage limit reached`. **Not findings** — must never block.

**Intended outcome** — after this lands: an unresolved Bugbot finding on the head SHA **blocks** squash-merge (symmetric with CodeRabbit), a `bugbot-out-of-band` label downgrades that block to WARN, a Bugbot usage-limit / "couldn't run" state is treated as NONE-pass (a spend cap never wedges a merge), and `coderabbit-triage` routes Bugbot findings to subsystem specialists.

User decision (2026-06-18): **Full merge gate** (not advisory-only / phased).

## Approach

Mirror the existing CodeRabbit gate machinery rather than invent a parallel mechanism. The poller already does one `gh api graphql` call returning a positional tuple consumed by a bash `read`; Bugbot adds (a) a small set of appended tuple fields and (b) one new decision bucket.

**Gate (`merge-gates.sh`)** — four edits: (1) extend the GraphQL projection (`merge-gates.sh:460-506`) with **two appended fields** computed like the `$crall`/`$crstate`/`reviewThreads` fields — `$bbstate` (latest `cursor[bot]` review state/marker on `$sha`, or `ABSENT`) then `bb_open` (count of unresolved non-outdated `reviewThreads` authored by `cursor[bot]`, mirroring `:480-481`). **`bb_open` MUST be the new trailing field** — it is always numeric/non-empty, so the `data=$(gh …)` trailing-newline collapse can't strip it (the exact reason `crReviewSkipped` is last today, `:362-367`); `$bbstate` defaults to `ABSENT` and sits before it. (2) **Bump the field-count assertion `merge-gates.sh:558` `-ne 24` → `-ne 26`** and the field-index inventory comment (`:352-367`) — adding fields without this trips the fail-closed `expected 24` guard on every poll. (3) parse a `bugbot-out-of-band` label alongside `cr-out-of-band` (`merge-gates.sh:375`). (4) add a Bugbot decision bucket beside the CR bucket (`:630-900` / downgrade block `:950-966`).

**Gate guards on `bb_open` only** — no review-state check (Bugbot reviews are always `COMMENTED`). Decision tree (mirrors the CR NONE-grace machinery, `MERGE_GATES_BB_GRACE_POLLS` default 10 = `MERGE_GATES_CR_GRACE_POLLS`'s shape):

```
if bb_open > 0:
    if label bugbot-out-of-band: WARN (downgrade, bb_override=1)
    else:                        BLOCK
elif bugbot terminal signal on head (issues-comment body "couldn't run" / "usage limit"):
                                 PASS   # no-wedge short-circuit (skips grace, like cr_review_skipped :817)
elif label bugbot-out-of-band:   PASS   # operator waives → also short-circuit the grace wait
elif no cursor[bot] review on head AND poll < BB_GRACE_POLLS:
                                 PENDING (wait — full CR-style grace on every new head)
else:                            PASS   # grace expired (WARN line) OR on-head review present & clean
```

The terminal-signal + grace-expired + out-of-band branches are the three no-wedge escape hatches: a usage-capped or silent Bugbot can never block a merge longer than `BB_GRACE_POLLS`, and the label bypasses it outright.

**Triage (`coderabbit-triage.md` + `.py`)** — generalise the login filter from a single string to a small allow-list `{coderabbitai[bot], cursor[bot]}`, and teach the parser Bugbot's body shape (`### <title>` + `**<Sev> Severity**`) alongside CodeRabbit's emoji severities. The 19-rule override table + subsystem routing table are bot-agnostic and apply unchanged. Filter Bugbot's `issues/N/comments` status lines (`couldn't run`, `usage limit`) out of the finding set.

**Trade-off named**: gating on a third-party bot with a spend cap risks wedging merges when the cap is hit — mitigated by the explicit NONE-pass-on-flaky branch + the `bugbot-out-of-band` manual override, so a stuck Bugbot is always escapable without `SKIP_MERGE_GATES`.

## Files to modify

1. `agents/scripts/core/merge-gates.sh:460` — GraphQL projection: append `$bbstate` (field 24) then `bb_open` (field 25, trailing — always numeric) mirroring the `$crstate` / unresolved-CR-threads computations at `:431-481`. Field order is positional + trailing-non-empty-constrained (see § Approach); `bb_open` last.
2. `agents/scripts/core/merge-gates.sh:558` — bump the field-count fail-closed assertion `-ne 24` → `-ne 26` (24 existing + `$bbstate` + `bb_open`).
3. `agents/scripts/core/merge-gates.sh:215` — add `local BB_GRACE_POLLS="${MERGE_GATES_BB_GRACE_POLLS:-10}"` beside `CR_GRACE_POLLS` (`:215`); document the knob in the header (`:60`).
4. `agents/scripts/core/merge-gates.sh:375` — parse `bugbot-out-of-band` label (`$bbOob`) beside `$cr` (`cr-out-of-band`).
5. `agents/scripts/core/merge-gates.sh:~640` — read `bb_state="${fields[24]:-ABSENT}"` + `bb_open="${fields[25]:--1}"` (-1 fails closed); add the Bugbot decision bucket per the § Approach tree (BLOCK on `bb_open>0`; terminal-signal / grace-expired / out-of-band → PASS; full grace via `BB_GRACE_POLLS`).
6. `agents/scripts/core/merge-gates.sh:950` — `bugbot-out-of-band` downgrade (BLOCK→WARN, `bb_override=1`) + grace short-circuit beside the `cr-out-of-band` downgrade; emit a `WARN: bugbot-out-of-band …` line.
7. `agents/scripts/core/merge-gates.sh:11` + `:27` + `:352-367` — header-comment contract: document gate #4 (Bugbot), the `bugbot-out-of-band` override row, the `MERGE_GATES_BB_GRACE_POLLS` knob, and the two new tuple field indices (24 `bbState` · 25 `bbOpen`) in the field-index inventory.
8. `agents/core/coderabbit-triage.md:66` — replace single-login filter with allow-list `{coderabbitai[bot], cursor[bot]}`; add a Bugbot body-shape parse note (`### <title>` / `**<Sev> Severity**` / `<!-- DESCRIPTION START -->`) + the `couldn't run` / `usage limit` noise-filter to § Process step 2–3.
9. `agents/scripts/core/coderabbit-triage.py` — Python-port sync of #8 (the shared "rules version" marker doctest at end-of-CI fails if `.md` and `.py` disagree — `coderabbit-triage.md:170`).
10. `docs/agent-rules/merge-gates.md` — document the Bugbot gate (#4) + `bugbot-out-of-band` in the override-label table + per-outcome semantics (incl. the three no-wedge escape hatches + `BB_GRACE_POLLS`).
11. `AGENTS.md` § Merge gates — one-clause add: Bugbot as a 4th condition + `bugbot-out-of-band` in the override list (stay navigation-only; defer detail to #10). Watch the 150-line cap (`agent_size_audit.py`).
12. `tests/bats/merge_gates.bats` — new cases (see § Verification Bucket A).

## Existing utilities reused

- CR gate machinery — `merge-gates.sh:431-505` (`$crall` / `$crstate` / `$crbody` / unresolved-CR-threads count) is the structural template for `$bbstate` / `bb_open`.
- `cr-out-of-band` label plumbing — `merge-gates.sh:375` (parse) + `:950-966` (downgrade) is the exact pattern for `bugbot-out-of-band`.
- Bot-exclusion idiom — `.author.__typename != "Bot"` (`merge-gates.sh:491-493`) already keeps Bugbot conversation noise out of the user gate; reused as-is (no change to the user-comments count).
- 19-rule override table + subsystem routing table — `coderabbit-triage.md:89-137`, bot-agnostic, applied to Bugbot findings unchanged.
- `merge_gates.bats` harness + GraphQL-fixture pattern — `tests/bats/merge_gates.bats` existing CR cases are the fixture template.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — agentic shell (bash poller + agent markdown); no product render path touched.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — no UI thread; the poller runs out-of-process in CI / the watcher.
- **Pillar 3 (never crash)**: jq projection stays total — Bugbot fields default (`ABSENT`, `0`) when no `cursor[bot]` artefacts exist, so a Bugbot-free PR yields a clean pass, never a parse error. The two new fields append at the tuple tail (`bb_open` last, always-numeric) so existing `fields[0..23]` reads don't shift; the `-ne 26` count guard catches any projection/read miscount fail-closed.
- **Pillar 4 (accessibility)**: N/A — no user-facing surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

**N/A — diff is agentic-shell + agent-markdown + docs only** (`agents/scripts/core/*.sh|.py`, `agents/core/*.md`, `tests/bats/*`, `docs/`, `AGENTS.md`). No `Source/Core/` C++, no `project.config.json` perf/lint zone, no `SMATCHET_UI_PERF_SCOPE` marker. PR-fast / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory all N/A.

## Risks / non-goals

- **Spend-cap wedge** — a Bugbot usage-limit on the head could block merge forever. *Mitigated*: three no-wedge escape hatches — terminal-signal short-circuit (`couldn't run`/`usage limit` body → PASS, skips grace), `BB_GRACE_POLLS`-bounded wait (a silent Bugbot blocks at most the grace window, then WARN-pass), and the `bugbot-out-of-band` manual override (PASS + grace short-circuit). This is the single highest-risk behaviour; the bats suite must pin all three (usage-limit fixture → PASS; grace-expiry fixture → PASS; out-of-band fixture → PASS).
- **Tuple-order regression** — the GraphQL tuple is positional with a `-ne 24` (→`26`) fail-closed count guard (`merge-gates.sh:558`) AND a trailing-non-empty constraint (command-subst strips trailing empty fields — `:362-367`). *Mitigated*: append the two fields at the tail with `bb_open` (numeric) last; bump the count guard `24→26` + the field-index inventory (`:352-367`) in the same edit; bats CR cases stay green (regression canary). Inserting mid-tuple, or putting `$bbstate` (can be empty) last, is the failure mode this risk names.
- **`.md`/`.py` drift** — the triage rule body lives in two files with a CI doctest guard (`coderabbit-triage.md:170`). *Mitigated*: edit both in the same commit; bump the shared rules-version marker.
- **Bugbot login variant** — confirmed `cursor[bot]` today; a Pro/self-hosted variant could differ. *Accepted* — same "confirm against actual JSON before extending the allow-list" caveat the CR filter already carries.
- **Bugbot "Fix all" / suggestion blocks** — Bugbot offers auto-fix links. *Non-goal*: this plan only *gates + triages*; it does not auto-apply Bugbot fixes.
- **Non-goal**: changing CodeRabbit behaviour, the user-comments gate, or `SKIP_MERGE_GATES` semantics.
- **Non-goal**: a label-existence bootstrap — `bugbot-out-of-band` must be `gh label create`d on the repo (a one-time ops step, noted in § Verification), not a code file.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (bats, `tests/bats/merge_gates.bats`)**: new fixture cases — (1) Bugbot review with `bb_open=2` unresolved `cursor[bot]` threads on head → **BLOCK**; (2) same + `bugbot-out-of-band` label → **WARN/pass** with the downgrade line; (3) `issues/N/comments` body `Bugbot couldn't run - usage limit reached`, no open threads → **PASS** (terminal-signal short-circuit, the spend-cap-no-wedge canary; skips grace); (4) no `cursor[bot]` review on head AND `poll < BB_GRACE_POLLS` → **PENDING** (grace wait — Bugbot mid-review, don't merge under it); (5) no `cursor[bot]` review on head AND `poll >= BB_GRACE_POLLS` → **PASS** (grace expired — never wedge); (6) clean `cursor[bot]` review on head, `bb_open=0` → **PASS**; (7) `bugbot-out-of-band` label with no review yet → **PASS** (waiver short-circuits grace); (8) no `cursor[bot]` artefacts at all → unchanged pass (Bugbot-free PR); (9) field-count guard: a fixture asserting the `-ne 26` assertion fires on a 25-field tuple (fail-closed canary); (10) existing CR cases stay green (tuple-order regression canary). Run: `bash tests/bats/merge_gates.bats` (or via `scripts/dev/test-all.sh`).
- **`--selftest`**: `bash agents/scripts/core/merge-gates.sh --selftest` (if present) stays green after the header contract-card tokens are updated.
- **Triage `.md`/`.py` sync doctest**: the end-of-CI grep that pins the shared rules-version marker across `coderabbit-triage.md` ↔ `.py` passes (bump the marker in both).
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Build gate**: N/A — no C++ in the diff (pure agentic-shell + docs); skip the dual-target build per `is-pure-docs-diff.sh`-class scope. (If any guard insists, `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` is a no-op confirmer.)
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-steps). A red doc-validation job blocks merge even though non-required. Note: `agent_size_audit.py` must stay green after the `AGENTS.md` edit (150-line cap).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: ✅ run 2026-06-18. Outcome — three design forks locked: **(Q1) gate role** = *Full merge gate* (blocking, symmetric with CodeRabbit, not advisory-WARN); **(Q2) block threshold** = *any unresolved `cursor[bot]` finding* blocks (no severity floor — Bugbot already self-filters to High/Medium); **(Q3) mid-review race** = *full CR-style grace* — `BB_GRACE_POLLS` (default 10, env `MERGE_GATES_BB_GRACE_POLLS`) bounds the wait for a Bugbot review to land on the head before grace-expiry WARN-passes. Code-grounding refinements folded in: tuple is *append-two-at-tail* (`bb_open` numeric last, `$bbstate` default `ABSENT` before it) with the `-ne 24`→`26` count-guard bump + `:352-367` inventory update, not a loose "append-only"; three no-wedge escape hatches (terminal-signal / grace-expiry / out-of-band label) pinned in Risks + Bucket A. No new `CONTEXT.md` term and no ADR warranted (reversible config knob, symmetric with the documented CR gate it mirrors — fails the hard-to-reverse + surprising-without-context ADR bar). Required for every plan — do not delete.
- **Ops step (label bootstrap)**: `gh label create bugbot-out-of-band --description "Downgrade Bugbot merge block to WARN" --color <hex>` on the repo, once, before the gate can be overridden. Named here so it is not lost as silent manual residue.
- **Manual residue**: none expected; if any verification ends up manual, name the deferred-automation plan + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **Greptile / Sweep / other future PR-bots** — same allow-list pattern; no-action until they appear live on the repo (Bugbot is the only third bot posting today).
- **Auto-applying Bugbot "Fix all" suggestions** — follow-up plan if desired; this plan gates + triages only.
- **Bugbot thread auto-resolution** — the CR path has `maybe_resolve_stuck_cr_threads` (`coderabbit-triage.md:166`); a Bugbot equivalent is a separate follow-up, not required for the gate to function (open threads simply block until a human/agent resolves or the `bugbot-out-of-band` override fires).
- **Promoting `bugbot-out-of-band` into branch protection / required checks** — Bugbot stays PR-advisory like CR (custom poller only), not a GitHub-required check.

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
   > **Keep the literal `<slug>` placeholder in this committed step — do NOT
   > expand it to this plan's real filename.** Writing the actual basename here
   > manufactures a `docs/plans/shipped/<name>.md` path that points at a file
   > still living in `active/` (the move hasn't happened yet), which
   > `test-plan-ref-integrity.sh` reports as a dangling self-reference. The gate
   > carves out the *placeholder* form on the Archive `git mv` line; the
   > expanded form defeats that carve-out. Run the literal command with your
   > slug substituted at the shell — never bake the expansion into the file.
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*No ref-sweep — references use the tier-less form `docs/plans/<slug>.md` (the gates resolve it against any tier; PR #890), so the move can't break them. Write new plan references tier-less.*

*(Delete this `## Archive` block as part of step 2 — once moved to `shipped/`, the file is reference material and the checklist has served its purpose.)*
