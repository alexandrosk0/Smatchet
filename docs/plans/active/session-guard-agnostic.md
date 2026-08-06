# Plan — Harness-agnostic concurrent-session HEAD-drift guard

> **Slug**: `session-guard-agnostic` (matches this file's basename without `.md`).
>
> **Status**: `active` — **not started** (no `agents/_shared/session-guard/` on develop).
>
> **Mandatory rules cross-link**: see `AGENTS.md` § Project rules § Plan-doc family.

## Context

PR #913 shipped the concurrent-session collision fix (worktree-per-session + a HEAD-drift guard that
denies a wrong-branch Edit/commit when HEAD moves under a session) — but it is **Claude-Code-only**:
the guard is 5 bash hooks wired through `.claude/settings.json`, and the registry lives at
`<tree>/.claude/.active-sessions/`. The user runs other harnesses (Pi, Codex) and wants the same
protection everywhere. Investigation confirmed Pi (`pi.on("tool_call")` → `{block,reason}`) and Codex
(`.codex/hooks.json` PreToolUse, **Claude-style deny JSON**) both expose pre-tool intercepts.

**Intended outcome — after this lands:** the drift guard runs from one harness-neutral "brain" with
thin per-harness adapters, actively enforced on **Claude + Pi + Codex** (Cursor has no hook surface →
worktree-isolation only). Originating thread: this session's "will this work for Pi / make it
agnostic / every for codex too" follow-up to #913.

## Approach

One ESM Node core (`agents/_shared/session-guard/guard.mjs`) holds all the drift logic ported from
#913's 5 bash hooks (sha-level drift, no-direct-commit-to-develop in the integration tree,
drift-on-move deny, aggressor-sibling deny, >7d sweep, liveness heartbeat, JSON-escaped reasons). Node
is the cross-harness lingua franca — both Claude Code and Pi *are* Node, so it dodges the
`python`/`python3` PATH-stub fragility seen on this machine, and Pi can `import` the core in-process
(zero subprocess). Each harness gets a thin adapter that translates its native hook event → `decide()`
→ its native block response; adding a future harness is one shim, brain untouched.

Two choices that remove friction: the registry moves to a **neutral `<tree>/.session-guard/`** (gitignored)
so a Claude session and a Pi session in the same tree detect each other; and the escape-env names are
built from `project.config.json` `project.env_prefix` (no hardcoded `SMATCHET` literal in the portable
dir → `test-portable-purity` stays green with no baseline edit). Trade-off named: `node` spawn per
Claude tool call (~50-80 ms) vs today's bash — accepted, in line with existing per-tool hook overhead;
Pi pays zero (direct import).

## Files to modify

1. `agents/_shared/session-guard/guard.mjs` — **new**, the brain (ESM; logic + git + registry + CLI + claude/pi/codex I/O shims; env-prefix from `project.env_prefix`).
2. `agents/_shared/session-guard/README.md` — **new**, registry format + decision contract + "how to add a harness adapter" (use `${PREFIX}_…` placeholders, never a literal `SMATCHET`).
3. `docs/harness/pi/extensions/session-guard/index.ts` — **new**, Pi adapter (`session_start`/`tool_call`/`tool_result` → core); `import './guard.mjs'`; tree path from `ctx.cwd`; maps Pi tool names `bash`/`edit`/`write`.
4. `docs/harness/codex/hooks.json` — **new**, Codex adapter config (PreToolUse `^Bash$|^apply_patch$|^Edit$|^Write$` + SessionStart + PostToolUse + Stop → `node guard.mjs … --harness codex`).
5. `docs/harness/codex/hooks-equivalent.md` — update: the drift-guard now uses a Codex PreToolUse hook; the lint-scanner stays on the git pre-commit hook.
6. `project.config.json` — add `"pi"` to `harness.supported` (`codex` already listed).
7. `agents/scripts/core/test-session-guard.sh` — **new**, fixture decision tests (ports #913's functional decision cases — 8 scenarios — against the core + a cross-harness sibling-count case).
8. `docs/harness/claude-code/hooks/{guard-head-drift,resync-head-baseline,guard-shared-tree}.sh` + `agents/scripts/core/{session-tree-banner,session-heartbeat}.sh` — **delete** (logic → core).
9. `docs/harness/claude-code/settings.json.tmpl` — swap the 5 bash-hook entries → `node guard.mjs … --harness claude` (SessionStart / PreToolUse Edit|Write|MultiEdit|NotebookEdit + Bash / PostToolUse Bash / Stop).
10. `agents/scripts/core/setup-harness.sh` — `setup_claude_code`: drop the 3 guard-hook `copy_template` lines + add a `node` presence check; `setup_pi`: copy `index.ts` + `guard.mjs` into `.pi/extensions/session-guard/`; `setup_codex`: deploy `.codex/hooks.json`.
11. `scripts/dev/worktree.sh` — `registry_dir` → `.session-guard`; list/resync read the new path.
12. `.gitignore` — add `.session-guard/`.
13. `docs/agent-rules/process-rules.md` — update § Concurrent interactive sessions: node core, neutral registry, cross-harness, which harnesses enforce vs worktree-only.
14. `docs/harness/capability-adapter.md` — add a `session-guard` row: claude ✓ / pi ✓ / codex ✓ (best-effort) / cursor ✗.
15. `docs/harness/pi/README.md` — document the session-guard extension.

## Existing utilities reused

- `agents/_shared/token-tracking/agent-token-log.py` — the proven harness-agnostic shared-core layout (invoked by Claude + Codex + Cursor); model for `agents/_shared/session-guard/`.
- #913 hooks (`docs/harness/claude-code/hooks/guard-head-drift.sh` et al.) — registry entry format + thresholds (1800s liveness, 604800s sweep) + decision rules ported verbatim into `guard.mjs`.
- `docs/harness/claude-code/hooks/guard-head-drift.sh` — the exact PreToolUse deny-JSON shape (reused for Claude + Codex, which share it).
- `setup_pi()` in `agents/scripts/core/setup-harness.sh` — existing extension-copy flow (the subagent extension) as the model for deploying our Pi extension.
- `scripts/dev/worktree.sh` `registry_dir` + `live_session_count` — reused for the registry-path change.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: no impact — this is agentic-harness tooling, not product UI; no `Source/Core` code runs in the app frame loop.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: no impact on the app. The only latency is a `node` spawn (~50-80 ms) per Claude/Codex tool call in the *agent* hook path (Pi: zero, in-process) — well under any concern and off the product UI thread.
- **Pillar 3 (never crash)**: no impact — the guard fails safe (any error / missing baseline → allow, never block spuriously); it cannot crash the app.
- **Pillar 4 (accessibility)**: N/A — no UI surface.

## Perf-review-system gates (mandatory when diff touches `Source/Core/`; else `N/A — <reason>`)

`N/A — the diff is pure agentic-shell (`agents/_shared/`, `docs/harness/`, `scripts/dev/`) + CI/config
(`project.config.json`, `.gitignore`); it touches no `Source/**` C++ and no `SMATCHET_UI_PERF_SCOPE`
markers.` All five gates (PR-fast CI, Pillar-2 scanner, dispatcher drain, bucket-E, marker inventory):
N/A — no perf-gated core source in the diff.

## Risks / non-goals

- **Codex partial coverage** — Codex PreToolUse intercepts simple Bash + `apply_patch` + MCP but **not** `unified_exec` streaming shell or non-shell tools (OpenAI-documented gap) → best-effort, weaker than Claude/Pi. Accepted + documented in the capability-adapter row; the worktree model remains the structural backstop.
- **Cursor excluded** — no programmable pre-tool hook → cannot enforce; worktree-isolation only. Non-goal: wiring Cursor (nothing to wire into).
- **Sequencing vs the #913-hardening PR** — a fast-follow session is editing `setup-harness.sh` + `process-rules.md` + `git-janitor.sh`/`merge-watcher.py`; this refactor also edits `setup-harness.sh` + `process-rules.md` and deletes the bash hooks. Mitigation: **land the hardening PR first, then rebase this on the post-hardening develop** before implementing. (Implementation-PR concern; this plan-doc PR is conflict-free.)
- **Pi cross-dir import fragility** — mitigated by copying `guard.mjs` alongside `index.ts` in `.pi/extensions/session-guard/` (flat `./guard.mjs`) rather than a `../../../` climb-out across `.pi/` regeneration.
- **Old registry** — `<tree>/.claude/.active-sessions/` is transient session state; no migration (new sessions write the neutral path; the old dir is harmless + gitignored).
- **Non-goal**: this is the agnostic-guard refactor only; it does not implement the #913-hardening fast-follow items (merge-watcher confinement, self-excluding janitor defer, fresh-clone-gap, AGENTS.md pointer) — those are their own in-flight effort.

## Verification

Per `AGENTS.md` § Verification automation. Buckets (this is agentic-shell, so no C++ ctest/bucket-E):

- **Bucket A (pure-logic)**: `N/A — no Source/Core helper` (the equivalent is the fixture suite below).
- **Bucket E (ImGui Test Engine)**: `N/A — no UI surface`.
- **Bash/Node-driver scenario**: `agents/scripts/core/test-session-guard.sh` feeds normalized events to `node guard.mjs … --harness pi|claude|codex` and asserts the #913 decision cases — 8 scenarios: drift Edit→deny, no-direct-commit→deny, clean→allow, escape-env→allow, drifted-move→deny, resync-clears, aggressor-sibling→deny, solo→allow — plus a cross-harness sibling-count case + a valid-deny-JSON check via `jq -e` for claude & codex shims.
- **Build gate**: `N/A — no C++ target changes`; instead `node --check guard.mjs` + `jq -e` on `docs/harness/codex/hooks.json` + `tsc --noEmit` on `index.ts` if a TS toolchain is present.
- **Doc validation (blocks plan-doc PRs — keep this bullet)**: the canonical `scripts/dev/test-docs.sh` suite green (anchors / agent-contract / plan-index / ref-integrity / portable-purity / md_lint). A red doc-validation job blocks merge even though non-required.
- **Plan stress-test — `grill-with-docs` (keep this bullet)**: this plan was stress-tested across three explicit triple-check passes in-session (Pi + Codex runtime contracts verified against upstream docs; ESM/jiti, `project.env_prefix`, tool-name + deny-JSON schemas corrected). Re-run `grill-with-docs` before the *implementation* PR if the domain model shifts.
- **Manual residue**: the live **Pi smoke-test** (run `pi`, simulate a drift, confirm the edit is blocked) cannot be automated here — Pi is not installed on this machine. Deferred-automation action: add a `docs/self-improvement/categories/tooling.md` entry to install Pi in CI / a dev box and wire a Pi-extension integration test; until then the core fixtures + `tsc` parse cover the logic and the Pi adapter is a thin forwarder.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — before finalising, grep `**/CONTEXT*.md`, `docs/adr/`,
`agents/*.md`, and `docs/self-improvement/categories/` for stray references to the #913 bash-hook
filenames (being deleted) + `.claude/.active-sessions` (being renamed), and revise them.

- **Cursor enforcement** — no hook surface; worktree-isolation only (no follow-up planned unless Cursor adds a pre-tool hook API).
- **Other harnesses (Aider, etc.)** — adapter-ready (one shim each) but not wired; add on demand.
- **#913-hardening fast-follow** — separate in-flight effort (see Risks).

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. flip § Status → `shipped`,
2. `git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,
3. regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.
