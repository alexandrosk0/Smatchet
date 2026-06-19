# Plan — Force plan-lock filing before file edits (force-on-contention, 3-layer)

> **Slug**: `plan-lock-enforcement` (matches this file's basename without `.md`).
>
> **Status**: `active` — the machine-readable lifecycle marker. Values: `active` (driving in-flight work) · `shipped` (post-ship sections populated + all cited PRs merged — this file belongs in `docs/plans/shipped/`) · `blocked` / `deferred` (paused — one-line why). **Flip to `shipped` in the SAME post-ship PR that fills § Implementation log AND `git mv`s this file active → shipped** (see § Archive). `agents/scripts/core/plan-archival-owed.sh` nags at SessionStart if any `active/` plan is marked `shipped` but never moved.
>
> **Usage**: copy this template to `docs/plans/active/<slug>.md` as the first step of any new plan. Fill every section. Sections that genuinely don't apply get `N/A — <one-line reason>`, not deletion — the headings drive the "did you consider this?" forcing function for every author + reviewer agent.
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan location, § Plan-doc safety, § Plan revision after implementation, § Plan stress-test, § Plan template, § Plan-doc perf-gate section.

## Context

The plan-lock system (git-ref CAS locks under `refs/locks/*`, design `docs/plans/shipped/git-ref-plan-locks.md`) is **healthy but dormant**: a 9/9 primitive self-test passes, all three render/staleness/cleanup workflows are green, but the last 20 PRs (#1378–#1401) were solo/sequential single-subsystem work that never filed a claim, and the render mirror `docs/plans/active/_plan-locks.generated.md` has shown "No active plan-locks" unchanged since 2026-05-31 (`d7bc9e7f`). Filing a lock is currently **advisory** — nothing forces an agent to claim a write set before editing files, so two parallel agents editing the same files silently race; the only protection is each agent voluntarily running the pre-flight overlap check documented in `_plan-locks-archive.md`.

This plan makes lock-filing **enforced, but only when it matters**: a write set must be claimed before edits **iff another session is concurrently live** (force-on-contention). Solo/sequential work — the common case — pays zero tax. Enforcement is defense-in-depth across three layers so no single harness or bypass defeats it: (A) a Claude Code `PreToolUse` hook at edit time, (B) a harness-agnostic `pre-push` git hook, (C) a server-side CI merge-gate that is unbypassable except via an explicit out-of-band label.

After this lands, X is true: **when ≥2 sessions are live and an agent edits a file outside any lock it holds, the edit (A) / push (B) / merge (C) is blocked with a copy-paste `lock-claim.sh` remediation line, while solo work is never gated.** Originating thread: session investigation of "are the _plan-locks working?" → "how can we force all agents to file a plan lock before changing any files?" (design forks locked: Scope = force-on-contention; Layers = all three A+B+C).

## Approach

Layer A's trigger is a **repo-global** live-agent count, not a per-tree one. The existing `sr_count_live_siblings` (`agents/scripts/core/session-registry-lib.sh:146`) only scans the local tree's `.claude/.active-sessions`, so two sibling worktrees never see each other — useless for the canonical `nsc slug-A` / `nsc slug-B` parallel setup. Instead the hook enumerates `git worktree list --porcelain` and sums live entries (reusing `sr_entry_is_live`) across every worktree's registry + the integration tree, firing only when that global count is ≥ 2 (excluding self). The trigger is **"≥2 agents active," not write-set overlap** — overlap is circular (you can't compute it until everyone has already filed a lock, which is the behaviour being forced). When the count is < 2 the hook `exit 0`s (no tax on solo work). When ≥ 2 it requires the edit's target path be covered by a `refs/locks/*` claim whose `owner` matches `AGENT_ID`, else emits `permissionDecision: "deny"` with a copy-paste `lock-claim.sh` line. The lock-table read is `locks-show.sh`-backed (`git fetch +refs/locks/*` + `cat-file`) and cached ~30 s; any network/parse/worktree-enum failure **fails open** (advisory, never wedge a session).

The three layers are deliberately asymmetric in their fail-mode. A and B are **advisory / fail-open** local hooks (catch the mistake early, cheaply, but never hard-wedge a flaky-network session) — A at edit time inside Claude Code, B at `git push` time for any harness (the catch-all for non-Claude-Code agents, wired via `core.hooksPath`). C is the **fail-closed hard net**: a required GitHub Actions check that recomputes contention server-side (overlap between this PR's changed files and any **other-branch `refs/locks/*` write set** — the declared-write-set basis **only**, NOT raw other-open-PR changed-file sets; non-circular, since it does not require THIS PR to have filed a lock) and blocks merge — unbypassable except the `plan-lock-out-of-band` label that downgrades block→WARN, symmetric with the existing `cr-out-of-band` / `tests-out-of-band` / `perf-out-of-band` overrides in `merge-gates.sh`. This is the same advisory-pair-plus-hard-net shape the tree-guards already use (`guard-shared-tree.sh` advisory + `guard-head-drift.sh` hard net).

**Lock seeding (adoption).** A lock must already exist when a *second* agent's first edit hits the guard — so seeding is **eager and decoupled from enforcement**: the ship-loop files a claim from the plan-doc's **§Files-to-modify** list at task start whenever a plan exists, reusing the declared write set verbatim (no separate discovery step). Eager seeding closes the race where agent A starts solo, files nothing, then agent B goes live and A is now contended with no lock filed — by always seeding from the plan, A's claim is present as the early-warning substrate the moment B arrives. *Seeding* is eager (file whenever a plan exists); *enforcement* (the deny) stays force-on-contention (≥2 live) — the two are independent. Plan-less tasks (small fixes, no plan-doc) have no auto-seed: they file on the first deny via the `lock-claim.sh` one-liner the guard hands them, and force-on-contention makes plan-less-AND-contended rare. `lock-claim-update.sh` expands the seeded set on mid-work discovery.

The non-obvious trade-off that shaped the design: **filing a claim must never itself be blocked.** `lock-claim.sh` builds its orphan commit via `git hash-object` / `git mktree` / `git commit-tree` and pushes a ref — it writes **zero** working-tree files — so the edit-time guard can never create a chicken-and-egg deadlock where you can't claim because claiming requires an edit. Mid-work write-set discovery (an agent realises it must touch a file it didn't list) is handled by `lock-claim-update.sh` (holder-only scope expansion); the guard's deny message hands the agent the exact one-liner.

## Files to modify

**Layer A — Claude Code PreToolUse edit-time guard (advisory, fail-open):**
1. `docs/harness/claude-code/hooks/guard-plan-lock.sh` (new) — PreToolUse(Edit|Write|MultiEdit|NotebookEdit) hook. Parses `.tool_input.file_path` from stdin (mirror the `json_field` helper in `docs/harness/claude-code/hooks/guard-shared-tree.sh:77-86`); short-circuits `exit 0` when the **repo-global** live-agent count < 2 (enumerate `git worktree list --porcelain`, sum `sr_entry_is_live` across each worktree's `.claude/.active-sessions` + the integration tree, exclude self) or `SMATCHET_REQUIRE_PLAN_LOCK=0` bypass exported pre-launch; else checks the target path against the AGENT_ID-owned lock table and emits the deny JSON (mirror `:149-152`). Fails open on missing lib / network / parse / worktree-enum error.
2. `docs/harness/claude-code/settings.json.tmpl:98-107` — add `guard-plan-lock.sh` to the existing `"matcher": "Edit|Write|MultiEdit|NotebookEdit"` hooks array (currently only `guard-head-drift.sh`). The worktree adapter (`scripts/dev/worktree.ps1` "sync … added missing template hooks") copies it into `.claude/hooks/` on next provision.
3. `agents/scripts/core/lock-table-cache.sh` (new) — wraps `locks-show.sh` JSON output (`LOCKS_SHOW_FORMAT=json` → `git fetch +refs/locks/*` + `cat-file` + `_lock-json.py format-json`) in a ~30 s TTL temp-file cache, plus an `AGENT_ID`/path-coverage query helper, shared by Layers A and B. (`git ls-remote` alone is insufficient — it returns only ref SHAs, not the `write_set` blob the coverage check needs.) Fail-open contract documented at top.

**Layer B — harness-agnostic pre-push hard-stop (advisory, fail-open):**
4. `scripts/git-hooks/pre-push:43+` — add a third independent stop `(C) UNLOCKED-CONTENDED-PUSH` after the existing `(A)` direct-push and `(B)` merged-PR stops: when the push's changed-file set (diff vs the remote-tracking base) overlaps a `refs/locks/*` write set owned by a **different** branch and no override is set, refuse. Override: `SMATCHET_ALLOW_UNLOCKED_PUSH=1` (sanctioned, logged), mirroring the existing `SMATCHET_ALLOW_DEVELOP_PUSH` / `SMATCHET_ALLOW_MERGED_PR_PUSH` convention. Reuses `lock-table-cache.sh`.

**Layer C — server-side CI merge-gate (fail-closed hard net):**
5. `.github/workflows/plan-lock-gate.yml` (new) — required check modeled on `.github/workflows/cr-finding-gate.yml`. Computes this PR's changed files; enumerates `refs/locks/*` claims (the authoritative global write sets on origin); blocks (non-zero) when this PR's changed files overlap a claim's `write_set` owned by a different `branch`. **Comparison basis is the declared `refs/locks` write_set ONLY — not raw other-open-PR changed-file overlap** (that re-introduces the file-overlap-as-trigger coupling Q1 removed and false-trips every adjacent / stacked PR; lock-less agents are driven to file via Layers A/B, not caught by a noisy second C trigger). Non-circular: does not require THIS PR to have filed a lock. Honours stale locks as non-blocking (older than `lock-staleness.yml`'s `THRESHOLD_DAYS=14`). Override: `plan-lock-out-of-band` label → block downgraded to WARN.
6. `agents/scripts/core/merge-gates.sh` (header + override block) — document the new `plan-lock-out-of-band` per-PR override alongside the existing `cr-out-of-band` / `tests-out-of-band` / `perf-out-of-band` entries so the orchestrator poller text stays in sync with the required-check set.
7. `AGENTS.md` § Merge gates § Per-PR overrides — register `plan-lock-out-of-band` in the canonical override-label table; cross-link § Concurrent interactive sessions to the new enforcement.

**Adoption wiring — ship-loop lock seeding (eager, non-blocking):**
8. `docs/agent-rules/ship-loops.md` (+ the autonomous ship-loop sequence in `AGENTS.md` § Autonomous ship-loop default) — add a "seed plan-lock from §Files-to-modify" step at ship-loop start when a plan-doc exists: `lock-claim.sh <slug> <file>…` built from the plan's declared write set. **Eager** (fires whenever a plan exists, independent of the live-count gate that drives *enforcement*); a single non-blocking ref push, never a deny path. Document the plan-less fallback (file on first deny via the guard's one-liner) so a small-fix task isn't expected to seed.

**Tests (see § Verification):**
9. `agents/scripts/core/test-guard-plan-lock-bats.sh` (new) — bats suite for Layer A, mirroring `agents/scripts/core/test-guard-shared-tree-bats.sh`.
10. `agents/scripts/core/test-pre-push-bats.sh` (extend, or new if absent) — cases for the `(C)` stop.

## Existing utilities reused

- `sr_entry_is_live` / `git worktree list --porcelain` — `agents/scripts/core/session-registry-lib.sh:134` — the per-tree liveness primitive summed across ALL worktrees for a **repo-global** active-agent count. NOT `sr_count_live_siblings` (`:146`) directly — that scans one tree's registry and is blind to sibling worktrees, the exact cross-worktree case plan-locks exist for.
- `lock-claim.sh` — `agents/scripts/core/lock-claim.sh:1-159` — the claim primitive the deny messages point to; writes **zero** working-tree files (orphan commit via `hash-object`/`mktree`/`commit-tree`, `:139-147` CAS reject classification) → guarantees no chicken-and-egg.
- `lock-claim-update.sh` — `agents/scripts/core/lock-claim-update.sh` — holder-only write-set expansion; the mid-work-discovery remediation the guard hands the agent.
- `locks-show.sh` — `agents/scripts/core/locks-show.sh` — live `refs/locks/*` query the cache layer wraps.
- `_lock-json.py` — `agents/scripts/core/_lock-json.py` — claim.json (owner / branch / write-set) parse helper used by Layers A/B/C.
- `guard-shared-tree.sh` — `docs/harness/claude-code/hooks/guard-shared-tree.sh:77-152` — the PreToolUse deny-JSON shape, `json_field` stdin parser, fail-open lib-resolution, and the **override-must-be-exported-pre-launch** deny wording (`:150`) Layer A copies verbatim.
- `scripts/git-hooks/pre-push:1-42` — the two-stop `(A)`/`(B)` structure Layer B extends with `(C)`.
- `.github/workflows/cr-finding-gate.yml` — the required-check workflow pattern Layer C mirrors.
- `merge-gates.sh` override-label block — `agents/scripts/core/merge-gates.sh` header — the `*-out-of-band` downgrade convention `plan-lock-out-of-band` follows.

## UX Pillar callouts

Per `AGENTS.md` § UX Pillars. This change touches only the agentic-shell (hooks / git-hooks / CI workflows / docs) — no product runtime, no `Source/` code, no ImGui path.

- **Pillar 1 (perf, 144 Hz / 6.94 ms steady-state)**: no impact — no product-runtime code; the only added cost is a ~30 s-cached `git ls-remote` at agent edit/push time, off the UI thread entirely (no process linkage to the app).
- **Pillar 2 (UI-thread never blocks > 100 ms without visible cue)**: no impact — no `ImGui::*`-reachable code added; the hooks run in the agent harness, not the app.
- **Pillar 3 (never crash)**: no impact — agentic-shell only; all three layers fail **open** (A/B) or are isolated CI (C), so a guard bug degrades to "lock not enforced," never an app crash.
- **Pillar 4 (accessibility — keyboard nav / font scaling / WCAG AA)**: no impact — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

`N/A — diff is pure agentic-shell (bash hooks), CI workflow YAML, and docs; it touches no `Source_Core/` / perf-gated path (`project.config.json` `lint.zones` / `perf`). No PR-fast scenario, Pillar-2 scanner, dispatcher-drain, bucket-E, or marker-inventory surface is exercised.`

## Risks / non-goals

**Risks:**
- **Per-edit network cost (Layer A).** Each edit under contention would otherwise `git ls-remote refs/locks/*`. Mitigation: `lock-table-cache.sh` caches the table in a temp file with ~30 s TTL; cache miss refreshes, network error **fails open** (advisory bias-to-allow, identical to `guard-shared-tree.sh`'s missing-lib fall-open `:70`).
- **Mid-work file discovery friction.** An agent that discovers it must touch an unlisted file is blocked on the first edit. Mitigation/intent: this friction is the point (it forces write-set honesty); the deny message hands the exact `lock-claim-update.sh <slug> <file>` one-liner (holder-only, one round-trip). Accepted; an opt-in `SMATCHET_PLAN_LOCK_AUTOEXPAND=1` fast-path that auto-expands the holder's own lock is documented but **off by default** (auditability over convenience).
- **Override-env footgun (the exact bug this design must not repeat).** An inline `SMATCHET_REQUIRE_PLAN_LOCK=0 <edit>` prefix does **not** reach a PreToolUse hook — the hook reads its own env before the tool runs. Mitigation: the deny message copies `guard-shared-tree.sh:150`'s wording verbatim — "export … in the session env BEFORE launch." Accepted (inherent to the hook model).
- **False-block on solo work.** Mitigation: force-on-contention — repo-global live-agent count < 2 → `exit 0`. Zero tax on the common case. **Verification dependency (confirm at impl):** a worktree session must self-register a `.claude/.active-sessions/<id>` entry (writer = `session-heartbeat.sh` / `session-tree-banner.sh`); if a worktree session does NOT register in its own tree, the global sum under-reports and Layer A silently no-ops. Layers B/C (origin `refs/locks/*`) still catch it — but flag this in the impl so Layer A isn't trusted as the sole net.
- **Fail-open layers could let a contended edit through.** Accepted by design: A/B are advisory (catch-early, never wedge); **Layer C is the fail-closed hard net** (server-side required check, unbypassable except the explicit `plan-lock-out-of-band` label). Defense-in-depth, same shape as `guard-shared-tree.sh` (advisory) + `guard-head-drift.sh` (hard net).
- **Stale lock wedging contention detection.** A claim from an abandoned session could falsely block. Mitigation: Layer C treats a lock older than `lock-staleness.yml`'s `THRESHOLD_DAYS=14` as non-blocking; the daily sweep frees it regardless.
- **Chicken-and-egg deadlock — NOT a risk (explicit non-risk).** `lock-claim.sh` touches no working-tree file, so claiming is never itself gated by the edit guard. Cited so a reviewer doesn't re-raise it.

**Non-goals:**
- Not *gating/blocking* solo/sequential work — enforcement (the deny) is force-on-contention only; a solo edit is never blocked. (Eager seeding still files a lock from the plan on a solo plan-task, but filing is a free, non-blocking ref push; the "no tax" guarantee is about *blocking*, not about whether a ref is pushed.)
- Not changing the lock wire format, ref layout, or backend dispatch (`SMATCHET_LOCK_BACKEND=p4-counter` still works; see Out of scope).
- Not building a lock dashboard / UI / live-contention visualiser.
- Not rewriting the frozen `_plan-locks-archive.md` advisory protocol history (it stays frozen at the 2026-05-17 cutover).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps. Buckets:

- **Bucket A (pure-logic ctest, `test-rig`)**: `N/A — no `Source/Core/` C++ helper added; logic lives in bash hooks + CI YAML, covered by the bats suites below.`
- **Bucket E (ImGui Test Engine, `cmake --build --preset ninja-ui-test-msvc`)**: `N/A — no ImGui / UI surface.`
- **Bash-driver scenario / screenshot / sanitizer**: `agents/scripts/core/test-guard-plan-lock-bats.sh` (Layer A — mirror `test-guard-shared-tree-bats.sh`): solo-allow (live==0), contended-deny-unlocked, contended-allow-when-holder, deny-JSON shape, target-path parse, fail-open-on-missing-lib, fail-open-on-network-error, bypass-env-pre-launch, claim-itself-never-blocked. `agents/scripts/core/test-pre-push-bats.sh` (Layer B — `(C)` stop): overlap-different-branch-blocks, own-branch-allows, override-allows, no-lock-table-fails-open. Layer C: a workflow-logic bats over the extracted gate script (this-PR changed-set ∩ other-branch lock `write_set` — declared-write-set basis only, no open-PR-changed-file overlap; stale-lock non-blocking; out-of-band-label downgrade) — keep the gate's decision logic in a sourceable `*.sh` so it is testable headless (don't bury it in inline YAML).
- **Build gate**: `N/A — no C++ compiled; diff is bash + YAML + Markdown. Dual-target build unaffected.`
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (it enumerates the doc-validation steps — anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint; defer to the script, don't hardcode the sub-step list here). A red doc-validation job blocks merge even though non-required. Applies here because this PR adds a plan-doc and edits `AGENTS.md`.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: stress-test this plan against the domain model + sharpen terms before finalising; record the outcome. Required for every plan — do not delete.
- **Manual residue**: if any verification step ends up manual, name the deferred-automation action plan + add a `docs/self-improvement/categories/tooling.md` entry. No silent residue. (Anticipated residue: none — all three layers' decision logic is shell/CI and bats-testable; the requirement to keep Layer C logic in a sourceable `.sh` exists precisely to avoid a manual "trigger a real PR collision" step.)

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — per `AGENTS.md` § Process rules § Scope-reduction edits: before finalising, grep `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for stray references to anything deferred here, and revise or delete them.

- **p4-counter backend symmetric enforcement** — Layers A/B query git `refs/locks/*`; under `SMATCHET_LOCK_BACKEND=p4-counter` the contention query must dispatch to the p4 counter table. Follow-up plan: extend `lock-table-cache.sh` with a backend switch mirroring `lock-claim.sh:41`. Git path ships first.
- **Cross-repo / submodule write-sets** — single-repo only; a write set spanning a submodule is not modeled. No-action (no current multi-repo workflow).
- **Auto-release on edit-abandon** — no new release trigger; rely on existing `lock-cleanup.yml` (PR-merge) + `lock-staleness.yml` (daily 14-day sweep). No-action.
- **Auto-expand-on-discovery as default** — `SMATCHET_PLAN_LOCK_AUTOEXPAND=1` is documented but ships **off**; flipping the default is a follow-up gated on observed friction data.

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
