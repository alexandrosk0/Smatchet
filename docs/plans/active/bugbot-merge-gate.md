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

**Gate (`merge-gates.sh`)** — three edits: (1) extend the GraphQL projection (`merge-gates.sh:460-506`) with Bugbot fields computed the same way the `$crall`/`$crstate`/`reviewThreads` fields are — a `$bbstate` derived from the latest `cursor[bot]` review on `$sha` (or its absence), and a `bb_open` count of unresolved non-outdated reviewThreads authored by `cursor[bot]`; (2) parse a `bugbot-out-of-band` label alongside `cr-out-of-band` (`merge-gates.sh:375`); (3) add a Bugbot decision bucket beside the CR bucket (`merge-gates.sh:625-900` / downgrade block `:950-966`) — `bb_open > 0` BLOCKs, `bugbot-out-of-band` downgrades that to WARN, and a **flaky/absent** state (`couldn't run` / `usage limit` body, or no Bugbot artefacts at all) is NONE-pass exactly like CR's NONE fallback so it never wedges. Bugbot's COMMENTED-only review means we gate on the **open-finding count**, not on a review state.

**Triage (`coderabbit-triage.md` + `.py`)** — generalise the login filter from a single string to a small allow-list `{coderabbitai[bot], cursor[bot]}`, and teach the parser Bugbot's body shape (`### <title>` + `**<Sev> Severity**`) alongside CodeRabbit's emoji severities. The 19-rule override table + subsystem routing table are bot-agnostic and apply unchanged. Filter Bugbot's `issues/N/comments` status lines (`couldn't run`, `usage limit`) out of the finding set.

**Trade-off named**: gating on a third-party bot with a spend cap risks wedging merges when the cap is hit — mitigated by the explicit NONE-pass-on-flaky branch + the `bugbot-out-of-band` manual override, so a stuck Bugbot is always escapable without `SKIP_MERGE_GATES`.

## Files to modify

1. `agents/scripts/core/merge-gates.sh:460` — GraphQL projection: append `$bbstate` + `bb_open` fields (mirror the `$crstate` / unresolved-CR-threads computations at `:431-481`). Keep field order append-only — the bash `read` consumes positionally.
2. `agents/scripts/core/merge-gates.sh:375` — parse `bugbot-out-of-band` label (`$bbOob`) beside `$cr` (`cr-out-of-band`).
3. `agents/scripts/core/merge-gates.sh:~640` — extend the bash `read` consumer + add the Bugbot decision bucket (BLOCK on `bb_open>0`; flaky/absent → NONE-pass) mirroring the CR bucket.
4. `agents/scripts/core/merge-gates.sh:950` — `bugbot-out-of-band` downgrade (BLOCK→WARN) beside the `cr-out-of-band` downgrade; emit a `WARN: bugbot-out-of-band …` line.
5. `agents/scripts/core/merge-gates.sh:11` + `:27` + `:356` — header-comment contract: document gate #4 (Bugbot), the `bugbot-out-of-band` override row, and the new tuple field indices (the header already inventories field positions at `:356`).
6. `agents/core/coderabbit-triage.md:66` — replace single-login filter with allow-list `{coderabbitai[bot], cursor[bot]}`; add a Bugbot body-shape parse note (`### <title>` / `**<Sev> Severity**` / `<!-- DESCRIPTION START -->`) + the `couldn't run` / `usage limit` noise-filter to § Process step 2–3.
7. `agents/scripts/core/coderabbit-triage.py` — Python-port sync of #6 (the shared "rules version" marker doctest at end-of-CI fails if `.md` and `.py` disagree — `coderabbit-triage.md:170`).
8. `docs/agent-rules/merge-gates.md` — document the Bugbot gate (#4) + `bugbot-out-of-band` in the override-label table + per-outcome semantics.
9. `AGENTS.md` § Merge gates — one-clause add: Bugbot as a 4th condition + `bugbot-out-of-band` in the override list (stay navigation-only; defer detail to #8). Watch the 150-line cap (`agent_size_audit.py`).
10. `tests/bats/merge_gates.bats` — new cases (see § Verification Bucket A).

## Existing utilities reused

- CR gate machinery — `merge-gates.sh:431-505` (`$crall` / `$crstate` / `$crbody` / unresolved-CR-threads count) is the structural template for `$bbstate` / `bb_open`.
- `cr-out-of-band` label plumbing — `merge-gates.sh:375` (parse) + `:950-966` (downgrade) is the exact pattern for `bugbot-out-of-band`.
- Bot-exclusion idiom — `.author.__typename != "Bot"` (`merge-gates.sh:491-493`) already keeps Bugbot conversation noise out of the user gate; reused as-is (no change to the user-comments count).
- 19-rule override table + subsystem routing table — `coderabbit-triage.md:89-137`, bot-agnostic, applied to Bugbot findings unchanged.
- `merge_gates.bats` harness + GraphQL-fixture pattern — `tests/bats/merge_gates.bats` existing CR cases are the fixture template.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: N/A — agentic shell (bash poller + agent markdown); no product render path touched.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: N/A — no UI thread; the poller runs out-of-process in CI / the watcher.
- **Pillar 3 (never crash)**: jq projection stays total — Bugbot fields default (`// ""`, `// 0`, `ABSENT`) when no `cursor[bot]` artefacts exist, so a Bugbot-free PR yields NONE-pass, never a parse error. Append-only tuple ordering avoids shifting existing field reads.
- **Pillar 4 (accessibility)**: N/A — no user-facing surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

**N/A — diff is agentic-shell + agent-markdown + docs only** (`agents/scripts/core/*.sh|.py`, `agents/core/*.md`, `tests/bats/*`, `docs/`, `AGENTS.md`). No `Source/Core/` C++, no `project.config.json` perf/lint zone, no `SMATCHET_UI_PERF_SCOPE` marker. PR-fast / Pillar-2 scanner / dispatcher-drain / bucket-E / marker-inventory all N/A.

## Risks / non-goals

- **Spend-cap wedge** — a Bugbot usage-limit on the head could block merge forever. *Mitigated*: flaky/absent state → NONE-pass (never blocks) + `bugbot-out-of-band` manual override. This is the single highest-risk behaviour; the bats suite must pin it (usage-limit fixture → PASS).
- **Tuple-order regression** — the GraphQL tuple is positional; inserting (vs appending) a field silently shifts every downstream `read` variable. *Mitigated*: append-only at the tuple tail + update the field-index inventory comment (`merge-gates.sh:356`) in the same edit; bats CR cases must stay green (regression canary).
- **`.md`/`.py` drift** — the triage rule body lives in two files with a CI doctest guard (`coderabbit-triage.md:170`). *Mitigated*: edit both in the same commit; bump the shared rules-version marker.
- **Bugbot login variant** — confirmed `cursor[bot]` today; a Pro/self-hosted variant could differ. *Accepted* — same "confirm against actual JSON before extending the allow-list" caveat the CR filter already carries.
- **Bugbot "Fix all" / suggestion blocks** — Bugbot offers auto-fix links. *Non-goal*: this plan only *gates + triages*; it does not auto-apply Bugbot fixes.
- **Non-goal**: changing CodeRabbit behaviour, the user-comments gate, or `SKIP_MERGE_GATES` semantics.
- **Non-goal**: a label-existence bootstrap — `bugbot-out-of-band` must be `gh label create`d on the repo (a one-time ops step, noted in § Verification), not a code file.

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (bats, `tests/bats/merge_gates.bats`)**: new fixture cases — (1) Bugbot review with `bb_open=2` unresolved `cursor[bot]` threads on head → **BLOCK**; (2) same + `bugbot-out-of-band` label → **WARN/pass** with the downgrade line; (3) `issues/N/comments` body `Bugbot couldn't run - usage limit reached`, no open threads → **NONE-pass** (the spend-cap-no-wedge canary); (4) no `cursor[bot]` artefacts at all → unchanged pass (Bugbot-free PR); (5) existing CR cases stay green (tuple-order regression canary). Run: `bash tests/bats/merge_gates.bats` (or via `scripts/dev/test-all.sh`).
- **`--selftest`**: `bash agents/scripts/core/merge-gates.sh --selftest` (if present) stays green after the header contract-card tokens are updated.
- **Triage `.md`/`.py` sync doctest**: the end-of-CI grep that pins the shared rules-version marker across `coderabbit-triage.md` ↔ `.py` passes (bump the marker in both).
- **Bucket E (ImGui Test Engine)**: N/A — no UI surface.
- **Build gate**: N/A — no C++ in the diff (pure agentic-shell + docs); skip the dual-target build per `is-pure-docs-diff.sh`-class scope. (If any guard insists, `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` is a no-op confirmer.)
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-steps). A red doc-validation job blocks merge even though non-required. Note: `agent_size_audit.py` must stay green after the `AGENTS.md` edit (150-line cap).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms (Bugbot "state" vs "open-count" terminology; "gate #4" naming) before finalising; record the outcome. Required for every plan — do not delete.
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
