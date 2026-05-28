# Agent self-improvement — process

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-27 · orchestrator · [process] · P1 — Ship-loop breaks autonomy by asking the user for direction at stages that should proceed automatically (merge-gate polling, CR finding triage, post-gate merge)
  Details: During a dummy-change validation of the full ship-loop (PR #472), the orchestrator paused three times to ask the user questions that violate the "one turn without pausing per stage" rule in `docs/agent-rules/ship-loops.md`: (1) After opening the PR, asked "Want me to poll the merge gates, or will you check manually?" — the orchestrator should have immediately started polling via `scripts/dev/merge-gates.sh` per the `[gate-check]` step in the default sequence. (2) After the first poll showed CodeRabbit blocking with 1 actionable finding, asked the user whether to check/resolve it — the orchestrator should have autonomously fetched the CR comment, assessed it, fixed or dismissed it, pushed, and resumed polling. (3) After gates passed, should have proceeded directly to squash-merge without confirmation. None of the five defined exceptions (debug-mode, destructive ops, cross-repo mutations, unauthorised actions, visual-validation) applied. The user had to explicitly point out "it's supposed to be autonomous" before the orchestrator corrected course. Root cause: the ship-loop spec is in `docs/agent-rules/ship-loops.md` (loaded via AGENTS.md), but the orchestrator's in-context prompt does not encode the gate-check → merge → cleanup sequence as a hard "do not pause" contract — it reads as descriptive rather than prescriptive, so the model defaults to its conservative "check with the user" instinct at each stage.
  Concrete next action: (a) Add a § Ship-loop autonomy contract to AGENTS.md § Project rules (inline, ≤5 lines) that encodes the prescriptive rule: "After the user's initial task instruction (and any batched clarifications), the orchestrator MUST NOT use `AskUserQuestion` or pause for confirmation until either (i) an exception from `docs/agent-rules/ship-loops.md` § Exceptions fires, or (ii) the post-ship 4-option menu. Each stage in the default sequence proceeds to the next automatically. CodeRabbit actionable findings are triaged and fixed autonomously; merge-gate polling starts immediately after PR creation; squash-merge fires immediately on GATES_PASSED." (b) Add a concrete checklist to `docs/agent-rules/ship-loops.md` § Autonomous ship-loop default naming the stages where the model is most likely to pause (post-PR-creation, post-CR-finding, post-gate-pass) with explicit "DO NOT pause here" annotations. ~30 min: 10 min AGENTS.md edit + 20 min ship-loops.md checklist.
  Status: open
  Last-reviewed: 2026-05-27

- 2026-05-26 · orchestrator · [process] · P3 — Source_Core source files should be kept under 67 KB; files exceeding this limit must be split and the split technique encoded in project rules
  Details: Three source files exceeded 67 KB and required splitting this session: `SmatchetPreferencesUi.cpp` (144 KB → 5 files), `TicketFieldEditor.cpp` (90 KB → 2 files), `AppController_LuaBindings.cpp` (103 KB → 2 files). Splitting is non-trivial: anonymous-namespace helpers (internal linkage) cannot be shared across TUs; shared state/guards need external linkage or a private header; `thread_local` vars in anonymous namespaces give each TU its own copy (silent breakage if split naively). The techniques used: (1) private `_detail.h` header with extern declaration + inline structs + constexpr constants; (2) move shared functions out of anonymous namespace to file scope (external linkage); (3) keep only truly TU-private helpers in anonymous namespace. None of this is documented in AGENTS.md, so future agents repeat the discovery every time a file grows past threshold.
  Concrete next action: (a) Add a § File-size cap to `AGENTS.md` § Project rules: "Source files in `Source_Core/` and `Plugins/` must stay under **67 KB** (GitHub diff render limit). When a file exceeds this, split before adding more code. Splitting recipe: create `Foo_detail.h` for shared types/guards; move shared free functions out of anonymous namespace (gives external linkage); keep only TU-private helpers in anonymous namespace; `thread_local` and `static` globals in anonymous namespaces have internal linkage — define them at file scope and use `extern` declarations in the private header." (b) Add a cppcheck / script hook that warns (not errors) when a `.cpp` in those directories exceeds 67 000 bytes. ~20 min total: 10 min AGENTS.md edit + 10 min one-liner check in `scripts/dev/`.
  Status: open
  Last-reviewed: 2026-05-26

- 2026-05-21 · orchestrator · [process] · P3 — Plan-revision edits must grep the keyword family before declaring complete
  Details: Commit `491f8425` rewrote ADR 0007 + plan A.12 + plan risks bullet + glossary entry to fix the audit-trail-substrate misnaming (SQLite → JSONL). The rewrite hit every obvious surface but **missed two orphans**: § Decisions locked point 3 still asserted `Schema bump bundled with this plan (agent_audit_trail migration)` and the bucket-A test description still said `+ migration apply`. Second-pass architect review caught both. Recurring failure shape: a substrate / shape / contract rewrite hits the structural sections (§ Approach, § Risks, ADR body) but skips the dense reference sections (§ Decisions locked, test descriptions, file-list rationales) where the same keyword recurs.
  Concrete next action: when the orchestrator finishes a plan-revision edit that fixes a substrate / shape / contract change, run a final `grep` over the plan + ADR + glossary + backlog for the keyword family of the changed concept (here: `migration`, `schema`, `SQLite`, `agent_audit_trail`) and clear every hit before committing. Add this as a § Final-check rule under AGENTS.md § Project rules § Plan revision after implementation. ~10 min doc edit.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-21 · architect · [process] · P3 — Plan template should make "dual-target build gate" a mandatory § callout
  Details: Architect pre-code review of `docs/design/github-tracker-backend.md` flagged the un-gating of `SMATCHET_WITH_AGENTIC` on `GitHubClient*.cpp` as a 🔴 Critical because the plan did not anchor an explicit "must pass dual-target build" check to the un-gate decision. The verification section had `cmake --build … --target SmatchetStandalone SmatchetCore_DX12` listed but it sat in the bulk build-gate list, not at the call-site of the un-gate. Every plan that touches `SMATCHET_WITH_*` source-list gating has this same shape (recurring DX12 pitfall — see ADR 0002 plugin-shim-link-discipline + ADR 0003 github-as-itrackerclient + the `unreal-bridge` agent's existence).
  Concrete next action: amend `docs/design/_plan-template.md` to add a new mandatory § "Dual-target compile gate" section between § Perf-review-system gates and § Risks. Section asks: "does this diff touch `SMATCHET_WITH_*` source-list gating? if yes, anchor the dual-target build cmd to the specific files touched; if no, fill with `N/A — diff is target-agnostic`". ~15 min template edit. Wins on every multi-TU CMake-gating change.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-20 · git-janitor · [process] · P2 — Pre-flight should cross-check `git worktree list` against `.git/worktrees/` to detect orphan on-disk dirs before pruning
  Details: End-of-session cleanup for PR #333 surfaced two worktree-bookkeeping mismatches the current `agents/git-janitor.md` pre-flight does not catch. (1) **`frosty-cohen-dac24d` was on detached HEAD `a3df319a`** (current develop tip) even though the prompt + `git branch -vv` framing said it was on `claude/frosty-cohen-dac24d`. The branch ref had already been deleted before the cleanup session; the unmerged commit `f134169e` (`docs(self-improvement): flag merge-gates STALE / MAX_POLLS / CR-trigger gaps`) was reachable only via that worktree's HEAD reflog (decaying — default 30 days for unreachable, 90 for reachable). `git worktree list` happily auto-discovered the still-on-disk dir as a detached-HEAD worktree even though `.git/worktrees/frosty-cohen-dac24d/` had been pruned. (2) **Phantom empty `epic-gagarin-4f58a5/` dir** at `C:/Dev/Smatchet/.claude/worktrees/epic-gagarin-4f58a5` — never appeared in `git worktree list`, never registered. Pure on-disk residue from a long-gone session. Net cost on this session: the agent had to do reactive `git fsck` + reflog rescue + create salvage tag `salvage/frosty-cohen-dac24d-flag-merge-gates-gaps` mid-cleanup. Worked, but a structural pre-check would have made the rescue proactive (or surfaced the orphan dir to the user before any worktree-prune ran).
  Concrete next action: extend `agents/git-janitor.md` § Destructive-op pre-flight with a new **Step 0 — worktree-bookkeeping cross-check**: (a) for each entry in `git worktree list`, confirm `.git/worktrees/<basename>/` exists; if missing, mark as "orphan on-disk dir, not git-managed — `git worktree prune` will not touch the on-disk content"; (b) for each on-disk dir under `.claude/worktrees/<id>/`, confirm a matching entry in `git worktree list`; if missing, same orphan classification; (c) for any worktree on **detached HEAD**, run `git -C <path> log --oneline HEAD ^origin/develop ^origin/main` to inventory unique commits — if non-empty, create a salvage tag `salvage/<worktree-name>-<short-commit-summary>` BEFORE any prune/remove op. Cross-link from AGENTS.md § Project rules § Destructive git ops in shared worktrees to the new git-janitor sub-section. ~30 min doc edit + 15 min agent-checklist update. Wins on every multi-session cleanup where prior agents pruned `.git/worktrees/<id>/` but left the working dir behind (current observed rate: ~1 per session).
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-20 · orchestrator · [process] · P3 — Default to `git merge` not `git rebase` for squash-merge-destined PR branches
  Details: PR #313's feature branch was 1 commit behind `origin/develop` (PR #312 added the pure-docs-skip block to AGENTS.md while #313 was open). `git rebase origin/develop` started replaying the 10-commit feature history one-by-one and immediately hit a `modify/delete` conflict on `Source_Core/src/AiChatTextEditorRender.cpp` (slice 1 of #311 modified the file; slice 5 of #311 deleted it; develop's merge of #311 then saw the deletion). The rebase machinery would have walked through ~10 sequential conflict-resolutions in the worst case. Aborted with `git rebase --abort`, ran `git merge origin/develop --no-edit` instead → ONE conflict (the same modify/delete on `AiChatTextEditorRender.cpp`), one resolution (`git rm` to confirm the deletion side won), commit, push. Squash-merge collapses the resulting merge commit out of `develop`'s linear history anyway, so the choice between rebase vs merge is purely about how many conflict-resolution rounds the local agent has to do. For branches destined for squash-merge (the standard Smatchet pattern), `merge` is strictly cheaper.
  Concrete next action: extend AGENTS.md § Project rules with a **"Catch-up sync for PR feature branches"** sub-rule: "When `origin/develop` advances while a feature branch is open + the branch needs to sync (e.g. CI says `CONFLICTING`, or before opening the PR), default to `git merge origin/develop --no-edit` rather than `git rebase origin/develop`. Squash-merge will collapse the merge commit; the only saving from rebase is a linear local history, which is moot post-squash. The rebase form's per-commit conflict replay is strictly more work than the merge form's single resolution. Use rebase only when (a) the branch is destined for a non-squash merge, OR (b) the per-commit history is being preserved for some other reason (rare in Smatchet today)." Cross-link from `agents/git-janitor.md` § Standard cleanup loop. ~15 min doc edit.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-19 · perf-detective · [process] · P3 — Sanctioned probe-scenario-via-lambda flow for ad-hoc perf investigations
  Details: When `perf-detective` needs to drive a render path no production scenario covers (e.g. the AI chat history `DrawHistoryArea` path which is gated on `ImGui::Begin == true` and never fires in a hidden spawn instance), the current process is 5 mechanical steps: (1) write a one-off `IScenario` subclass, (2) register it in `AppController.cpp`'s scenario factory list, (3) build, (4) measure, (5) strip the file + the registration line. Step (5) is the failure mode — deleting the .cpp without removing the registration causes a compile break the next time someone touches `AppController.cpp`. Hit this during PR #311's perf investigation; recovered cleanly but the cleanup pass was a non-trivial scan across two files for residue.
  Concrete next action: add a `ProbeScenario` factory in `Source_Core/include/Commands/Scenarios/` that takes a `std::function<void(int)>` per-frame lambda + an optional `std::function<void()>` setup hook. Auto-deregisters on destructor via a stack-allocated `ProbeScope` RAII wrapper in the perf-detective's investigation source. One TU, no `AppController.cpp` edits, zero residue when the investigation ends. ~3 h to design + implement; saves 15–30 min per future perf-detective investigation that needs a probe path.
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

- 2026-05-16 · test-rig · [process] · P3 — `AppControllerDepsAdapter.cpp` is a link-trap for tests
  Details: PR D introduced `Source_Core/src/AppControllerDepsAdapter.cpp` as the production-side implementation of `IOfflineQueueDeps` + `ITicketSyncDeps` against a live `AppController&`. Adding it to a test target's source list drags unresolved `AppController::*` symbols (since `AppController.cpp` is correctly excluded — ImGui-tainted). Tests should always use `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`; the adapter belongs only in the production exe. PR E lost a link-error round-trip before the agent figured this out.
  Concrete next action: add a one-paragraph note to `agents/test-rig.md` § Workflow: "Adapter TUs (`AppControllerDepsAdapter.cpp` and similar) are production-only — never link them into test targets. Always use Fake* fixtures under `tests/support/`." Estimated cost 5 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig + orchestrator · [process] · P3 — Mutation-sanity recipe in test-rig packets needs taxonomy: prod-mutation vs test-mutation
  Details: callstack-adversarial-subcases run (PR #112) hit the auto-mode classifier denying two mutation-sanity recipe steps: (a) production-side substring-prefix relaxation in `ApplyPathRemaps` (legit denial — production was strictly out-of-scope per the packet), (b) test-side fixture mutation that would have removed a load-bearing invariant from a high-risk case (also legit). The current `test-rig` packet language ("one production-side mutation per high-risk case, demonstrably fails the new test, reverted before commit") assumes both options open. In practice, when production code is `Out of scope — refuse if asked`, every prod-side mutation is denied by the classifier. Agent has to argue-from-assertion-shape for 1/4 of the cases and document deferred-with-rationale.
  Concrete next action: split the recipe into (1) production-side mutation when production is in the write set, demonstrably fails, revert; (2) production-side mutation **deferred** when production is out-of-scope — instead, argue from assertion shape + neighbour-test coverage that the production branch is reachable; (3) test-side fixture mutation only when it does NOT remove a load-bearing invariant. Land in `agents/test-rig.md` § Mutation-sanity recipe + AGENTS.md § Orchestrator delegation packet.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-20 · orchestrator · [process] · P2 — AI chat panel bucket-E coverage gap (post-feature-completion)
  Details: `docs/design/ai-chat-claude-desktop-parity.md` § Verification listed 5 mandatory bucket-E ImGui-Test-Engine scenarios (`ai_chat_pin_bookmark`, `ai_chat_copy_clipboard`, `ai_chat_history_persist`, `ai_chat_clear_confirm`, `ai_chat_keyboard_nav`). None authored — feature shipped on visual sign-off + the new `ai-chat-history-render` perf scenario as evidence. AGENTS.md § Verification automation — zero manual steps says "manual residue without a backlog entry is a fail"; this entry closes that loop. Also: bucket-C screenshot golden bootstrap rig still doesn't exist; AI chat user-bubble + pin-strip + theme-token visuals inherit that existing gap.
  Concrete next action: `test-author` to spec the 5 ImGui-Test-Engine scenarios using the existing `tests/ui/views_columns_reorder.test.cpp` shape + `ninja-ui-test-msvc` preset as the reference. Each scenario is ~30-50 lines of ImGui-Test-Engine driver code (open panel → seed messages via `g_ui` mutation or direct dispatch → click via test engine → assert state). Estimated 3-4 hours total. Per-scenario cost amortised because the seed + open-panel scaffolding is shared.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-16 · orchestrator · [process] · P3 — ReDoS / perf budget figures in agent packets must be STL-backend-qualified
  Details: callstack-adversarial-subcases packet specified `≥64 KiB / 50 ms` for the ReDoS sentinel. On MinGW UCRT64 `std::regex` (`Source_Core/src/CallstackParser.cpp:57-58` regex, `-O2`), probe shows: 256 B → 1 ms, 512 B → 4 ms, 1 KiB → 21 ms, 2 KiB → 101 ms, 4 KiB → 403 ms, ≥ ~32 KiB stack-overflows runner (0xC00000FD). Orchestrator-spec was ~3 orders of magnitude away from achievable. Agent retuned to 1 KiB / 100 ms and routed regex hardening to `p4-blame` via the security-category backlog entry.
  Concrete next action: at packet-composition time, the orchestrator runs a 4-point probe (256B / 512B / 1KiB / 2KiB) for any regex-bearing budget claim before pinning numbers. Land in AGENTS.md § Orchestrator delegation packet § Invariant decisions. Estimated cost 15 min doc edit (packet template note) + one-time 5 min per packet that names a regex budget.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [process] · P3 — Parallel-write-fan-in to `tests/CMakeLists.txt` needs sequential-merge stance documented
  Details: 4 parallel Wave A2 test-rig agents (tracker-labels / datetime / payload / field-catalog) each appended their new test + source `.cpp` to the same lines of `tests/CMakeLists.txt`. Each PR after the first needed manual rebase resolving union-merge — orchestrator absorbed this cost (~5 min per PR). Already documented in `docs/design/test-suite-expansion-completion.md` § Deviations from plan; not in agent-level docs.
  Concrete next action: promote to `agents/test-rig.md` § Parallel-with-N-other-agents note — explicit rule "when N siblings touch `tests/CMakeLists.txt`, append at the END only; merge order is serial; orchestrator handles rebase". Saves explanation in every parallel-batch packet. Estimated cost 10 min doc edit.
  Status: open
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

- 2026-05-24 · orchestrator · [process] · P3 — slice→agent routing should reject test-rig for production-header slices
  Details: Wave-A slice 7 of `autonomous-debugging-no-creds.md` was dispatched to `test-rig`. The agent correctly declined: scope was ~80% production code (new `tests/_debug/SmatchetAgentDebug.h` production-resident header, `Source_Core/include/Logger.h` macro edit, CMake option wiring, `docs/agent-rules/delegation.md` docs edit, branch/commit/push/PR ship authority) — all outside test-rig's "doctest files under `tests/Source_Core/`" charter. Re-dispatched to `general-purpose` and shipped (PR #445). Cost a clean abort + restart cycle; clean self-flag from test-rig (no half-shipped state).
  Concrete next action: extend the orchestrator's slice→agent routing heuristic in `docs/agent-rules/delegation.md` § Subsystem specialists with: "if slice creates new production headers or edits `Source_Core/include/*.h`, do NOT route to test-rig — route to the closest subsystem specialist or `general-purpose`. test-rig stays scoped to tests/Source_Core/ doctest expansion against an already-shipped helper." 5-minute edit; surfaces only on the next slice that pre-mixes production + test surfaces.
  Status: open
  Last-reviewed: 2026-05-24

- 2026-05-24 · orchestrator · [process] · P3 — plan-doc drafts should grep-verify pre-named TUs don't already exist
  Details: Wave-A slice 1 of `autonomous-debugging-no-creds.md` directed the agent to create `Source_Core/{include,src}/GitHubIssueMappingPure.{h,cpp}`. Those names didn't exist, but the equivalent pure helpers had already shipped under different names from PR12 (`GitHubIssueSearchMapping.{h,cpp}` + `GitHubClientHelpers.{h,cpp}` + `GitHubQueryFromJql.{h,cpp}`). Agent caught the duplication via a 30-second sanity grep and reused the existing TUs — correct outcome, but the plan author shouldn't have to depend on per-agent rigour. Low friction this round; high cost the day an agent doesn't catch it and lands a parallel duplicate.
  Concrete next action: add a one-liner to `docs/design/_plan-template.md` § Files to modify: "Before listing a new `<Foo>.{h,cpp}` here, run `rg -l '<Foo>' Source_Core/` to confirm it doesn't already exist under that or a synonym name." 2-minute edit. Surfaces every time a plan author lists fresh TUs without first grepping the codebase.
  Status: open
  Last-reviewed: 2026-05-24

- 2026-05-24 · orchestrator · [process] · P3 — slice coordination paragraphs should call out the "if sibling slice hasn't landed yet" case
  Details: Wave-A slice 2 of `autonomous-debugging-no-creds.md` § Coordination read "your env-hook addition adjacent to Jira's [from slice 1]". Slice 1 hadn't merged when slice 2's agent started; the agent had to design the hook shape from scratch rather than pattern-match. Friction was mild (the hook block is small) but a sibling slice that depended on a non-trivial slice-1 surface would have stalled.
  Concrete next action: add a one-liner to `docs/design/_plan-template.md` § Per-slice "Coordination" section template: "If your slice depends on or copies a pattern from a sibling slice that hasn't merged yet, include the pattern's intended shape (3-5 lines of code or a fixture-name list) inline in this slice's Coordination paragraph so the agent doesn't have to invent it." 2-minute edit; surfaces every parallel-dispatched plan.
  Status: open
  Last-reviewed: 2026-05-24


