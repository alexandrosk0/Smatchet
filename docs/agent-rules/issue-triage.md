# GitHub Issue triage protocol

> The agentic protocol for GitHub Issues + the boundary between Issues and the
> internal self-improvement backlog. Decision: [`ADR-0014`](../adr/0014-github-issues-canonical-for-product-bugs.md)
> (GitHub Issues are canonical for product bugs). Plan: [`docs/plans/shipped/issue-triage-protocol.md`](../plans/shipped/issue-triage-protocol.md).
> AGENTS.md keeps a ≤3-line § Issue triage stub pointing here.

## Boundary — which tracker owns what

| Stream | Home | What goes here |
|---|---|---|
| **Product bugs** (defects in shipped behaviour) | **GitHub Issues** | crash, UB, wrong output, data corruption, PII leak, UI freeze > 100 ms, data race, security surface — anything user-observable or a correctness/safety violation |
| **Product tech-debt** | **`docs/self-improvement/categories/debt.md`** | internal maintainability with NO user-observable defect: god-object, duplication, coupling, "should refactor", missing abstraction |
| **Agent-system self-improvement** | `docs/self-improvement/categories/{process,tooling,infra,test,security}.md` | friction in the agentic harness itself — including bugs *in the harness scripts* (e.g. a `comment_audit.py` cp1252 crash folds into `tooling`/`infra`, NOT a product bug) |
| **User / crash reports** | **GitHub Issues** | the shipped `log-a-bug-github` intake (user bug reports + phase-2 crash reports) |

The backlog's old **`bug`** category is **deprecated** — that is exactly what now lives in GitHub Issues. `bug.md` stays readable (frozen header) but takes no new entries.

## The bug-vs-debt rule (deterministic — keyed on observable effect, not judgement)

- **Bug** → **GitHub Issue**: a user-observable defect OR a correctness/safety violation. Test: *"would a user, or a sanitizer/correctness gate, observe this as wrong?"* (crash, UB, wrong output, data corruption, PII leak, UI freeze > 100 ms, data race, exploitable surface).
- **Tech-debt** → **`debt.md`**: internal maintainability with no user-observable defect (god-object, duplication, coupling, missing abstraction, "should refactor").
- **Ambiguous** → leave in the backlog, flag for a human, **never silently file an Issue**.

A code-review / CodeRabbit finding about a *real product bug* is a GitHub Issue, not a backlog entry.

## Orchestrator create-flow (dedup-first)

When a **confirmed pre-existing product bug** is encountered (e.g. during CR triage of an unrelated PR):

1. **Dedup** — `gh issue list --state open --search "<key terms>"` (and grep `bug.md` for legacy dups). If an open Issue already covers it → comment a backlink, stop.
2. **Create** — if none, `gh issue create` a **structured** Issue:
   - **Title**: `bug(<area>): <symptom>` (imperative, file:line if known).
   - **Body**: symptom · repro/trigger · `file:line` · suspected cause · the PR/commit that surfaced it.
   - **Labels**: `bug` + priority (`P0`–`P3`) + `area:<subsystem>` + a source marker (`src:code-review` for agent-found; the `log-a-bug` template marks user reports).
3. **Never** append a product bug to `bug.md` — that path is replaced by the Issue create.

Priority maps to the same scale as the backlog: `P0` data-corruption/exploitable/crash-on-launch · `P1` load-bearing/silent-failure/regression · `P2` real but bounded · `P3` cosmetic/minor.

## Triage decision tree — an *encountered* open Issue

Run per open Issue (the `issue-sweep.sh` automates this; verdict keys on **author** + body markers + dedup):

| Condition | Verdict |
|---|---|
| **Bot-authored** (`app/coderabbitai` etc.) AND a verified duplicate of an existing Issue / migrated bug | **`mirror-then-close`** — backlink to canonical, close the stray |
| **Bot-authored**, real + not a dup, but unlabeled / mislabeled | **`relabel`** — add `bug` + `P?` + `area:*` + `src:code-review`, keep open |
| **Human-authored** (`log-a-bug` user report or a maintainer) | **`keep`** — report-only; relabel suggestions allowed, but **never auto-close a human Issue** |
| Real but obsolete (fixed / code removed) | **`flag-stale`** — surface for human close; never auto-close unless bot-authored + provably fixed |
| Unlabeled product bug, real | **`flag-relabel`** — propose labels, apply if bot-authored |

**Autonomous-action rule:** the sweep may auto-act (close / relabel) **only** on **bot**-authored Issues. Every **human**-authored Issue is **report-only** — surfaced for a human, never auto-mutated beyond an additive label suggestion. `--dry-run` is the default; `--apply` performs the mutations.

## Issue ↔ backlog backlink convention

- When a backlog tech-debt item later proves to be a user-observable bug, file an Issue and add `→ #<n>` to the `debt.md` entry (or move it).
- When closing a bot-stray as a dup (`mirror-then-close`), the close comment links the canonical Issue/entry: `Duplicate of #<n> — <one-line>`.
- A migrated `bug.md` entry's Issue records its origin in the body (`migrated from bug.md`), and the `bug.md` entry is deleted (the Issue is now canonical).

## Fixing an Issue — the elevation flow

Triage *tracks + labels* Issues; **fixing one is user-initiated and label-routed** (the loop never autonomously works a product bug):

0. **Dedup pre-check (BEFORE dispatching any fix agent)** — confirm the finding isn't *already fixed*. A historical-review sweep can file a **duplicate** Issue for a finding whose original Issue was already closed by a merged fix (e.g. finding #948 was fixed by PR #959 / Issue #954 CLOSED, yet the sweep filed #1002 for the same finding — dispatching a fix agent against #1002 was a pure no-op). Run [`agents/scripts/core/finding-already-fixed.sh`](../../agents/scripts/core/finding-already-fixed.sh)` <finding-ref> [<fix-token>]` (it greps **closed** Issues for the finding ref + `git log -S <token> origin/develop` for a fix already on develop) — or do the two checks by hand (`gh issue list --state closed --search <ref>` + `git log -S <token> origin/develop`). If it reports an already-fixed signal, **read the cited `file:line` on develop to confirm it's genuinely fixed, then close the duplicate Issue with a pointer to the real fix instead of spawning an agent.**
1. **Elevate** — `gh issue view <n>` to read symptom · repro · `file:line`; `gh issue develop <n> -b develop` creates + links a `<n>-<slug>` fix branch.
2. **Route** — the **`area:<subsystem>`** label names the specialist directly (it mirrors the `coderabbit-triage` path→agent map): `area:tracker-backend` → `tracker-backend`, `area:grid-engine` → `grid-engine`, `area:offline-sync` → `offline-sync`, etc. **`P0–P3`** sets urgency.
3. **Fix** — run the normal ship-loop on that branch; the PR body carries **`Fixes #<n>`** so GitHub auto-closes the Issue on merge into the **default branch** (`develop`). **One closing keyword per Issue** — a *batched* fix PR (one PR closing several Issues, per the PR-batching rule) needs the keyword repeated: `Fixes #<a>` / `Fixes #<b>` / … (or `Fixes #<a>, fixes #<b>`). A bare comma-list `Fixes #<a>, #<b>` links **only the first** Issue — the rest silently stay open. Verify the real linkage before merge with `gh pr view <pr> --json closingIssuesReferences`. The sweep/janitor then sees it closed — no manual close step.

**Auto-propose, never auto-fix (the guardrail).** At ship-loop closeout the sweep, *after* triaging strays, **surfaces the highest-priority open bug as a proposal** — a `[issue-propose] #<n> <title> (P<k>, area:X) — elevate? gh issue develop <n>` line for the top open `P0` (then `P1`) — so the most urgent bug never sits unnoticed. It does **NOT** start a fix and does **NOT** pause the loop. The human elevates (`fix #<n>`); agents *propose* the next bug, the human *decides*. This keeps the never-silently-mutate-product-behaviour posture: the loop auto-files, auto-triages, and auto-*proposes*, but a product-code fix is always human-initiated.

## Labels

Managed by [`agents/scripts/project/sync-issue-labels.sh`](../../agents/scripts/project/sync-issue-labels.sh) from a checked-in [manifest](../../agents/scripts/project/issue-labels.manifest) (so the set is reproducible + the `area:*` labels stay in parity with the `coderabbit-triage` subsystem map):

- **`bug`** (exists) — every product bug Issue.
- **`P0`–`P3`** — priority, the backlog scale.
- **`area:<subsystem>`** — mirrors the `agents/core/coderabbit-triage.md` path→agent routing map (`area:tracker-backend`, `area:grid-engine`, `area:offline-sync`, `area:command-system`, `area:mcp-toolsmith`, `area:p4-annotate`, `area:ui`, …).
- **`src:code-review`** — agent-found (vs the `log-a-bug` user-report marker in the report body).
- `duplicate` / `wontfix` already exist.

## CodeRabbit auto-Issue reconciliation

`.coderabbit.yaml` `knowledge_base.issues.scope: auto` + `chat.auto_reply: true` let CR open Issues from `@coderabbitai` CR-triage replies (this is what created the #734 stray). The orchestrator is the **authoritative** creator, so CR's strays are **embraced + reconciled** by `issue-sweep.sh` (`mirror-then-close` / `relabel`) rather than fought. If CR-strays become noisy, tighten the `issues.scope` knob — but the sweep's reconcile is the idempotent backstop regardless.

## Triage cadence

- **Closeout sweep** — `issue-sweep.sh --dry-run` runs in the ship-loop closeout: surfaces triage verdicts (`--apply` only on explicit authorisation) **and** emits the top-`P0`/`P1` `[issue-propose]` line (§ Fixing an Issue) — propose-only, never auto-fix, never pauses.
- **Periodic janitor** — [`issue-janitor`](../../agents/core/issue-janitor.md) (agent + scheduled `issue-janitor.yml` workflow, mirroring `p4-janitor` / `git-janitor`) keeps Issues labelled / deduped / stale-swept off the ship-loop.
