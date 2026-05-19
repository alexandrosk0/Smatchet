# Agent self-improvement — process

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-19 · debug-detective + test-author · [process] · P1 — Bootstrap-golden screenshot tests can enshrine the bug they were meant to catch
  Details: PR-train on the theme-switch-residual-colors slice nearly shipped `tests/golden/theme-switch-roundtrip.png` as a checked-in regression gate — except the golden was bootstrapped from the live build at a moment when the bug was active, so the PNG was a literal picture of the buggy post-round-trip state. The diff gate would have passed forever against the broken-state golden; the test would have certified the bug as "expected behaviour." Caught only because the user opened the golden by hand and said "this is the result of the bug". The same failure mode applied to `dock-gap-sentinel` and `command-palette-fuzzy` goldens that had to be re-bootstrapped against the fixed build. Bootstrap-golden tests are one bad commit away from this trap on every visual-regression slice.
  Concrete next action: extend AGENTS.md § Project rules with a **Golden-image approval contract**: "Any agent that writes or regenerates a `tests/golden/*.png` (or any other checked-in reference artefact a regression gate diffs against — screenshot, JSON snapshot, deterministic byte stream) MUST (1) build, (2) hand the golden file path + a launched-app handle showing the captured-state to the user, (3) wait for an explicit user 'looks right' / 'approve golden' verdict before any `git add tests/golden/*.png` + commit. Bootstrap-from-live-build is a UI-tuning-equivalent change — visual-validation exception applies. On rejection, `git checkout -- tests/golden/<file>.png` and iterate the underlying fix before re-bootstrapping." Cross-link to the existing visual-validation exception entry above. Also add an `agents/test-author.md` checklist bullet under "Bucket-C scenarios" — golden files cannot ship without user approval; prefer dual-capture-no-golden patterns (see `scripts/dev/test-theme-roundtrip.sh`) when both states are produced at runtime within the same test, since those tests have no checked-in artefact to enshrine.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-19 · orchestrator · [process] · P3 — `git branch -d <name>` rejects post-squash-merge cleanup; needs `-D` after every squash-merge
  Details: After GitHub squash-merges a PR, the local feature branch's tip (the last per-feature commit) is NOT an ancestor of the new squash-merge commit on `develop`. `git branch -d <name>` then errors with `error: the branch '<name>' is not fully merged` even though the change has demonstrably landed via squash. Observed on PR #308 cleanup — local branch `chore/unblock-external-2-3-4` tip `755d4004` is not ancestor of squash-merge `2ba2c5bc`, so `git branch -d` failed and required `git branch -D`. AGENTS.md § Destructive git ops in shared worktrees lists `branch -D` as requiring the 5-step pre-flight, but the post-squash-merge case is the most common cleanup path and the orchestrator hits it on every merged PR.
  Concrete next action: extend `agents/git-janitor.md` § Standard cleanup loop with an explicit "**Post-squash-merge branch cleanup**: after a squash-merge, the local feature-branch tip is orphaned (not reachable from the new squash commit). Use `git branch -D <name>` after verifying (a) `gh pr view <N> --json state` is `MERGED`, (b) `mergeCommit.oid` is reachable from `origin/<base>`. The 5-step pre-flight from AGENTS.md § Destructive git ops in shared worktrees does NOT apply when the branch was just merged via squash AND no other agent has touched it — record a tighter post-squash-merge fast-path." Cross-link from AGENTS.md § Project rules § Destructive git ops to the git-janitor sub-section. ~15 min doc edit.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-18 · whisper-phase-d · [process] · P3 — `AppController_LuaBindings.cpp:1816` calls `ImGui::InputText` raw, bypassing the `SmatchetLocalizedImGui` dictation wrapper
  Details: Phase D of the whisper-dictation plan auto-wired dictation insertion to every ImGui input via the `SmatchetLocalizedImGui::InputText` / `InputTextMultiline` / `InputTextWithHint` wrappers (the existing `#define ImGui SmatchetLocalizedImGui` pattern). One first-party call site bypasses the wrapper: `Source_Core/src/AppController_LuaBindings.cpp:1816` invokes `ImGui::InputText` directly (it's inside the sol2 binding that lets Lua scripts spawn dynamic widgets — the wrapper macro is intentionally off in that TU). Effect: Lua-authored dynamic InputText widgets in `scripts/*.lua` do NOT participate in dictation; focused-buffer auto-registration skips them. Real-world impact is small (Lua-driven widgets are advanced-user territory; built-in surfaces are all already covered). No NEW raw-`ImGui::InputText` call sites should be allowed elsewhere — those would be regressions.
  Concrete next action: either (a) extend the localized-ImGui wrapper macro into `AppController_LuaBindings.cpp` so Lua widgets pick up dictation automatically (need to verify the wrapper doesn't conflict with sol2's macro expansion — non-trivial), or (b) add an explicit `g_dictationRouter.RegisterInputText(buf, cap, nullptr)` call adjacent to the raw `ImGui::InputText` call and an unregister on the next-frame boundary. Option (b) is the minimal change. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · perf-measure · [process] · P2 — "extend the CLI / scenarios if missing, never ask user to run UI manually" rule not encoded in `agents/perf-measure.md`
  Details: User-private memory `feedback_perf_automation.md` (rooted in PR #66 lua-recorded-cmd-list perf friction — `priority-grid-scroll` scenario didn't cover the Lua provider path, manual `SmatchetHooks.lua` edit was required) encodes the rule "if a needed scenario does not exist, extend the CLI / scenario registry first; do not skip the measurement; never fall back to asking the user to run a live UI session". `agents/perf-measure.md` describes the happy-path `perf.reset → scenario.run → perf.snapshot` loop but neither it nor `agents/perf-detective.md` / `agents/spike-hunter.md` state the "extend or fail, never ask user manually" clause. Same gap as ship-loop policy — other harnesses get none of this.
  Concrete next action: add a Hard Rule to `agents/perf-measure.md` and a § Verification automation contract bullet to `agents/perf-detective.md` + `agents/spike-hunter.md`. Suggested wording: "If the validating scenario does not exist, extend `Source_Core/src/Commands/Scenarios/` (and the scenario-arg surface, if needed) as part of the same PR. Never substitute a manual UI session for a missing scenario — the measurement is the deliverable." ~15 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · lua-binder · [process] · P3 — Plan packet "stub parity" framing misleading when receivers are already always-on
  Details: Phase E packet explicitly listed `Source_Core/src/AppController_LuaStubs.cpp` as a MOD path with "mirror stub implementations of the same 3 glue functions" claim. But the Lua surface in question (`ai.*`) calls `AppController::AddAiContext` / `ClearAiContext` / `PromptAi` which are **always-on** members (declared without `SMATCHET_WITH_LUA_AUTOMATION` gate, shipped Phase B specifically so Phase E Lua glue is stable across LUA=ON/OFF + AI=ON/OFF). No stub mirror was needed; the agent added a docstring to LuaStubs.cpp to honour the packet's write-set claim but no functional code change.
  Concrete next action: distinguish two cases in orchestrator delegation packets that touch the LuaBindings ↔ LuaStubs pair — (a) glue calls a Lua-only method on AppController → stub mirror required + `LuaStubsCompile.test.cpp` sentinel update; (b) glue calls an always-on AppController method → **no** stub action; parity invariant already satisfied by the always-on declaration. ~5 min phrasing change to `agents/lua-binder.md` § Hard invariants as a checklist bullet.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · orchestrator · [process] · P3 — `unique_ptr<incomplete-type>` footgun in AGENTS.md § C++14 invariants
  Details: Phase B (PR #163) hit `invalid application of sizeof to incomplete type` errors from every TU that includes `AppController.h` (including `main.cpp` via the PCH) because `AppController.h` held `std::unique_ptr<AiAssistantController>` with only a forward-decl. The header compiled in isolation but failed at every consumer's implicit-dtor instantiation site. Resolution: include the full `AiAssistantController.h`. The general C++ pattern — `unique_ptr<T>` member in a header needs `T` complete at every consumer's implicit-dtor instantiation, NOT just at the owning class's dtor definition site — is non-obvious and bit Phase B. Worth one-line callout in AGENTS.md § Quality / § Project rules so future agents lift to the full include up-front instead of attempting the forward-decl + pImpl pattern that requires manual out-of-line dtor.
  Concrete next action: add a single bullet to AGENTS.md § Quality (~line 92) stating "`std::unique_ptr<T>` member in a class declared in a header — include `T`'s full definition in that header. Forward-decl only works with an out-of-line dtor defined in a TU where `T` is complete; trying it without the out-of-line dtor fires sizeof-incomplete at every consumer." ~5 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · orchestrator · [process] · P3 — Plan packet pre-flight should check "shim wired? what links it?"
  Details: Phase A (PR #140) added `SmatchetCoreAiShim` INTERFACE target to mirror the MCP shim pattern, but never linked it to any consuming target. Phase B (PR #163) discovered the gap when trying to compile AI code in `Source_Core/` — the shim's compile definitions weren't propagating because no `target_link_libraries(<target> SmatchetCoreAiShim)` existed. Phase B had to wire it (CMakeLists.txt +6 LoC). Same pattern likely to repeat for any future `INTERFACE` shim. The orchestrator's plan-time production-file existence check (per AGENTS.md § Orchestrator delegation packet) should be extended to "INTERFACE target linkage check" — for every `add_library(<X> INTERFACE)` named in the plan, confirm at least one `target_link_libraries(... <X>)` is also named, otherwise flag as incomplete.
  Concrete next action: append a one-line bullet to AGENTS.md § Orchestrator delegation packet § Invariant decisions / Plan-time check list. ~5 min.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [process] · P3 — PR #140 `Source_Core/include/AiTypes.h:35,60` `Temperature = -1.0f` and `MaxTokens = 0` sentinels for "unset"
  Details: Future reader could set `0.0f` thinking it's a neutral value and not realise it's the sentinel for "unset".
  Concrete next action: add a comment at each constant naming the sentinel semantics. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [process] · P3 — PR #146 `Source_Core/src/Commands/Scenarios/*.cpp` manual `extern UiDrawSession g_ui;` duplicated across files
  Details: Duplicated across `CommandPaletteFuzzyScenario.cpp` + `DockGapSentinelScenario.cpp` + `BuiltinCommands_Debug.cpp`.
  Concrete next action: promote to an unconditional `extern` in `SmatchetUiSession.h`. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [process] · P3 — PR #140 `TrackerHttpUtils.cpp` clang-format reflow churn mixed into behavioural commit
  Details: Should have been a separate `style:` commit per AGENTS.md commit hygiene.
  Concrete next action: when next touching the file, split style-only changes into their own commit. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [process] · P3 — Plan-doc file-level tables drift from grep ground truth; re-verify before sealing
  Details: While shipping `ai-assistant-side-panel` Phase A, two inaccuracies surfaced in the plan's § File-level changes table that would have wasted agent cycles if delegated blindly:
    1. Plan listed `Source_Core/src/FieldCatalogCache.cpp` among 3 `NetworkUsageTracker::Instance().Record(...)` callers to update with a `HttpTrafficKind::Tracker` first arg. `git grep "NetworkUsageTracker::Instance().Record"` finds the symbol only in `TrackerHttpUtils.cpp` (5×) + `JiraIssueMutation.cpp` (1×). `FieldCatalogCache.cpp` does not call the API.
    2. Plan said "Add new sources to `CORE_SOURCES`" in `CMakeLists.txt`. The list is actually `file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source_Core/src/*.cpp")` (line 530) — new files auto-included, no list edit needed.
  Both cost ≤5 min to re-verify with a single `git grep` + `grep -n CORE_SOURCES CMakeLists.txt` before sealing the file-level table. Without the re-verify, a downstream agent could waste ~10 min hunting for a non-existent caller or writing a redundant `CORE_SOURCES` edit before noticing the glob.
  Concrete next action: orchestrator's plan-time production-file existence check (per `AGENTS.md` § Orchestrator delegation packet) should be extended to "production-symbol existence + production-mechanism shape" — one `git grep <symbol-list>` + one `grep -n <cmake-variable> CMakeLists.txt` before the file-level table is sealed. Five-minute cost catches both classes of drift.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [process] · P3 — `AppControllerDepsAdapter.cpp` is a link-trap for tests
  Details: PR D introduced `Source_Core/src/AppControllerDepsAdapter.cpp` as the production-side implementation of `IOfflineQueueDeps` + `ITicketSyncDeps` against a live `AppController&`. Adding it to a test target's source list drags unresolved `AppController::*` symbols (since `AppController.cpp` is correctly excluded — ImGui-tainted). Tests should always use `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`; the adapter belongs only in the production exe. PR E lost a link-error round-trip before the agent figured this out.
  Concrete next action: add a one-paragraph note to `agents/test-rig.md` § Workflow: "Adapter TUs (`AppControllerDepsAdapter.cpp` and similar) are production-only — never link them into test targets. Always use Fake* fixtures under `tests/support/`." Estimated cost 5 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [process] · P2 — Worktree-absolute vs main-repo-absolute path discipline
  Details: PR F agent initially used `Edit` / `Write` with absolute paths like `C:\Dev\Smatchet\...` while running in worktree `C:\Dev\Smatchet\.claude\worktrees\agent-<id>\...`. Edits landed in the **main repo** (which happened to be checked out on PR E's branch at the time) instead of the agent's own worktree. Agent recovered via `git stash` on main + `git show stash@{0}:path` to copy content into worktree, then re-edited with worktree-absolute paths explicitly. The current `test-rig` prompt's note ("always use absolute file paths") doesn't distinguish worktree-absolute vs repo-absolute. Cost ~5 min recovery + one accidental cross-branch stash.
  Concrete next action: add an explicit instruction to `agents/test-rig.md` (and AGENTS.md § Delegation): "When `Working directory` env shows a worktree path under `.claude/worktrees/<id>/`, all `Edit` / `Write` absolute paths must start with that worktree prefix, NOT the main-repo prefix. Absolute paths to the main repo land changes on whatever branch main is currently on — often a sibling agent's branch." Estimated cost 5 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig + orchestrator · [process] · P3 — Mutation-sanity recipe in test-rig packets needs taxonomy: prod-mutation vs test-mutation
  Details: callstack-adversarial-subcases run (PR #112) hit the auto-mode classifier denying two mutation-sanity recipe steps: (a) production-side substring-prefix relaxation in `ApplyPathRemaps` (legit denial — production was strictly out-of-scope per the packet), (b) test-side fixture mutation that would have removed a load-bearing invariant from a high-risk case (also legit). The current `test-rig` packet language ("one production-side mutation per high-risk case, demonstrably fails the new test, reverted before commit") assumes both options open. In practice, when production code is `Out of scope — refuse if asked`, every prod-side mutation is denied by the classifier. Agent has to argue-from-assertion-shape for 1/4 of the cases and document deferred-with-rationale.
  Concrete next action: split the recipe into (1) production-side mutation when production is in the write set, demonstrably fails, revert; (2) production-side mutation **deferred** when production is out-of-scope — instead, argue from assertion shape + neighbour-test coverage that the production branch is reachable; (3) test-side fixture mutation only when it does NOT remove a load-bearing invariant. Land in `agents/test-rig.md` § Mutation-sanity recipe + AGENTS.md § Orchestrator delegation packet.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [process] · P3 — ReDoS / perf budget figures in agent packets must be STL-backend-qualified
  Details: callstack-adversarial-subcases packet specified `≥64 KiB / 50 ms` for the ReDoS sentinel. On MinGW UCRT64 `std::regex` (`Source_Core/src/CallstackParser.cpp:57-58` regex, `-O2`), probe shows: 256 B → 1 ms, 512 B → 4 ms, 1 KiB → 21 ms, 2 KiB → 101 ms, 4 KiB → 403 ms, ≥ ~32 KiB stack-overflows runner (0xC00000FD). Orchestrator-spec was ~3 orders of magnitude away from achievable. Agent retuned to 1 KiB / 100 ms and routed regex hardening to `p4-blame` via the security-category backlog entry.
  Concrete next action: at packet-composition time, the orchestrator runs a 4-point probe (256B / 512B / 1KiB / 2KiB) for any regex-bearing budget claim before pinning numbers. Land in AGENTS.md § Orchestrator delegation packet § Invariant decisions. Estimated cost 15 min doc edit (packet template note) + one-time 5 min per packet that names a regex budget.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [process] · P3 — API-500 mid-run recovery procedure not documented
  Details: 4/4 Wave A2 agents (`tracker-labels`, `tracker-datetime`, `tracker-payload`, `tracker-field-catalog`) errored API 500 on final-synthesis turn after shipping 100% of file edits. Worktree state was complete and correct — only the agent's report write-up failed. Orchestrator recovered each by: (1) inspecting worktree `git status` for new/modified files, (2) running local gates (`cmake --build`, `ctest`, dual-target), (3) `git add -A && git commit -m '<recovery message>'`, (4) `git push -u origin <branch>`, (5) `gh pr create` with a stand-in body. Tracker-payload required force-push amend because initial `git commit` only included staged files (`tests/CMakeLists.txt` + `TrackerFieldPayload.cpp`) — the 3 new files weren't staged. Recovery cost: ~5-10 min per agent.
  Concrete next action: document the recovery as an operational rule in AGENTS.md § Delegation. Key gotcha: after working-tree inspection, `git add -A` (not `git add <list>`) before commit so new untracked files are included. Estimated cost 30 min doc edit. Land in AGENTS.md § Delegation § API-500 recovery (new sub-section).
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [process] · P3 — Parallel-write-fan-in to `tests/CMakeLists.txt` needs sequential-merge stance documented
  Details: 4 parallel Wave A2 test-rig agents (tracker-labels / datetime / payload / field-catalog) each appended their new test + source `.cpp` to the same lines of `tests/CMakeLists.txt`. Each PR after the first needed manual rebase resolving union-merge — orchestrator absorbed this cost (~5 min per PR). Already documented in `docs/design/test-suite-expansion-completion.md` § Deviations from plan; not in agent-level docs.
  Concrete next action: promote to `agents/test-rig.md` § Parallel-with-N-other-agents note — explicit rule "when N siblings touch `tests/CMakeLists.txt`, append at the END only; merge order is serial; orchestrator handles rebase". Saves explanation in every parallel-batch packet. Estimated cost 10 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · orchestrator · [process] · P3 · OBSERVATIONAL — `_plan-locks.md` stale-read race when concurrent orchestrators / hooks edit
  Details: Multiple times this session `Edit` errored with `File has been modified since read, either by the user or by a linter`. Cause: concurrent orchestrator (`claude/coordination-plan-locks` worktree at `jolly-cerf-97840e`) editing the same file, or PostToolUse hook reformatting. Re-Read-on-stale + re-Edit pattern always recovered.
  Concrete next action: no obvious procedural fix beyond what's already in place. Not actionable today; documented for cross-session continuity awareness.
  Status: observational
  Last-reviewed: 2026-05-17

- 2026-05-14 · architect · [process] · P3 · DEFERRED — `TodoWrite` reminder noise during read-only tasks
  Details: System injected three `TodoWrite` reminders into a read-only validation run. Read-only agents (architect, code-review, security-review, perf-measure) rarely benefit from a todo list; the reminder hook could be muted for them based on the agent banner or `tools:` frontmatter (no `Write`/`Edit`).
  Concrete next action: harness-side (Claude Code injects the reminder unconditionally; not configurable via project settings.json). Re-open once a Claude Code release exposes a per-agent toggle, or once a second harness cites the same noise.
  Status: deferred
  Last-reviewed: 2026-05-17

- 2026-05-12 · tracker-backend · [process] · P3 · DEFERRED — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs use PascalCase
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Concrete next action: C++ rename touching every `RemoteProject` call site (tracker-backend + grid-engine + bulk-import). Architect should scope the rename inside the next PR that legitimately touches `RemoteProject`. Don't open a standalone rename PR — bundle with adjacent work to minimise diff noise.
  Status: deferred
  Last-reviewed: 2026-05-17

- 2026-05-12 · offline-sync · [process] · P3 · DEFERRED — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Concrete next action: small C++ refactor — bundle into the next PR that touches `SaveFieldCatalogSnapshot`. Don't open a standalone refactor PR; the win shows up only when adding the next per-axis arg, which is when the bundling decision gets reviewed in context.
  Status: deferred
  Last-reviewed: 2026-05-17
