# Plan — Backlog PR roadmap (open-entry → PR grouping)

> **Slug**: `backlog-pr-roadmap` (matches this file's basename without `.md`).
>
> **Status**: `active` — a living tracker, not a single-feature plan. Each PR group below is `proposed` until a branch opens; flip its row to `in-flight` / `shipped (#N)` as work lands. This file flips to `shipped` and moves to `docs/plans/shipped/` only once the roadmap is exhausted or explicitly retired.

## Context

A full **multi-agent trap-sweep** (18 verifier agents, every verdict git-cited) over all 205 open/partial self-improvement backlog entries reconciled 37 stale-status rows (PRs #1493/#1494/#1496) and left **151 genuinely-open entries** verified accurate. This doc groups those 151 into coherent, owner-aligned PRs so the backlog becomes an actionable roadmap instead of a flat pile. Intended outcome: *after this lands, every open backlog item has a named home PR + owner, ordered so infrastructure ships before its dependents.*

Source data: the trap-sweep run (`wux8jz521`, run `wf_ca0057a0-57d`). Reconciliation PRs: #1493, #1494, #1496.

## Approach

Cluster the open entries by **subsystem + owning specialist** (per the delegation tables in `docs/agent-rules/delegation.md`), not by category file — entries that touch the same code/scripts ship and review together. Sequence so shared infrastructure precedes its consumers: the bucket-E harness fixes (PR-9) before the bucket-E scenario PRs (PR-10/11); the sanctioned merge wrapper (PR-1) before the rest of the merge-gate hardening. P3 entries are siblings of P2 clusters and fold into the matching PR rather than spawning their own.

This is a **batching map**, not an implementation design — each PR group gets its own plan doc (or a lightweight ship-loop) when picked up. Rows carry the entry's backlog slug so the owning agent can pull full detail + git-cited evidence from `docs/self-improvement/categories/`.

## Files to modify

This roadmap doc only (`docs/plans/active/backlog-pr-roadmap.md`). Each PR group below names its own target files when picked up; they are not pre-listed here (a batching map, not a design).

## Existing utilities reused

- `docs/agent-rules/delegation.md` — the subsystem-specialist → owner mapping each PR row cites.
- `docs/self-improvement/categories/*.md` — the open-entry source rows (slug-referenced per PR group).

## PR groups (P2 — actionable tier)

### Merge-gate / ship-loop tooling

| PR | Members (backlog slug) | Owner | Status |
|---|---|---|---|
| PR-1 Sanctioned merge wrapper + gate visibility | `intent-gate-safe-merge-wrapper`, `green-pr-blocked-no-merge-signal`, `auto-merge-poller-default`, `auto-merge-armed-before-final-push-orphans-commit`, `verify-unresolved-review-threads-vs-head` | agentic-infra | proposed |
| PR-2 CodeRabbit handling | `cr-review-skipped-pure-docs-auto-downgrade`, `cr-rate-limit-code-pr-auto-pause`, `coderabbit-current-head-helper`, `coderabbit-plan-ref-convention-path-instruction` | agentic-infra | proposed |
| PR-3 Merge-watcher robustness | `merge-snapshot-ledger-uncommitted-loss-risk`, `merge-watcher-triage-attempts-unbounded`, `merge-watcher-agent-notify`, `merge-gate-absence-blind-nonrequired-allowlist`, `bucket-lane-status-broken-sentinel-auditable` | agentic-infra | proposed |

### Lint / gate authoring

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-4 Comment-noise + new-file lint reflex | `comment-blank-run-flags-single-doc-paragraph-separator`, `agent-headers-trip-comment-noise-gate`, `comment-noise-gate-reds-required-build`, `new-file-delta-lint-reflex`, `build-verify-shortcut-bypasses-lint-gate` | build-doctor | proposed |
| PR-5 New strict-zone lint rules | `ban-bare-json-parse-on-untrusted-ingress`, `concurrency-correctness-no-headless-test-home` (g_ui write rule), `function-size-audit-grandfather-blind`, `fail-open-meta-gate-authoring-check` | build-doctor | proposed |
| PR-6 Gate false-positive fixes | `lint-syntax-both-pch-version-drift-fp`, `fuzz-target-include-closure-unresolved-invisible` | build-doctor | proposed |

### Plan-doc / index tooling

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-7 plan-index robustness | `shallow-clone-corrupts-git-log-date-generators`, `test-plan-index-shallow-clone-corrupts-date-sort`, `plan-index-fix-wrong-cwd-silent-noop`, `test-plan-index-fix-shipped-date-placeholder`, `markdown-links-local-passes-ci-fails-after-plan-archive` | mechanic | proposed |
| PR-8 Plan-staleness gates | `archive-staleness-check`, `plan-doc-postship-closeout-stale-active-gate`, `extraction-sizing-step`, `historical-review-ledger-staleness` | mechanic | proposed |

### Bucket-E test coverage

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-9 Bucket-E harness fixes *(unblocks PR-10/11)* | `bucket-e-uitestscenario-no-live-local-cache`, `bucket-e-failures-blind-stdout`, `faketrackerclient-fetch-queue-auto-sticky`, `bucket-e-ci-fixture-env-export`, `fakep4runner-spawn-fail-vs-timeout` | test-rig | proposed |
| PR-10 Bucket-E: grid / views | `multigrid-slice3-lifecycle-bucket-e`, `data-dependent-windows-bucket-e-render`, `grid-description-tooltip-bucket-e`, `views-editor-field-selection-bucket-e`, `user-info-window-bucket-e-coverage`, `keybindings-editor-rebind-bucketE-residue`, `help-marker-hover-fallback-bucket-e` | grid-engine / ui-host | proposed |
| PR-11 Bucket-E: AI chat | `ai-chat-bucket-e-coverage` (5 scenarios) | test-rig | proposed |

### AI / assistant subsystem

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-12 AI client tests | `aiclientcancel-per-client-regression`, `per-client-error-body-redaction-gate`, `aiassistant-streaming-scenarios-s2-s4-s5` | tracker-backend / test-rig | proposed |
| PR-13 AI prefs/controller bug fixes | `assistant-prefs-3-bugs`, `aiassistantcontroller-3-loads`, `whisper-prefs-4-bugs`, `imgui-define-macro`, `whisper-local-backend-default-flip-decision` | tracker-backend | proposed |

### Coverage / build / hygiene / infra

| PR | Members | Owner | Status |
|---|---|---|---|
| PR-14 Raise core coverage 65→70 | `raise-core-coverage-67-to-70`, `backend-impl-coverage-recovery` | test-rig | proposed |
| PR-15 CMake / CI robustness | `cmake4-fresh-configure-drops-ehsc`, `fetchcontent-cache-path-drift`, `advisory-ci-step-level-template`, `ubsan-merged-without-executing-validation` | build-doctor | proposed |
| PR-16 Worktree / session-registry + branch-edit guards | `session-registry-liveness-followups`, `edit-on-merged-pr-branch-reverts-develop`, `campaign-sibling-prs-edit-shared-plan-doc-thrash`, `decomposition-prs-serial-conflict-shared-files` | git-janitor | proposed |
| PR-17 Ship-loop discipline rules (docs) | `not-started-status-verified-against-merged-code`, `review-before-commit-hardening`, `adversarial-rca-before-coding`, `security-review-plan-time-trust-boundary`, `ship-time-issue-elevation-check`, `exe-auto-launch-diff-trigger`, `ci-config-slice-dup-preflight` | docs | proposed |

### Standalone subsystem items

| PR | Member | Owner | Status |
|---|---|---|---|
| PR-18 Pink-clear dock-gap scan | `pink-clear-dock-gap-scan` | ui-host | proposed |
| PR-19 DX12 backbuffer readback screenshot | `dx12-backbuffer-readback-screenshot-diff` | unreal-bridge | proposed |
| PR-20 Tracker redirect no-follow regression | `tracker-redirect-no-follow-regression-test` | tracker-backend / security | proposed |
| PR-21 Per-pane catalog value-read routing | `per-pane-catalog-value-read-routing` | grid-engine | proposed |
| PR-22 Portable-layer + daemon hardening | `de-smatchetify-portable-layer`, `daemon-loop-per-iteration-backstop-audit`, `cr-rate-limit-code-pr-auto-pause` (infra half) | build-doctor / agentic-infra | proposed |

## P3 entries (73) — fold into matching P2 PR

The 73 P3 opens are smaller siblings of the clusters above (more bucket-E scenarios, more gate helpers/FP-fixes, doc-convention tweaks, P4-layer polish, minor UI/test gaps). They ride the matching P2 PR rather than spawning their own — e.g. a P3 plan-index tweak rides PR-7, a P3 bucket-E scenario rides PR-10. Full list: query the trap-sweep verdicts or `grep 'P3' docs/self-improvement/categories/*.md`.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact — this is a docs-only roadmap; perf belongs to each PR when picked up.
- **Pillar 2 (UI-thread)**: no impact (docs only).
- **Pillar 3 (never crash)**: no impact (docs only).
- **Pillar 4 (accessibility)**: no impact (docs only).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A`)

`N/A` — this PR touches only `docs/plans/active/`; no `Source/Core/` diff. Each member PR declares its own perf gates when authored.

## Risks / non-goals

- **Risk: roadmap rots as PRs land.** Mitigation: each PR's ship-loop flips its row to `shipped (#N)`; a periodic re-sweep (the same trap-sweep harness) re-verifies open rows.
- **Non-goal: implementing any member PR.** This doc only groups + sequences; design lives in each member's own plan/ship-loop.
- **Non-goal: re-litigating priorities.** P2/P3 tiers are inherited from the backlog entries as-verified, not re-scored here.

## Verification

Per `AGENTS.md` § Project rules — this is a pure-docs roadmap; no build/test buckets apply.

- **Bucket A / E / scenario**: N/A — docs only.
- **Build gate**: N/A — no C++ diff.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint).
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: roadmap stress-tested against the live backlog — every PR row traces to a verified-open entry from the trap-sweep; no row references a reconciled/shipped slug.
- **Manual residue**: none.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — no symbols are deferred by this doc (it defers nothing; it schedules).

- **Mechanical archival of now-`applied` monolith entries** into `applied.md` — the trap-sweep flipped 13 entries to `applied` in place; relocating them is a separate hygiene PR, not part of this roadmap.

## Implementation log
*(populated as PR groups land — bullet per shipped member: `#N · PR-<k> <slug> shipped`)*

## Deviations from plan
*(populated as the roadmap is revised — regrouping, reprioritisation, new entries from future sweeps)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
*The `git mv` is the step that reliably gets dropped. In the SAME PR that retires this roadmap —*
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*

*(Delete this `## Archive` block as part of step 2.)*
