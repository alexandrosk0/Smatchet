# Agent self-improvement — process

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-02 · orchestrator · [process] · P2 — Test goldens must be captured from a real run, never asserted by source-reading; orchestrator must run the FULL local suite before "done"
  Details: During `full-function-size-compliance` (PRs #694–#700), three decomposition agents shipped bucket-A tests whose expected values were fabricated by reading the code rather than captured by executing the function: (1) #695 `TrackerFieldValueParser` asserted `"Spent 30m"` for 1800s, but the formatter emits `"0h 30m"`; (2) #699 `CompactDateFormat` hardcoded a date-only golden `"2026-03-15"` that renders in local time → `"2026-03-14"` under EST, failing the full `ctest` on any non-UTC machine; (3) related: #696/#697/#700 shipped comments the comment-audit gate later flagged. CI masked #2 because runners are UTC, and the orchestrator's "tests compile-pending-CI" shortcut (skipping the local full suite because the `ninja-test-msvc` dir was stale) let all of them reach merge. Found only by a post-merge full-suite run during a double-check. Root pattern: a value asserted by inspection is a guess, and a value that flows through timezone/locale/clock/float formatting is a guess that happens to be right in exactly one environment.
  Concrete next action: (a) add to every test-authoring agent prompt (`test-rig` + subsystem specialists when they add tests): "Capture goldens by RUNNING the function or built binary — never transcribe an expected value from source. For any output through locale/timezone/clock/float formatting, assert shape + invariants, not an environment-dependent literal." (b) add to the ship-loop: run the FULL local `ctest` (not CI's filtered bucket-A subset) before declaring a test-bearing slice done. (c) unblock (b) by fixing the stale `ninja-test-msvc` configure so a full local run is cheap (`build-doctor`), and pair with a CI job pinned to a non-UTC `TZ` so this class fails in CI too.
  Status: open
  Last-reviewed: 2026-06-02

- 2026-05-28 · deep-audit · [process] · P3 — Widespread PR-numbered temporal comments violate the comment-hygiene rule
  Details: `docs/agent-rules/delegation.md:23` (comment discipline): "code comments explain durable intent, never task/PR/temporary plans (no comments like `PR 4:` or `remove in PR 7`)." Yet ~24-27 first-party files carry such comments (110+ comment-lines reference `PR<n>`), several describing now-stale future work: `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:178` "PR 7 will replace this…"; `Source/Core/include/ITrackerConnectivity.h:35` "real impls land in PR 4 of the remove-global-project-key rollout"; `Source/Core/src/Config/ConfigManager.cpp:188` "still on TrackerConfig until PR 6 deletes them". Flagged by no tool today. Verified (deep-audit, adversarially confirmed; examples verbatim).
  Concrete next action: sweep first-party `.cpp/.h` for `// PR <n>` / `PRn` comments — delete or rewrite to durable intent (describe what the code does, not which PR touched it); cross-reference design docs by stable slug. Add a cheap grep guard alongside `test-shell-lint`'s C++ checks to stop re-accumulation. ~1-2 h.
  Status: open
  Last-reviewed: 2026-05-28

- 2026-05-21 · orchestrator · [process] · P3 — Plan-revision edits must grep the keyword family before declaring complete
  Details: Commit `491f8425` rewrote ADR 0007 + plan A.12 + plan risks bullet + glossary entry to fix the audit-trail-substrate misnaming (SQLite → JSONL). The rewrite hit every obvious surface but **missed two orphans**: § Decisions locked point 3 still asserted `Schema bump bundled with this plan (agent_audit_trail migration)` and the bucket-A test description still said `+ migration apply`. Second-pass architect review caught both. Recurring failure shape: a substrate / shape / contract rewrite hits the structural sections (§ Approach, § Risks, ADR body) but skips the dense reference sections (§ Decisions locked, test descriptions, file-list rationales) where the same keyword recurs.
  Concrete next action: when the orchestrator finishes a plan-revision edit that fixes a substrate / shape / contract change, run a final `grep` over the plan + ADR + glossary + backlog for the keyword family of the changed concept (here: `migration`, `schema`, `SQLite`, `agent_audit_trail`) and clear every hit before committing. Add this as a § Final-check rule under AGENTS.md § Project rules § Plan revision after implementation. ~10 min doc edit.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-19 · perf-detective · [process] · P3 — Sanctioned probe-scenario-via-lambda flow for ad-hoc perf investigations
  Details: When `perf-detective` needs to drive a render path no production scenario covers (e.g. the AI chat history `DrawHistoryArea` path which is gated on `ImGui::Begin == true` and never fires in a hidden spawn instance), the current process is 5 mechanical steps: (1) write a one-off `IScenario` subclass, (2) register it in `AppController.cpp`'s scenario factory list, (3) build, (4) measure, (5) strip the file + the registration line. Step (5) is the failure mode — deleting the .cpp without removing the registration causes a compile break the next time someone touches `AppController.cpp`. Hit this during PR #311's perf investigation; recovered cleanly but the cleanup pass was a non-trivial scan across two files for residue.
  Concrete next action: add a `ProbeScenario` factory in `Source/Core/include/Commands/Scenarios/` that takes a `std::function<void(int)>` per-frame lambda + an optional `std::function<void()>` setup hook. Auto-deregisters on destructor via a stack-allocated `ProbeScope` RAII wrapper in the perf-detective's investigation source. One TU, no `AppController.cpp` edits, zero residue when the investigation ends. ~3 h to design + implement; saves 15–30 min per future perf-detective investigation that needs a probe path.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-18 · whisper-phase-d · [process] · P3 — `AppController_LuaBindings.cpp:1816` calls `ImGui::InputText` raw, bypassing the `SmatchetLocalizedImGui` dictation wrapper
  Details: Phase D of the whisper-dictation plan auto-wired dictation insertion to every ImGui input via the `SmatchetLocalizedImGui::InputText` / `InputTextMultiline` / `InputTextWithHint` wrappers (the existing `#define ImGui SmatchetLocalizedImGui` pattern). One first-party call site bypasses the wrapper: `Source/Core/src/AppController_LuaBindings.cpp:1816` invokes `ImGui::InputText` directly (it's inside the sol2 binding that lets Lua scripts spawn dynamic widgets — the wrapper macro is intentionally off in that TU). Effect: Lua-authored dynamic InputText widgets in `scripts/*.lua` do NOT participate in dictation; focused-buffer auto-registration skips them. Real-world impact is small (Lua-driven widgets are advanced-user territory; built-in surfaces are all already covered). No NEW raw-`ImGui::InputText` call sites should be allowed elsewhere — those would be regressions.
  Concrete next action: either (a) extend the localized-ImGui wrapper macro into `AppController_LuaBindings.cpp` so Lua widgets pick up dictation automatically (need to verify the wrapper doesn't conflict with sol2's macro expansion — non-trivial), or (b) add an explicit `g_dictationRouter.RegisterInputText(buf, cap, nullptr)` call adjacent to the raw `ImGui::InputText` call and an unregister on the next-frame boundary. Option (b) is the minimal change. ~1 h.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · lua-binder · [process] · P3 — Plan packet "stub parity" framing misleading when receivers are already always-on
  Details: Phase E packet explicitly listed `Source/Core/src/AppController_LuaStubs.cpp` as a MOD path with "mirror stub implementations of the same 3 glue functions" claim. But the Lua surface in question (`ai.*`) calls `AppController::AddAiContext` / `ClearAiContext` / `PromptAi` which are **always-on** members (declared without `SMATCHET_WITH_LUA_AUTOMATION` gate, shipped Phase B specifically so Phase E Lua glue is stable across LUA=ON/OFF + AI=ON/OFF). No stub mirror was needed; the agent added a docstring to LuaStubs.cpp to honour the packet's write-set claim but no functional code change.
  Concrete next action: distinguish two cases in orchestrator delegation packets that touch the LuaBindings ↔ LuaStubs pair — (a) glue calls a Lua-only method on AppController → stub mirror required + `LuaStubsCompile.test.cpp` sentinel update; (b) glue calls an always-on AppController method → **no** stub action; parity invariant already satisfied by the always-on declaration. ~5 min phrasing change to `agents/project/lua-binder.md` § Hard invariants as a checklist bullet.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [process] · P3 — PR #140 `Source/Core/include/AiTypes.h:35,60` `Temperature = -1.0f` and `MaxTokens = 0` sentinels for "unset"
  Details: Future reader could set `0.0f` thinking it's a neutral value and not realise it's the sentinel for "unset".
  Concrete next action: add a comment at each constant naming the sentinel semantics. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [process] · P3 — PR #146 `Source/Core/src/Commands/Scenarios/*.cpp` manual `extern UiDrawSession g_ui;` duplicated across files
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
  Details: PR D introduced `Source/Core/src/AppControllerDepsAdapter.cpp` as the production-side implementation of `IOfflineQueueDeps` + `ITicketSyncDeps` against a live `AppController&`. Adding it to a test target's source list drags unresolved `AppController::*` symbols (since `AppController.cpp` is correctly excluded — ImGui-tainted). Tests should always use `FakeOfflineQueueDeps` / `FakeTicketSyncDeps`; the adapter belongs only in the production exe. PR E lost a link-error round-trip before the agent figured this out.
  Concrete next action: add a one-paragraph note to `agents/core/test-rig.md` § Workflow: "Adapter TUs (`AppControllerDepsAdapter.cpp` and similar) are production-only — never link them into test targets. Always use Fake* fixtures under `tests/support/`." Estimated cost 5 min doc edit.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig + orchestrator · [process] · P3 — Mutation-sanity recipe in test-rig packets needs taxonomy: prod-mutation vs test-mutation
  Details: callstack-adversarial-subcases run (PR #112) hit the auto-mode classifier denying two mutation-sanity recipe steps: (a) production-side substring-prefix relaxation in `ApplyPathRemaps` (legit denial — production was strictly out-of-scope per the packet), (b) test-side fixture mutation that would have removed a load-bearing invariant from a high-risk case (also legit). The current `test-rig` packet language ("one production-side mutation per high-risk case, demonstrably fails the new test, reverted before commit") assumes both options open. In practice, when production code is `Out of scope — refuse if asked`, every prod-side mutation is denied by the classifier. Agent has to argue-from-assertion-shape for 1/4 of the cases and document deferred-with-rationale.
  Concrete next action: split the recipe into (1) production-side mutation when production is in the write set, demonstrably fails, revert; (2) production-side mutation **deferred** when production is out-of-scope — instead, argue from assertion shape + neighbour-test coverage that the production branch is reachable; (3) test-side fixture mutation only when it does NOT remove a load-bearing invariant. Land in `agents/core/test-rig.md` § Mutation-sanity recipe + AGENTS.md § Orchestrator delegation packet.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-20 · orchestrator · [process] · P2 — AI chat panel bucket-E coverage gap (post-feature-completion)
  Details: `docs/plans/shipped/ai-chat-claude-desktop-parity.md` § Verification listed 5 mandatory bucket-E ImGui-Test-Engine scenarios (`ai_chat_pin_bookmark`, `ai_chat_copy_clipboard`, `ai_chat_history_persist`, `ai_chat_clear_confirm`, `ai_chat_keyboard_nav`). None authored — feature shipped on visual sign-off + the new `ai-chat-history-render` perf scenario as evidence. AGENTS.md § Verification automation — zero manual steps says "manual residue without a backlog entry is a fail"; this entry closes that loop. Also: bucket-C screenshot golden bootstrap rig still doesn't exist; AI chat user-bubble + pin-strip + theme-token visuals inherit that existing gap.
  Concrete next action: `test-author` to spec the 5 ImGui-Test-Engine scenarios using the existing `tests/ui/views_columns_reorder.test.cpp` shape + `ninja-ui-test-msvc` preset as the reference. Each scenario is ~30-50 lines of ImGui-Test-Engine driver code (open panel → seed messages via `g_ui` mutation or direct dispatch → click via test engine → assert state). Estimated 3-4 hours total. Per-scenario cost amortised because the seed + open-panel scaffolding is shared.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-16 · orchestrator · [process] · P3 — ReDoS / perf budget figures in agent packets must be STL-backend-qualified
  Details: callstack-adversarial-subcases packet specified `≥64 KiB / 50 ms` for the ReDoS sentinel. On MinGW UCRT64 `std::regex` (`Source/Core/src/CallstackParser.cpp:57-58` regex, `-O2`), probe shows: 256 B → 1 ms, 512 B → 4 ms, 1 KiB → 21 ms, 2 KiB → 101 ms, 4 KiB → 403 ms, ≥ ~32 KiB stack-overflows runner (0xC00000FD). Orchestrator-spec was ~3 orders of magnitude away from achievable. Agent retuned to 1 KiB / 100 ms and routed regex hardening to `p4-blame` via the security-category backlog entry.
  Concrete next action: at packet-composition time, the orchestrator runs a 4-point probe (256B / 512B / 1KiB / 2KiB) for any regex-bearing budget claim before pinning numbers. Land in AGENTS.md § Orchestrator delegation packet § Invariant decisions. Estimated cost 15 min doc edit (packet template note) + one-time 5 min per packet that names a regex budget.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-16 · test-rig · [process] · P3 — Parallel-write-fan-in to `tests/CMakeLists.txt` needs sequential-merge stance documented
  Details: 4 parallel Wave A2 test-rig agents (tracker-labels / datetime / payload / field-catalog) each appended their new test + source `.cpp` to the same lines of `tests/CMakeLists.txt`. Each PR after the first needed manual rebase resolving union-merge — orchestrator absorbed this cost (~5 min per PR). Already documented in `docs/plans/shipped/test-suite-expansion-completion.md` § Deviations from plan; not in agent-level docs.
  Concrete next action: promote to `agents/core/test-rig.md` § Parallel-with-N-other-agents note — explicit rule "when N siblings touch `tests/CMakeLists.txt`, append at the END only; merge order is serial; orchestrator handles rebase". Saves explanation in every parallel-batch packet. Estimated cost 10 min doc edit.
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


