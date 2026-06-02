# Plan — B10: encode deferred process forcing-rules into the docs

> **Slug**: `b10-docs-forcing-rules` (matches this file's basename without `.md`).
>
> Sub-plan of [`agentic-backlog-campaign.md`](agentic-backlog-campaign.md) batch **B10**. Mandatory rules cross-link: see `AGENTS.md` § Project rules § Plan-doc family.

## Context

The self-improvement backlog accumulated ~14 P3 "process forcing-rule" suggestions — small doc edits each agent flagged after hitting the same friction (wrong git command, un-grepped TU name, mis-routed slice, etc.). They were deferred as a batch (campaign B10) because shipping 14 micro-PRs would burn CodeRabbit quota for one conceptual change. This plan lands them as **one pure-docs PR**.

Pre-plan verification (2026-06-02) confirmed almost all are **genuinely live** (the rule is not yet in the target doc) — unlike B1–B3 which were stale. A few are **partial** (a sibling rule exists in a different scope). After this lands: the deferred forcing-rules are encoded in their natural home docs, and the ~12 source backlog entries flip to `applied`.

Intended outcome: an agent that hits one of these frictions next time finds the rule already written, instead of re-discovering it.

## Approach

One PR, pure-docs. Each rule lands in its **natural home doc** per the `AGENTS.md` § Process rules meta-rule ("1-liners stay in § Project rules; rules that fit an extracted topic land in that file; ≤30-line orphans go in `process-rules.md`; >30-line topics get their own file"). No new topic file is warranted — every rule is a 1–5 line addition to an existing doc.

**Hard constraint that shapes placement — portable-purity.** `agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/` are **portable dirs**; `test-portable-purity.sh` blocks any NEW project literal (`Source/Core`, `Smatchet`, `api.openai.com`, `ninja-*-msvc`, agent names as products, …) added to them vs the baseline. So:
- Rules with **inherent project literals** (67 KB `Source/Core` cap, the public-header-dir path, AppController-shim example) land in **non-portable** project docs — `AGENTS.md`, `docs/plans/`, `docs/guides/`, `docs/self-improvement/` — where literals are free.
- Rules landing in **portable** docs (`process-rules.md`, `delegation.md`, `git-janitor.md`, `architect.md`) MUST be phrased generically (git concepts, "the project's public-header dir", "the binding-adapter/facade layer") or reference `project.config.json` keys — never a raw literal.

Each rule's row below names its target **and** whether the target is portable (→ generic phrasing required). Source backlog entries flip to `applied` + `test-backlog-counts.sh --fix` in the same PR.

## Files to modify

Per-member table. **Status**: LIVE = not present anywhere; PARTIAL = a sibling rule exists in different scope. **Portable** = target is a portable dir (generic phrasing required).

| # | Rule (source backlog entry) | Target file → section | Status | Portable? |
|---|---|---|---|---|
| 1 | baseline.md is informational, not gate input — don't edit per-decomposition; regen once per campaign (process 2026-06-01) | `docs/guides/imgui-draw-pattern.md` + `docs/plans/active/decompose-top-20-monoliths.md` § Approach | LIVE | no |
| 2 | Loss/residue audits diff vs `origin/<base>` post-fetch; `gh pr state==MERGED` is merge-truth (process 2026-05-30) | `docs/agent-rules/process-rules.md` § Git/p4 discipline (new bullet) | LIVE | **yes** |
| 3 | Backlog/SI/plan claims about a file's behaviour must cite a verified line (process 2026-05-30) | `docs/self-improvement/AGENT_SELF_IMPROVEMENT.md` § Workflow step 2 | LIVE (process-rules has CR-reply sibling, different scope) | no |
| 4 | `Source/Core` / `Plugins` source files stay < 67 KB; split-recipe (process 2026-05-26) | `AGENTS.md` § Project rules (cap 1-liner) + split-recipe detail | LIVE | no |
| 5 | Plan-template "dual-target build gate" mandatory § (process 2026-05-21, architect) | `docs/plans/active/_plan-template.md` | PARTIAL (§ Verification already has the dual-target build cmd) | no |
| 6 | Grep `rg -l '<Foo>'` before listing a new `<Foo>.{h,cpp}` in § Files (process 2026-05-24) | `_plan-template.md` § Files to modify | LIVE | no |
| 7 | If a slice copies a not-yet-merged sibling's pattern, inline the 3–5-line shape (process 2026-05-24) | `_plan-template.md` (placement TBD — template has no per-slice section) | LIVE | no |
| 8 | Default `git merge origin/develop` not `rebase` for squash-destined branches; catch-up-sync sub-rule (process 2026-05-20) | `docs/agent-rules/process-rules.md` § Git/p4 discipline | LIVE | **yes** |
| 9 | Post-squash-merge `branch -D` fast-path (5-step pre-flight doesn't apply when branch was just squash-merged + untouched) (process 2026-05-19) | `agents/core/git-janitor.md` § cleanup | PARTIAL (general `branch -D` 5-step exists at :54) | **yes** |
| 10 | `unique_ptr<T>` member in a header needs `T` complete at every consumer (process 2026-05-17) | `AGENTS.md` § Quality | LIVE | no |
| 11 | Plan-time check: every `add_library(<X> INTERFACE)` needs ≥1 `target_link_libraries(... <X>)` (process 2026-05-17) | `docs/agent-rules/delegation.md` § Orchestrator delegation packet | LIVE | **yes** |
| 12 | Slice creating new public headers / editing the project's include dir → don't route to `test-rig` (process 2026-05-24) | `docs/agent-rules/delegation.md` § Subsystem specialists | LIVE | **yes** |
| 13 | Cross-cutting sig change: grep the upstream caller; if ~zero direct calls, name the binding-adapter/facade as the change-site (tooling 2026-05-21, architect) | `agents/core/architect.md` § review template | LIVE | **yes** |
| 14 | When CR marks a finding "✅ Addressed", read the cited commit's diff to confirm before trusting (test 2026-05-18) | `AGENTS.md` § Merge gates (or `docs/agent-rules/process-rules.md`) | LIVE | depends on home |

Plus, in the same PR:
- `docs/self-improvement/categories/{process,tooling,test}.md` — remove the ~12 source entries; `applied.md` — add resolution lines; `AGENT_SELF_IMPROVEMENT.md` § Index — `--fix` count sync.
- `docs/plans/active/agentic-backlog-campaign.md` § Implementation log + § Re-scoped remainder — mark B10 done (and add the deferred B4 plan-log line while here).

## Existing utilities reused

- `bash agents/scripts/core/test-backlog-counts.sh --fix` — count sync after archival.
- `bash agents/scripts/project/test-portable-purity.sh` — run BEFORE push to catch any new project literal in a portable-dir edit (members 2, 8, 9, 11, 12, 13).
- `project.config.json` keys — reference instead of literals in portable docs where a concrete value is unavoidable.

## UX Pillar callouts

- **Pillar 1 / 2 / 3**: N/A — pure-docs, no `Source/` change, no runtime.
- **Pillar 4**: N/A.

## Perf-review-system gates

N/A — pure-docs diff, no `Source/Core/` touch (`agents/scripts/core/is-pure-docs-diff.sh` should classify this PR pure-docs → skips build + perf).

## Decisions needed before implementation (flag, don't assume)

1. **Member 5 (dual-target plan-template §)** — the template § Verification already carries `cmake --build … --target SmatchetStandalone SmatchetCore_DX12`. Options: (a) **downgrade** — add a one-line "if the diff touches `SMATCHET_WITH_*` source-list gating, anchor the dual-target build to the specific files" note to § Files to modify (cheap, no new section); (b) **full** — add a dedicated mandatory "## Dual-target compile gate" section between § Perf gates and § Risks (matches the architect's original ask, more ceremony). **Recommend (a).**
2. **Member 7 (slice-coordination inline-shape)** — the template is single-feature and has no per-slice / Coordination section. Options: (a) add a short optional "## Per-slice coordination (multi-slice plans only)" section to the template; (b) fold the rule into § Files to modify as a one-liner about cross-slice pattern dependencies; (c) drop it as too-niche (it fired once, mild friction). **Recommend (b).**
3. **Member 14 (CR "✅ Addressed" verify-the-diff)** — home is either `AGENTS.md` § Merge gates (project doc, literal-free anyway) or `process-rules.md` (portable). Either works; **recommend `AGENTS.md` § Merge gates** since it's CR-gate-adjacent.

## Risks / non-goals

- **Portable-purity trip (highest risk)** — members 2, 8, 9, 11, 12, 13 touch portable dirs. Mitigation: generic phrasing per the table; run `test-portable-purity.sh` before push (the gate also runs in CI's "Doc anchors + agent contract" job, but local-first saves a round-trip — the documented edit-time gap).
- **comment-noise / doc-validation gates** — markdown edits can trip `comment_audit` only on C++; doc-validation checks plan anchors. Low risk; run `test-all.sh` doc subset before push.
- **Count-index drift** — archiving 12 entries across 3 category files; `--fix` handles it. Risk is forgetting → pre-push gate rejects. Mitigation: `--fix` in the same commit.
- **Non-goal**: rules already fully present (none found live-clean, but if implementation finds one already encoded, skip it + still archive the entry with an "already-present" resolution).
- **Non-goal**: rewording existing rules or restructuring the target docs — additive edits only.

## Verification

- **Pure-docs** — no build, no ctest. `is-pure-docs-diff.sh` classifies the PR.
- `bash agents/scripts/project/test-portable-purity.sh` — PASS (no new literals in portable dirs).
- `bash agents/scripts/core/test-backlog-counts.sh` — PASS (index matches after `--fix`).
- doc-validation subset of `scripts/dev/test-all.sh` (plan-anchor + agent-contract checks) — PASS.
- **Manual residue**: none — every member is a doc edit verifiable by re-grepping the target for the rule text post-edit.

## Out of scope (flagged, not designed)

- The PARTIAL members (3, 5, 9) — the sibling rule covers part of the intent; this PR adds the missing scope, it does not rewrite the existing sibling.
- Tooling that some entries *also* proposed (e.g. a `git-leftover-audit.sh` for member 2, a file-size check script for member 4) — those are **B9 / separate tooling batches**, not B10. B10 is the doc-rule half only; cross-link from each doc rule to its tooling sibling where one exists.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*
