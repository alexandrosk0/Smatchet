# Plan — GitHub Issue triage protocol (Issues-canonical for product bugs)

> **Slug**: `issue-triage-protocol` (matches this file's basename without `.md`).
>
> **Usage**: defines the **missing** agentic protocol for GitHub Issues and the boundary between GitHub Issues and the internal self-improvement backlog. Decided via `grill-with-docs` (2026-06-03); core decision recorded in **[ADR-0014](../../adr/0014-github-issues-canonical-for-product-bugs.md)**.
>
> **Mandatory rules cross-link**: `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template. **Docs + config + shell only — no `Source/` diff** → perf-gate section is `N/A`.

## Context

Three "issue-ish" streams exist with **no documented boundary, dedup rule, or autonomous-handling protocol** between them (verified: no `issue-triage.md`, no issue section in `AGENTS.md`, no sweep script):

1. **Product-facing GitHub Issues** — the shipped `log-a-bug-github` feature files user bug + (phase-2) crash reports as Issues on a configured dev repo. Legitimate, intentional.
2. **CodeRabbit auto-created Issues** — `.coderabbit.yaml` `chat.auto_reply: true` + `knowledge_base.issues.scope: auto` caused CR to open **#734** 31 s after an `@coderabbitai` CR-triage reply on PR #733, lifting priority/locations/framing from the comment. Unintended duplication.
3. **Internal self-improvement backlog** — `docs/self-improvement/categories/*.md`; its `bug` category is chartered "defect in shipped behaviour" and held **19 entries, ~all product bugs**, one of which (`bulkImportFutures.clear()`) is the exact dup of #734.

**Decision (ADR-0014):** GitHub Issues are the **canonical tracker for product bugs**; the internal backlog is for the **agentic system itself** (process/tooling/infra/test/security) **+ product tech-debt** (new `debt` category). The backlog's `bug` category is **deprecated**.

## Approach

Six locked decisions (grill-with-docs, 2026-06-03):

1. **Canonical home** — product **bugs** (defects in shipped behaviour) → **GitHub Issues**; backlog = agent-meta + tech-debt.
2. **Existing 19 `bug.md` entries** — **split**: genuine product bugs migrate to Issues; tech-debt/architecture stays in the backlog.
3. **Categories** — **deprecate `bug`, add `debt`**; agent-harness bugs (e.g. cp1252 `comment_audit` crash) fold into `tooling`/`infra`.
4. **Issue creator** — **orchestrator, dedup-first**: on a confirmed pre-existing bug, grep open Issues, and if none, `gh issue create` a structured Issue; CR's stray auto-issues are embraced + reconciled by the sweep.
5. **Labels** — `bug` + **priority `P0`–`P3`** + **`area:<subsystem>`** + a **source marker** (`src:code-review` vs the `log-a-bug` user reports); ~12–14 new labels mirroring the `coderabbit-triage` subsystem map.
6. **Triage cadence** — **closeout sweep + periodic janitor**: `issue-sweep.sh` runs in the ship-loop closeout AND as a scheduled `issue-janitor`.

**The bug-vs-debt classification rule** (deterministic, keyed on observable effect, not judgement): a **bug** = a user-observable defect or a correctness/safety violation (crash, UB, wrong output, data corruption, PII leak, UI freeze > 100 ms, data race, security surface) → **GitHub Issue**. **Tech-debt** = internal maintainability with no user-observable defect (god-object, duplication, coupling, "should refactor", missing abstraction) → **`debt.md`**. Ambiguous → leave in backlog, flag for human, never silently file an Issue.

**Workstreams:**

- **A. Backlog spec change** — deprecate `bug` + add `debt` in `AGENT_SELF_IMPROVEMENT.md` (§ Categories) and the category list in `agents/scripts/core/test-backlog-counts.sh`; create `docs/self-improvement/categories/debt.md`; mark `bug.md` deprecated (header note → "product bugs now live as GitHub Issues; see issue-triage.md").
- **B. Protocol doc** — `docs/agent-rules/issue-triage.md`: the boundary table, the bug-vs-debt rule, the orchestrator dedup-first create flow, the triage decision tree for an *encountered* Issue (keep / relabel / mirror-then-close / flag-stale), autonomous-action rules (auto-act on **bot**-created strays only; never auto-close a **human**-filed Issue), and the Issue↔backlog backlink convention.
- **C. Labels** — create the label set (`gh label create` or a checked-in `agents/scripts/project/sync-issue-labels.sh` from a manifest): `P0`–`P3`, `area:<subsystem>` per the triage map, `src:code-review`. `bug`/`duplicate`/`wontfix` already exist.
- **D. Orchestrator create-flow** — encode the dedup-first `gh issue create` step (structured title/body file:line + repro + labels) into `agents/core/coderabbit-triage.md` and the ship-loop, replacing "append to `bug.md`" for product bugs.
- **E. Sweep + janitor** — `agents/scripts/core/issue-sweep.sh` (`--dry-run` default / `--apply`): classify open Issues by author + body markers + backlog/Issue dedup grep → verdict (`keep` / `relabel` / `mirror-then-close` / `flag-stale`); wire into the ship-loop closeout (`ship-loops.md`) and add an `issue-janitor` (agent + scheduled workflow, mirroring `p4-janitor`/merge-watcher).
- **F. CR reconcile** — `.coderabbit.yaml`: confirm the exact knob; since the orchestrator is the authoritative creator, scope CR so a CR-triage reply does not spawn a competing Issue (or accept it and let the sweep dedup). Document the knob in `issue-triage.md`.
- **G. Migrate the 19** — one-time: scripted bulk `gh issue create` for the genuine product bugs in `bug.md` (label + body from each entry), then remove those entries; move the ~3–4 tech-debt entries to `debt.md`; **#734** becomes canonical for `bulkImportFutures.clear()` → delete that `bug.md` entry. A `--dry-run` migration script prints the plan first for human sign-off before any Issue is created.
- **H. `AGENTS.md` stub** — § Issue triage pointer (≤ 3 lines) next to § Merge gates / § Process rules, citing ADR-0014.

## Files to modify

- `docs/adr/0014-github-issues-canonical-for-product-bugs.md` — **done** (this plan's decision record).
- `docs/agent-rules/issue-triage.md` — **new**. The protocol (B).
- `AGENTS.md` — **edit**. § Issue triage stub + ADR-0014 cross-link (H).
- `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` — **edit**. § Categories: deprecate `bug`, add `debt` (A).
- `agents/scripts/core/test-backlog-counts.sh` — **edit**. Category list: add `debt`; handle deprecated `bug` (A).
- `docs/self-improvement/categories/debt.md` — **new** + `bug.md` header-deprecate + migrate entries (A, G).
- `agents/core/coderabbit-triage.md` — **edit**. Product-bug → dedup-first `gh issue create` instead of `bug.md` (D).
- `docs/agent-rules/ship-loops.md` — **edit**. Add issue-sweep to the closeout sequence (E).
- `agents/scripts/core/issue-sweep.sh` — **new** + `tests/bats/issue_sweep.bats` (E).
- `agents/core/issue-janitor.md` + scheduled workflow — **new** (E).
- `agents/scripts/project/sync-issue-labels.sh` (+ manifest) — **new** (C).
- `.coderabbit.yaml` — **edit**. Issue auto-create policy (F).
- `agents/scripts/project/migrate-bugs-to-issues.sh` — **new**, one-time, `--dry-run` default (G).
- `CONTEXT-MAP.md` / doc-coverage gate — **check**. Register the new `docs/agent-rules/` doc if gated.

## Existing utilities reused

- `gh issue list/view/create/close/comment/label` + `gh api` — same CLI the ship-loop uses for PRs.
- `agents/scripts/core/merge-gates.sh` `gh api graphql` + `jq` + halt-prompt idioms — the sweep mirrors its shape.
- `agents/scripts/core/test-shell-lint.sh` (5-rule) + `tests/bats/*.bats` harness — for `issue-sweep.sh` + `migrate-*`.
- `agents/core/coderabbit-triage.md` subsystem-routing map — source for the `area:<subsystem>` labels.
- `agents/core/p4-janitor.md` / `git-janitor` / merge-watcher closeout shape — template for `issue-janitor`.
- `log-a-bug-github` Issue-body template — the classifier keys on its markers to tell user-reports from CR findings.
- self-improvement backlog format + `test-backlog-counts.sh` — the `debt` category + `mirror-then-close` path reuse it.

## UX Pillar callouts

`N/A — docs + shell + config only; zero runtime / UI-thread code. The product log-a-bug path this references carries its own Pillar-2/3 review in its shipped plan; this plan does not touch it.`

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A — no Source/ diff. Touches docs/, AGENTS.md, .coderabbit.yaml, agents/scripts/, agents/core/, and tests/bats/ only.`

**Pre-push local check**: `N/A` (no perf scenario). `bash scripts/dev/pre-ship.sh` + shell-lint + bats still run.

**Override**: `N/A`.

## Risks / non-goals

- **Risk — auto-closing a real user Issue.** Mitigation: the sweep auto-acts only on **bot**-authored strays matching a verified duplicate; every **human**-authored Issue is report-only. `--dry-run` default.
- **Risk — migration spams the tracker / mis-classifies a tech-debt item as a bug.** Mitigation: `migrate-bugs-to-issues.sh --dry-run` prints the full bug-vs-debt classification + the Issues it *would* create for human sign-off before any `gh issue create`.
- **Risk — CR keeps auto-creating competing Issues after the config change.** Mitigation: verify the knob against CR docs; the sweep's `relabel`/`mirror-then-close` reconcile is the idempotent backstop.
- **Risk — `bug`-category deprecation breaks `test-backlog-counts.sh` / existing tooling that enumerates categories.** Mitigation: grep every consumer of the category list (`test-backlog-counts.sh`, `AGENT_SELF_IMPROVEMENT.md`, the memory-drain + self-improvement skills) before the spec change; deprecate gracefully (keep `bug.md` readable, just frozen) rather than delete.
- **Risk — `area:<subsystem>` labels drift from the `coderabbit-triage` subsystem map.** Mitigation: generate them from one manifest (`sync-issue-labels.sh`); a cheap check asserts manifest ↔ triage-map parity.
- **Non-goal**: GitHub Projects / milestones automation; an Issue UI; the phase-2 crash reporter.
- **Non-goal**: changing `log-a-bug-github` product behaviour or destination repo.
- **Non-goal**: migrating `process`/`tooling`/`infra`/`test`/`security` backlog entries — only the `bug` category is affected.

## Verification

- **Bats (`tests/bats/issue_sweep.bats`)**: fixtures per decision-tree branch — `app/coderabbitai` dup → `mirror-then-close`/`relabel`; human `log-a-bug` Issue → `keep`; unlabeled bug → `flag-relabel`; obsolete → `flag-stale`. Assert `--dry-run` mutates nothing.
- **Bats (`tests/bats/migrate_bugs_to_issues.bats`)**: a fixture `bug.md` → assert the script classifies bug-vs-debt correctly and `--dry-run` creates zero Issues.
- **Live dry-runs**: `issue-sweep.sh --dry-run` MUST classify **#734** as a bot-dup of the `bulkImportFutures` entry; `migrate-bugs-to-issues.sh --dry-run` MUST split the 19 into ~15 bugs / ~4 debt matching a hand-audit.
- **Spec integrity**: `test-backlog-counts.sh` green after the `bug`→deprecated / `debt`-added change; every category-list consumer updated (grep-verified).
- **Labels**: `sync-issue-labels.sh` creates `P0`–`P3` + `area:*` + `src:code-review`; manifest ↔ triage-map parity check passes.
- **Shell-lint + pre-ship**: clean on all new scripts; `test-lint-rules.sh --diff` + doc-anchor/plan-index gates green.
- **CR-config (manual, one-shot)**: after the `.coderabbit.yaml` change, a fresh `@coderabbitai` CR-triage reply on a test PR does not spawn a competing Issue (or the sweep reconciles it).
- **End-to-end**: apply the protocol to #734 — confirm the `bug.md` dup is removed and #734 is the labelled canonical record.

## Out of scope (flagged, not designed)

- **Phase-2 in-process crash reporter** → Issues (deferred in `log-a-bug-github`; own plan).
- **GitHub Projects / milestones / a broader label taxonomy** beyond `bug` + `P0–P3` + `area:*` + `src:*`.
- **Migrating non-`bug` backlog categories** to Issues.
- **A real-time issue-triage daemon** beyond the closeout sweep + scheduled janitor — promote to higher frequency only if volume warrants.
- **Changing `log-a-bug-github`** product behaviour.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
