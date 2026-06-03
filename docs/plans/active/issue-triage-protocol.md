# Plan — GitHub Issue triage protocol (autonomous issue handling)

> **Slug**: `issue-triage-protocol` (matches this file's basename without `.md`).

> **Usage**: this plan defines the **missing** agentic protocol for GitHub Issues. Today there is none — `docs/agent-rules/` covers PRs / ship-line / internal backlog exhaustively but says nothing about GitHub Issues, so an Issue like #734 (CodeRabbit-auto-created from a CR-triage `@coderabbitai` reply on PR #733) sits in a tracking system the loop has no rules for, duplicating the internal `bug.md` entry.

> **Mandatory rules cross-link**: `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template. This plan is **docs + config only** (no `Source/` diff) → perf-gate section is `N/A`.

## Context

Three distinct "issue-ish" streams exist in Smatchet today, with **no documented boundary, dedup rule, or autonomous-handling protocol** between them:

1. **Product-facing GitHub Issues** — the shipped **`log-a-bug-github`** feature (`docs/plans/shipped/log-a-bug-github.md`) files **user bug reports + (phase-2) crash reports** as GitHub Issues on a fixed configured dev repo, with runtime context inlined. This is a **legitimate, intentional** use of GitHub Issues as user-facing intake.
2. **CodeRabbit auto-created Issues** — CR's `.coderabbit.yaml` `knowledge_base.issues.scope: auto` + its chat behaviour caused it to open **issue #734** 31 s after an `@coderabbitai` CR-triage reply on PR #733 (the reply stated intent to "capture … as a backlog item (bug, P2)"). CR lifted the priority, file locations, and "pre-existing/behaviour-preserving" framing straight from the comment. **Unintended** — the orchestrator meant to file it internally.
3. **Internal self-improvement backlog** — `docs/self-improvement/categories/{bug,process,tooling,test,infra,security}.md` is the canonical agent work-tracker (format/triage spec in `AGENT_SELF_IMPROVEMENT.md`). The `bulkImportFutures.clear()` bug was filed here (via #733) **and** auto-mirrored into #734 → the same bug now lives in **two** trackers.

The gap: there is **no `docs/agent-rules/issue-triage.md`**, no issue-handling section in `AGENTS.md`, no skill, and no sweep script (verified by grep across `AGENTS.md`, all of `docs/agent-rules/`, `agents/`, `.claude/`). So the autonomous loop has no rule for: *when I encounter / am about to create a GitHub Issue, what do I do?* — and no closeout step that reconciles open Issues against the internal backlog. Left alone, CR auto-issues accumulate untracked and silently duplicate `bug.md`.

This plan defines the **canonical boundary** between the three streams and a **deterministic triage + autonomous-handling protocol** the ship-loop and a periodic janitor can execute.

## Approach

**Core decision — the canonical boundary (recommended; flag for maintainer sign-off in stress-test):**

| Stream | Canonical home | Rationale |
|---|---|---|
| **Product bug / crash reports** (user- or crash-reporter-filed via `log-a-bug-github`) | **GitHub Issues** | User-facing intake; external visibility; already wired to a real repo. |
| **Agent self-improvement** (process / tooling / infra / test friction, prompt fixes, gate gaps) | **Internal backlog** (`docs/self-improvement/`) | Meta-work on the agentic system; machine-local triage cadence; never user-facing. |
| **Code-review findings on real product bugs** (CR-triage, like #734) | **Internal `bug.md`** is canonical; **no GitHub Issue** | Keeps one source of truth; avoids the #734 double-track. Surfaced during a PR, owned by the dev loop, not the user. |

i.e. **GitHub Issues = user/external intake only; the internal backlog = everything the agent loop generates.** A CR-triage product bug is dev-loop-generated → backlog, not a GitHub Issue.

**Workstreams:**

1. **Write `docs/agent-rules/issue-triage.md`** — the protocol doc. Contents:
   - The canonical-boundary table above (which stream → which home).
   - A **triage decision tree** for an agent that *encounters* an open GitHub Issue: classify by author + content → (a) **product bug/crash** (log-a-bug / external user) → keep open, label, leave for human product triage, do **not** auto-fix unless in scope; (b) **CR/bot-auto-created duplicate of an internal backlog item** → close with a comment pointing at the canonical `bug.md`/category entry (single source of truth); (c) **agent-meta** mis-filed as an Issue → mirror into the internal backlog, then close with a pointer; (d) **stale/obsolete** → close with reason.
   - **Autonomous-action rules**: what an agent MAY do without asking (close a verified bot-created duplicate with a pointer; relabel; mirror→backlog) vs MUST pause for (closing a *user-filed* product issue; acting on an issue that implies a product behaviour change → that's a normal ship-loop task, not issue-triage).
   - **Anti-duplication rule**: before filing a `bug.md` entry from a CR-triage reply, do **not** `@`-address the reply in a way that trips CR auto-issue creation (see workstream 4); and before opening any Issue, grep the internal backlog for the same symbol to avoid double-track.
   - **Backlink convention**: Issue ↔ backlog entry cross-reference shape (`see docs/self-improvement/categories/bug.md` ↔ `tracks #N`).

2. **Bake an issue-sweep into the ship-loop closeout** (`docs/agent-rules/ship-loops.md`) — at end-of-session / end-of-program closeout (the same place the plan-doc closeout lives, per the process/P2 lesson just backlogged), run the sweep: list open Issues, classify per the decision tree, auto-close verified bot-duplicates with pointers, and **report** the residue (user-filed product issues) for human triage. Mirrors the existing `git-janitor` / merge-watcher closeout shape.

3. **`agents/scripts/core/issue-sweep.sh`** (new, portable) — `gh issue list --json …` → classify each open issue by author (`app/coderabbitai` / bot vs human), title/body markers (the `log-a-bug` body template vs a CR-finding shape), and a backlog-grep for the same symbol → emit a per-issue verdict (`keep` / `close-duplicate` / `mirror-then-close` / `flag-stale`). `--dry-run` (default, just reports) vs `--apply` (executes the safe closes). Shell-lint-clean; bats-tested with fixtures. Reuses the `merge-gates.sh` `gh api` + jq idioms.

4. **Reconcile CodeRabbit auto-issue creation** (`.coderabbit.yaml`) — decide + encode whether CR should auto-create Issues at all. Given the canonical boundary (CR findings → internal backlog, not Issues), the recommended setting suppresses CR issue auto-creation (or scopes it so a CR-triage reply does **not** spawn an Issue), keeping `knowledge_base.issues.scope` only as a *read* source. Document the exact key in the protocol doc so it's discoverable. (Confirm the precise CR config knob during implementation — `knowledge_base.issues.scope` governs reading; the auto-create trigger may be a separate chat/behaviour setting that needs a support-doc check.)

5. **`AGENTS.md` stub + cross-link** — a 1-2 line § Issue triage pointer to `docs/agent-rules/issue-triage.md`, placed next to § Merge gates / § Process rules.

6. **Reconcile the live #734 incident** — as the protocol's first application: confirm the `bug.md` entry is canonical, close #734 with a pointer to it (the (b) path), proving the decision tree end-to-end.

## Files to modify

- `docs/agent-rules/issue-triage.md` — **new**. The protocol: boundary table + decision tree + autonomous-action rules + anti-dup + backlink convention.
- `AGENTS.md` — **edit**. § Issue triage stub + cross-link (≤ 3 lines).
- `docs/agent-rules/ship-loops.md` — **edit**. Add the issue-sweep step to the closeout sequence.
- `agents/scripts/core/issue-sweep.sh` — **new**. Classify + (dry-run/apply) sweep of open Issues.
- `tests/bats/issue_sweep.bats` — **new**. Fixture-driven classification tests (bot-dup, log-a-bug product, mirror-then-close, stale).
- `.coderabbit.yaml` — **edit**. Issue auto-create policy per the decided boundary.
- `docs/self-improvement/categories/bug.md` — **edit (small)**. Add the `tracks #734` backlink to the existing `bulkImportFutures.clear()` entry (and #734 gets closed pointing back).
- `CONTEXT-MAP.md` / doc-coverage gate — **check**. If `docs/agent-rules/` additions are gated for index sync, register the new doc.

## Existing utilities reused

- `gh issue list/view/close/comment` (+ `gh api`) — same CLI the ship-loop already uses for PRs.
- `agents/scripts/core/merge-gates.sh` `gh api graphql` + `jq` + halt-prompt idioms — the sweep mirrors its shape.
- `agents/scripts/core/test-shell-lint.sh` (5-rule gate) + the `tests/bats/*.bats` harness — for the new script.
- The self-improvement backlog format + `test-backlog-counts.sh` — the mirror-into-backlog path writes a standard entry.
- `log-a-bug-github` issue-body template (`GitHubClient::BuildCreatePayload`) — the classifier keys on its markers to recognise product reports vs CR findings.
- `git-janitor` / `smatchet-merge-watcher` closeout pattern — the issue-sweep slots in alongside.

## UX Pillar callouts

`N/A — docs + shell + config only; zero runtime / UI-thread code. (The product `log-a-bug` path this protocol references already carries its own Pillar-2/3 review in its shipped plan; this plan does not touch it.)`

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A`)

`N/A — no Source/Core/ (or any Source/) diff. Touches docs/, AGENTS.md, .coderabbit.yaml, and agents/scripts/core/ + tests/bats/ only.`

**Pre-push local check**: `N/A` (no perf scenario). Standard `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` + `bash scripts/dev/pre-ship.sh` still run; shell-lint + bats for the new script.

**Override**: `N/A`.

## Risks / non-goals

- **Risk — auto-closing a real user issue.** Mitigation: the sweep only auto-closes issues whose author is a **bot** AND that match a verified internal-backlog duplicate; **every human-authored issue is report-only** (never auto-closed). `--dry-run` is the default.
- **Risk — CR re-creates issues after a config change.** Mitigation: verify the exact `.coderabbit.yaml` knob against CR's current docs during implementation; if auto-create isn't fully suppressible, the sweep's `close-duplicate` path is the backstop (idempotent — re-closes on next sweep).
- **Risk — boundary ambiguity (is X a product bug or agent-meta?).** Mitigation: the decision tree keys on **author + body template**, not judgement; ambiguous → `flag-stale`/report, never auto-act.
- **Risk — the canonical-boundary decision is a project-policy call.** Mitigation: this plan **recommends** GitHub-Issues-for-user-intake / backlog-for-everything-else but flags it for maintainer sign-off in the `grill-with-docs` stress-test before any code lands.
- **Non-goal**: migrating existing `bug.md` entries into GitHub Issues (or vice-versa) wholesale — the boundary applies forward.
- **Non-goal**: building an issue UI, GitHub Projects/milestones automation, or the phase-2 crash reporter (its own deferred plan).
- **Non-goal**: changing the `log-a-bug-github` product feature's behaviour.

## Verification

- **Bats (`tests/bats/issue_sweep.bats`)**: fixtures for each branch of the decision tree — (a) `app/coderabbitai` issue duplicating a `bug.md` symbol → verdict `close-duplicate`; (b) a `log-a-bug` product-template issue from a human → verdict `keep`; (c) agent-meta mis-filed → `mirror-then-close`; (d) obsolete → `flag-stale`. Assert `--dry-run` mutates nothing.
- **Live dry-run**: `bash agents/scripts/core/issue-sweep.sh --dry-run` on the real repo MUST classify **#734** as `close-duplicate` (pointing at the `bug.md` `bulkImportFutures.clear()` entry) and leave any human-filed issues `keep`.
- **Shell-lint**: `bash agents/scripts/core/test-shell-lint.sh agents/scripts/core/issue-sweep.sh` clean.
- **Doc-validation**: `issue-triage.md` reachable from `AGENTS.md`; plan-index / doc-anchor gates green; `test-subsystem-docs.sh` if it gates `docs/agent-rules/`.
- **CR-config**: after the `.coderabbit.yaml` change, confirm a fresh `@coderabbitai` CR-triage reply on a test PR does **not** spawn a new Issue (manual, one-shot — the only human-in-the-loop check).
- **Protocol self-test**: apply the decision tree to #734 by hand → close it with a `bug.md` pointer; confirm single-source-of-truth restored.

## Out of scope (flagged, not designed)

- **Phase-2 in-process crash reporter** → GitHub Issues (deferred in `log-a-bug-github`; its own plan).
- **Bulk migration** of internal `bug.md` ↔ GitHub Issues.
- **GitHub Projects / milestones / label taxonomy** beyond the minimal classify-and-close.
- **A merge-watcher-style daemon** for continuous issue triage — start with the closeout sweep + manual `issue-sweep.sh`; promote to a janitor/cron only if volume warrants.
- **Changing `log-a-bug-github`** product behaviour or destination repo.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
