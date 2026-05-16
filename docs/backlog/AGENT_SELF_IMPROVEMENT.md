# Agent self-improvement — entries

> Format / categories / workflow / triage cadence: see [`../agents/self-improvement.md`](../agents/self-improvement.md).

<!-- Latest first. Append new entries at the top. -->

- 2026-05-15 · git-janitor · [tooling] — `git pull --rebase --empty=drop` unsupported on shipped git version
  Details: `git-janitor` "Bringing `develop` to latest" snippet uses `git pull --rebase --empty=drop`. The `--empty=drop` option is not recognised by the git version on this Windows host. Chained behind `&&`, the pull failure short-circuited the subsequent `git branch -D` of the merged feature branch, leaving an undeleted local branch the agent had to clean up in a second pass. Wasted one full-tree round-trip per session-close. Proposal: agent should probe `git version` once at the start (or just use `git pull --ff-only` which is the correct operation when `develop` has no local divergence post-merge — the rebase path is dead code in the clean-merge scenario this agent runs in 100% of the time).
  Status: applied — `agents/git-janitor.md` § Bringing `develop` to latest now uses `git pull --ff-only` (replication-lag belt below already used `--ff-only`; the leading line is now consistent with the belt). The agent's contract bans direct pushes to `develop`, so local develop is always upstream-tracking post-merge — FF is the correct op and the rebase path was dead code.

- 2026-05-15 · git-janitor · [process] — PR-only-to-`develop` rule has no FF-clean docs-only escape valve
  Details: On a solo-dev repo, the realistic end-of-session state is a clean local `develop` strictly ahead of `origin/develop` with docs / scripts / tests commits and no C++ behavior changes. `git-janitor` contract bans direct pushes to `develop`, so it has to halt with `partial` and hand the push back to the user every session, even when (a) local is strictly ahead (FF-clean), (b) all commits touch only docs/scripts/tests, (c) regression gates pass. This session: 7 commits queued (`67a6b3b`..`6400ed2`), all docs/scripts/tests, mirror + `scripts/dev/test-all.sh` both green, user had to issue `git push origin develop` by hand. Proposal: define an explicit "FF-clean docs batch" escape valve in `agents/git-janitor.md` — when (FF-clean) ∧ (no path under `Source_Core/`, `Plugins/`, `Target_Standalone/`, `UnrealPlugins/`, `CMakeLists.txt`, `cmake/`) ∧ (all gates pass), the agent may push `origin develop` directly. Anything that touches build / C++ / packaging still falls back to PR-only.
  Status: applied — agents/git-janitor.md § FF-clean docs-batch exception; version bumped 1 → 2; banner + Hard refusals updated with pointer to the exception.

- 2026-05-15 · test-rig · [context] — `JiraClient.h` cascade blocks per-cpp testing of `TrackerFieldValueParser`
  Details: `TrackerFieldValueParser.h` includes `JiraClient.h`, which transitively pulls `ITrackerClient.h` + `ConfigManager.h` + HTTP / cpr surfaces. Linking `TrackerFieldValueParser.cpp` into `SmatchetTests` would force the entire tracker / HTTP / config stack into the test exe, violating the rig's pure-logic invariant. Split `JiraClient.h` so the `TrackerField` / `TrackerFieldOption` / `TrackerUser` POD types live in a small header (e.g. `TrackerFieldSchema.h` — already exists) and `TrackerFieldValueParser.h` depends on that small header only. Then `ParseWorkDurationToSeconds` + `NormalizeTrackerFieldValue` + friends become testable per-cpp.
  Status: applied (03576ff) — `Source_Core/include/TrackerFieldValueParser.h` now `#include "TrackerFieldSchema.h"` instead of `JiraClient.h`; `FormatWorkDurationFromSeconds` declaration moved from `JiraClient.h:18` to the value-parser header (semantically pairs with `ParseWorkDurationToSeconds`); `Source_Core/src/TrackerGridFieldDisplay.cpp` picked up the new include so the call site still resolves. `tests/Source_Core/TrackerFieldValueParser.test.cpp` ships 10 cases / 38 assertions covering parse/format/round-trip. Test exe pulls TrackerFieldValueParser.cpp + TrackerFieldValueUtils.cpp + Logger.cpp + ConfigManager.cpp (the last needs `crypt32` on Windows for CryptProtectData — added to `tests/CMakeLists.txt`). Dual-target Standalone + DX12 build green; ctest 1/1.

- 2026-05-15 · test-rig · [context] — `OfflineCreateQueue::kMaxReplayAttempts` lives behind `<SQLiteCpp/SQLiteCpp.h>`
  Details: The replay-cap constants are declared in `Source_Core/include/LocalCacheManager.h`, which `#include <SQLiteCpp/SQLiteCpp.h>` for the `CachedTicket` / `PendingCreate` / `PendingFieldEditRecord` structs that share the file. The doctest rig bans SQLite includes, so testing the cap arithmetic requires either: (a) moving the two `constexpr int kMaxReplayAttempts = 5;` declarations into a new tiny header (e.g. `OfflineQueueCaps.h`) with no SQLite include, or (b) lifting the cap-decision logic out of `OfflineQueueService.cpp` into a free function `bool ShouldArchiveAfterAttempt(int currentAttempts)` that lives in its own .cpp with no SQLite dep. Option (b) is the better long-term shape because it lets the test exercise the decision boundary, not just compare the literal against 5.
  Status: applied (86895de) — option (b) landed. New `Source_Core/include/OfflineQueueReplayPolicy.h` declares `kMaxReplayAttempts = 5` + inline `ShouldArchive(int currentAttempts, int maxAttempts = kMaxReplayAttempts)`; zero banned includes. `LocalCacheManager.h`'s `OfflineCreateQueue::kMaxReplayAttempts` and `OfflineFieldEditQueue::kMaxReplayAttempts` now alias the policy constant so the single source of truth lives outside the SQLite-tainted header. `Source_Core/src/OfflineQueueService.cpp` updated at four decision sites (two pre-attempt gates + two post-failure gates across `TickOfflineFieldEdits` + `TickOfflineCreates`) to call `OfflineQueueReplayPolicy::ShouldArchive(...)`. `tests/Source_Core/OfflineQueueReplayPolicy.test.cpp` ships 5 cases / 26 assertions covering the literal sentinel, pre-attempt boundary, post-failure loop, explicit-maxAttempts override, and negative-attempts defensive behaviour. Dual-target build green; SmatchetTests aggregate now 35 cases / 133 assertions.

- 2026-05-14 · lua-binder · [context] — sol2 v2.20.6 API limitations not in plan
  Details: Plans wrote `sol::this_state` first-param on usertype member functions and `sol::no_constructor` as a positional sentinel in `new_usertype(...)`. Both fail to compile with sol2 v2.20.6 (template `make_string_view` rejection on usertype-method signatures; no-constructor sentinel signature mismatch). Burned ~15 min of iteration. Add a hard invariant to `agents/lua-binder.md`: "sol2 v2.20.6 — recorder/usertype methods take plain args (no `sol::this_state` first param); `new_usertype` takes only `name` + method-name/ptr pairs, no constructor sentinel."
  Status: applied — agents/lua-binder.md § Hard invariants

- 2026-05-14 · lua-binder · [context] — Lua-as-C++ needs more than `LANGUAGE CXX`
  Details: Lua 5.3's `lua.h` lacks `extern "C"` guards, so flipping the source language to CXX mangles every Lua API symbol and the entire host fails to link. The plan for `lua-recorded-cmd-list` named only `set_source_files_properties(... LANGUAGE CXX)`; agent had to discover the second-step `luaconf.h` patch (or wrap host includes in `extern "C"`) the hard way. Add to `agents/lua-binder.md` hard invariants: "Compiling Lua 5.3 as C++ requires patching `luaconf.h` (or wrapping host-side `#include <lua.hpp>` blocks) with `extern \"C\"` — `LANGUAGE CXX` alone is insufficient."
  Status: applied — agents/lua-binder.md § Hard invariants

- 2026-05-14 · architect · [tooling] — `mcp__vexp__get_skeleton` empty result on indexed files
  Details: During plan-validation pass, `get_skeleton` returned "no skeleton data" for three files known to be in the vexp index. Forced fallback to `Read` for line-confirmation, adding 4 tool calls. Worth a pre-flight `index_status` health check before any read-only validation pass, or a guard in `agents/architect.md` to fall back automatically when skeleton returns empty for an indexed file.
  Status: applied — agents/architect.md § Pre-flight (fall back to Read on empty skeleton, optional index_status at start of long runs)

- 2026-05-14 · architect · [process] — `TodoWrite` reminder noise during read-only tasks
  Details: System injected three `TodoWrite` reminders into a read-only validation run. Read-only agents (architect, code-review, security-review, perf-measure) rarely benefit from a todo list; the reminder hook could be muted for them based on the agent banner or `tools:` frontmatter (no `Write`/`Edit`).
  Status: deferred — harness-side (Claude Code injects the reminder unconditionally; not configurable via project settings.json). Re-open once a Claude Code release exposes a per-agent toggle, or once a second harness cites the same noise.

- 2026-05-13 · p4-blame · [process] — multi-file split handoff packet missed transitive call closure
  Details: When splitting `BlameAnalysisUi.cpp` (2430 → 7 TUs), the orchestrator's handoff packet enumerated modal helpers explicitly but omitted export builders (`BuildAiExport`, `BuildBlameExport{Csv,Json}`, `BuildCallstackRowTsv`, `BuildAnnotatedRowTsv`) that `DrawWindow` calls. Agent had to discover them mid-split. Rule for orchestrator handoff packets on file-split tasks: include an explicit closure rule — "everything `<target-fn>` calls that isn't already in another TU goes to <bucket>" — instead of enumerating from memory.
  Status: applied — see commit at HEAD; AGENTS.md § Orchestrator delegation packet § File-split closure rule.

- 2026-05-13 · p4-blame · [context] — missing-include after split is silent until build
  Details: When extracting helpers from a monolithic `.cpp` into separate TUs, includes that were only in the original `.cpp` are not catchable by inspection — only by build. `CompactDateFormat.h` was needed by both `_Config.cpp` and `_Window.cpp` after split but wasn't in the shared internal header. Rule for split-refactor agents: after creating the shared internal header, scan the original `.cpp`'s include list and replicate every non-self include into the internal header to pre-empt this class of failures.
  Status: applied — see commit at HEAD; AGENTS.md § Orchestrator delegation packet § Post-split include-replication rule.

- 2026-05-13 · orchestrator · [process] — branch-switch wipes untracked plan files
  Details: Working-tree-only files (e.g. a plan doc not yet `git add`-ed) are silently lost on `git checkout <other-branch>` when GitHub Desktop or `git reset --hard` runs. Recovery via `git fsck --lost-found` + content-search on dangling blobs is slow. Rule: as soon as a plan file lands at `docs/design/`, `git add` + commit it immediately, even with a `wip:` prefix, before any other work or branch operation. Never leave a plan file untracked across a session boundary.
  Status: applied (40c0bb2 — AGENTS.md § Project rules § Plan-doc safety)

- 2026-05-13 · orchestrator · [process] — wrong-exe testing burns iterations
  Details: When multiple build outputs exist (`build/ninja-release/`, `build/ninja-iter-msys2/`, worktree builds), the user can easily run the OLD exe and report bugs that the new patched exe doesn't have. Wasted ~5 round-trips this session. Rule: after each rebuild, `ls -la` both the patched and the most-likely-stale exe paths, print mtimes side-by-side, and tell the user explicitly which path to run. Same rule applies to subagents diagnosing perf or layout bugs.
  Status: applied (16eb7af — AGENTS.md § Debug techniques § Exe staleness check + agents/{perf-detective,spike-hunter,build-doctor}.md hard rules)

- 2026-05-13 · orchestrator · [process] — schema-version churn
  Details: The `kCurrentLayoutSchemaVersion` constant got bumped seven times in one session (1→2→3→4→5→6→7→8→9, then back to 2). Each bump shipped as its own commit because the user ran each intermediate fix and reported new symptoms. Rule: when a feature requires a config schema bump, hold the version bump until the feature is verified working end-to-end. Do not commit interim version bumps — squash or amend. Final version should be exactly one higher than the previous shipped version.
  Status: applied (40c0bb2 — AGENTS.md § Project rules § Schema-version bumps)

- 2026-05-13 · orchestrator · [shortcut] — pink-diagnostic clear color for UI gap detection
  Details: For "is the background ever visible behind panels?" questions: set `glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` (or `ImVec4(1,0,1,1)` on DX12 RTV clear). Pink is rare in normal UI palettes; any visible pink is a guaranteed dock gap or transparent region. Combine with automated screenshot + per-pixel pink scan for objective regression tests (script template in this session's `debug.window.screenshot` PPM pipeline).
  Status: applied (40c0bb2 — AGENTS.md § Debug techniques § Pink-clear UI gap detection)

- 2026-05-13 · spike-hunter / orchestrator · [context] — ImGui docking state cannot be re-parented at runtime
  Details: `ImGui::LoadIniSettingsFromDisk()` after the first frame does NOT move already-created docked windows to new DockIds. A common bug pattern: schema migration runs post-load (in the per-frame Draw), windows stay at old positions. Rule: any dock-layout migration must run BEFORE `io.IniFilename` is set and the first `ImGui::NewFrame` call. In Smatchet this means inside `SmatchetImGuiHost::Initialize` (DX12) and inside `main.cpp` before `ImGui_ImplOpenGL3_Init` (Standalone).
  Status: applied (45c14c9 — agents/grid-engine.md + agents/unreal-bridge.md § Hard invariants)

- 2026-05-13 · code-review · [tooling] — lint hook does not run clang-format on newly created `.h` files
  Details: The `PostToolUse` hook via `.claude/hooks/lint-cpp.sh` runs clang-format on `.cpp` edits but not on new header files. New headers consistently arrive at code-review with alignment-padding violations that could have been auto-fixed. Add `*.h` (or `*.{cpp,h}`) to the hook's glob pattern in `.claude/settings.json` or the lint script.
  Status: applied · root cause was script-side, not harness-side. Sentinel-log reproducer (2026-05-15 session) confirmed `PostToolUse` fires on every `Edit`/`Write` (including `.h`, including non-C++ files like `.sh`), so the prior "harness-side" hypothesis was wrong. Real bug: under MSYS2 the hook received `CLAUDE_PROJECT_DIR=/c/Dev/Smatchet` (POSIX form) while `tool_input.file_path` arrived as `C:\Dev\Smatchet\...` (Windows form). After backslash normalisation, `NORM_PROJ=/c/Dev/Smatchet` and `NORM_FILE=C:/Dev/Smatchet/...` shared no prefix, so `REL="${NORM_FILE#$NORM_PROJ/}"` left `REL` as the full path. The case glob `Source_Core/*.cpp|Source_Core/**/*.h|...` therefore never matched anything, the script silently exited 0, and neither `.cpp` nor `.h` edits actually triggered clang-format / cppcheck / clang-tidy. The reason `.h` looked worse than `.cpp` in code-review output was observer bias — most `.cpp` edits enter pre-formatted, so the no-op was invisible. Fix: normalise both `NORM_PROJ` and `NORM_FILE` via `cygpath -m` (idempotent across `/c/...`, `C:/...`, `C:\...`) before the prefix strip, so `REL` becomes `Source_Core/include/Foo.h` and the case glob matches. Verified end-to-end with a deliberately misformatted `Source_Core/include/_lint_hook_probe.h` `Write` — hook reformatted in place + surfaced cppcheck stderr. No upstream report needed.

- 2026-05-13 · architect · [process] — skip architect when prompt already specifies file paths + symbols + commit messages
  Details: Orchestrator dispatched a fully-specified three-commit implementation task (symbols, file paths, commit messages all pre-specified) to the `architect` agent (read-only design role). Architect cannot edit files or build. Rule of thumb to add to delegation heuristic: if the prompt already specifies file paths + symbols + commit messages, design is resolved — skip architect, go direct to general-purpose or the matching subsystem specialist.
  Status: applied (40c0bb2 — AGENTS.md § Heuristic, new bullet)

- 2026-05-12 · orchestrator · [process] — pre-resolve hard-invariant collisions before delegating implementation slices
  Details: The project-key run burned a full first attempt when a backend agent correctly paused on whether `ITrackerClient::FetchFieldCatalog` could be widened. The orchestrator already had AGENTS.md invariants in context; future delegation packets should state the approved option up front.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Invariant decisions; tracker-backend.md ITrackerClient widening rule hardened)

- 2026-05-12 · orchestrator · [context] — build one shared literal inventory and pass it to every delegated agent
  Details: Multiple agents rediscovered the same `cfg.ProjectKey` call sites. For exact symbol / literal work, do one exhaustive text-search in the orchestrator and pass `<file>:<line>:<role>` lines to each agent.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Shared inventory)

- 2026-05-12 · orchestrator · [context] — inline relevant design-doc sections in agent prompts and forbid rereads unless blocked
  Details: Agents reread the same long design doc even when each PR only needed a few sections. Paste the needed excerpt into the delegation packet and say whether reopening the source doc is allowed.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Inline task context)

- 2026-05-12 · orchestrator · [process] — cap routine implementation reports to short table form
  Details: PR 6 emitted a long prose report where a table of files / decisions / verification would have preserved the signal at much lower output cost. Default to `Report <= 200 words, table form, no prose paragraphs` for routine implementation agents.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Output budget)

- 2026-05-12 · orchestrator · [process] — remind subagents that code comments must not reference the task or PR plan
  Details: Several generated comments named PR numbers or temporary removal plans. Add the reminder to delegation packets because global comment discipline may not propagate reliably into delegated contexts.
  Status: applied (d4714ad — AGENTS.md "Orchestrator delegation packet" § Comment discipline)

- 2026-05-12 · orchestrator · [tooling] — allow text-search first for exhaustive literal / symbol inventories
  Details: vexp is best for ownership and impact, but graph-ranked semantic search is the wrong tool for "find every occurrence of this exact string." AGENTS.md should explicitly carve out mechanical inventory / rename / cleanup tasks.
  Status: applied (d4714ad + follow-up — AGENTS.md "Semantic-search exceptions" section, placed outside the auto-managed vexp block so it survives vexp tool updates)

- 2026-05-12 · orchestrator · [tooling] — dedupe or cap repeated lint-hook diagnostics from PostToolUse
  Details: The hook correctly runs after every C++ edit, but large multi-file changes can stream repeated analyzer output into every agent context. Consider per-file digesting, duplicate suppression, or a quiet mode that still surfaces real errors.
  Status: applied (d4714ad — `.claude/hooks/lint-cpp.sh` adds `format_issues` (awk dedupe + cap); `SMATCHET_LINT_MAX_LINES` env var, default 120, documented in `agents/build-doctor.md`)

- 2026-05-12 · command-system · [shortcut] — when the harness lint hook auto-runs on every edit, don't also run a batch `clang-format` at the end
  Details: Doing so produced large reformat diffs on `BuiltinCommands.cpp` / `PlaneClient.cpp` during PR 6 of the project-key removal. The PostToolUse hook in `.claude/settings.json` already covered every edited file.
  Status: applied (45c14c9 — agents/command-system.md § Hard invariants, new bullet)

- 2026-05-12 · grid-engine, command-system · [context] — localization accessor is `SmatchetLocalization::T(key, englishFallback)`, not `Loc(...)` / `Translate(...)`
  Details: Both PR 4b and PR 6 agents guessed wrong names and only converged via grep. Add a one-line note to `agents/grid-engine.md` and `agents/command-system.md`.
  Status: applied (d4714ad — agents/grid-engine.md L55 + agents/command-system.md L54 both carry the `SmatchetLocalization::T(key, englishFallback)` invariant)

- 2026-05-12 · grid-engine · [process / new-agent] — design-doc PRs that span ≥3 subsystems have no clear owner
  Details: PR 4 of the project-key removal touched tracker-backend (`ListProjects`) + grid-engine (draft picker, view pill) + bulk-import + i18n. `grid-engine` paused and asked for a split, which was correct but cost a round-trip. Either add an explicit `pr-driver` meta-agent that splits design-doc PRs into subsystem sub-delegations, or add a note to AGENTS.md instructing the orchestrator to pre-split such PRs before delegating.
  Status: applied (d4714ad — AGENTS.md § Orchestrator delegation packet § Subsystem split bullet covers pre-split rule; `pr-driver` meta-agent not pursued — orchestrator-side discipline is sufficient)

- 2026-05-12 · tracker-backend · [context] — `RemoteProject` POD uses lowerCamelCase (`id`, `key`, `displayName`) while most other DTOs in `Source_Core/include/` use PascalCase (`Id`, `Name`, …)
  Details: Style drift introduced in PR 1. Worth normalizing before more call sites accumulate. Architect call.
  Status: open
  Defer: C++ rename touching every `RemoteProject` call site (tracker-backend + grid-engine + bulk-import). Architect should scope the rename inside the next PR that legitimately touches `RemoteProject`. Don't open a standalone rename PR — bundle with adjacent work to minimise diff noise.

- 2026-05-12 · tracker-backend · [tooling / new-agent] — no test rig in the repo
  Details: pure-C++14 helpers (`JqlProjectScope`, value parser, JQL surgery) had to invent compile-only test patterns that aren't actually executed. A small `test-rig` agent that wires up a CTest target with doctest/GoogleTest against `Source_Core` would unblock real unit tests. High ROI given how much pure-logic code lives there.
  Status: applied · 5-commit migration per `docs/design/applied/test-rig-agent.md` landed on `develop`: 97ab7f1 (rig bootstrap — `tests/CMakeLists.txt` + doctest v2.4.11 FetchContent + `SMATCHET_BUILD_TESTS` option + `ninja-test-msys2` preset + first `JqlProjectScope.test.cpp`), 3b47ff0 (`agents/test-rig.md` + AGENTS.md row), 7f024fc (TextMerge + JsonParseUtil tests — plan deviation: `TrackerFieldValueParser` skipped due to `JiraClient.h` cascade, `OfflineQueueReplay` skipped because `kMaxReplayAttempts` lives in `LocalCacheManager.h` which `#include <SQLiteCpp/SQLiteCpp.h>`, banned per rig invariants), 1f2ad93 (lint-cpp.sh filter extended to `tests/**` + per-target compile_commands), plus the plan revision + this status flip. Final state: 20 test cases / 69 assertions; `ctest --output-on-failure` 1/1 green on `ninja-test-msys2`. Two follow-on items surfaced (split `JiraClient.h` so `TrackerField*` is testable; lift offline-queue replay-cap decision into a free function) — filed as separate backlog entries below.

- 2026-05-12 · tracker-backend · [context] — design-doc PR sections that list line numbers should mark each as `(cfg-read)` / `(draft-write)` / `(audit-only)`
  Details: Project-key PR 2 §2.3 listed lines 358, 382, 70, 92, 349 alongside `cfg.ProjectKey`-read sites, but they were draft-writes — required a disambiguation pass.
  Status: applied (d4714ad — AGENTS.md § Orchestrator delegation packet § Shared inventory mandates `<file>:<line>:<role>` with the exact role suffixes)

- 2026-05-12 · tracker-backend · [tooling] — `mcp__vexp__run_pipeline` rejects `max_tokens` as float when JSON wire format is double
  Details: Surfaced as "floating point, expected usize" — schema should accept integers-as-floats or improve the error message.
  Status: open
  Defer: External — vexp tool source lives outside this repo. File an issue / PR at the vexp project; not actionable in Smatchet. Workaround: cast to int literal in callers (`max_tokens: 12000` not `max_tokens: 12000.0`).

- 2026-05-12 · offline-sync · [shortcut] — `SaveFieldCatalogSnapshot` accumulated 4 extra primitive args; a `FieldCatalogSaveContext` struct would prevent future drift
  Details: callers already had each arg in scope; bundling them into one struct keeps the call site narrow as more per-axis state lands.
  Status: open
  Defer: Small C++ refactor — bundle into the next PR that touches `SaveFieldCatalogSnapshot`. Don't open a standalone refactor PR; the win shows up only when adding the next per-axis arg, which is when the bundling decision gets reviewed in context.

- 2026-05-12 · command-system · [process] — when a PR plan names a specific line/symbol, do a 30-second sanity grep before editing
  Details: Project-key PR 6 plan flagged `AppController_LuaBindings.cpp:~L254` as a "Lua config setter to deprecate" — it was actually `LuaApplyIssueCreateKv` (per-operation draft kv, not a config setter). One round-trip cost. Agent correctly flagged back to orchestrator before editing.
  Status: applied (45c14c9 — agents/command-system.md § Workflow step 3 + agents/tracker-backend.md § Workflow step 1)

- 2026-05-13 · orchestrator · [process] — vexp `<!-- vexp -->` block auto-regenerates inside `AGENTS.md`; should land in `.claude/CLAUDE.md` instead
  Details: AGENTS.md is the harness-agnostic root per the agents.md spec. The vexp tool injects ~30 lines of Claude-Code-specific MCP guidance (`run_pipeline`, `get_skeleton`, MCP tool list) directly into AGENTS.md, which other harnesses load and ignore. Editing the block in-place fights the regenerator. Upstream fix: vexp tool emits to `.claude/CLAUDE.md` only; the `@`-import in `.claude/CLAUDE.md` then pulls AGENTS.md without the vexp section.
  Status: open
  Defer: External — vexp tool source lives outside this repo. File an issue / PR at the vexp project. Workaround in the meantime: leave the block alone; the ~250 input-token cost per session is small relative to the auto-regen friction of fighting it.

- 2026-05-13 · orchestrator · [process] — ASCII em-dash banner bars (`━━━`) at agent open / close burn input + output tokens per call with no routing value
  Details: Every agent carried 4 `━` rules (open instr + open banner + close instr + close banner) plus the instruction prose ("Begin every response with this banner ... Use the horizontal rules"). ~9 lines × 18 agents = 162 lines of pure ceremony; emit cost is ~24 output tokens per subagent call. Banner content is per-agent unique only by `name` + `model/complexity/access`. Replaced with one-line banner spec: `**Banner** — open: \`🤖 AGENT: name · model/complexity · access\`. Close (before \`## Self-improvement\`): \`✅ END — name · model/complexity · access\`.`
  Status: applied (94d5836)

- 2026-05-13 · orchestrator · [context] — generic "Semantic search first" preamble duplicated across 11 agents; covered by AGENTS.md § Semantic codebase search
  Details: 11 of 18 agents carried near-identical `**Semantic search first** — call your harness's semantic codebase search ...` boilerplate. ~45 words × 11 = ~500 redundant input tokens permanently in agent corpus. Dropped from agents with no agent-specific guidance; kept on agents that add real twists (build-doctor's CMake file-read note, perf-detective / spike-hunter's `preset: debug` hint, perf-instrument / mechanic's exhaustive-text-search rule, code-review / security-review's process sections, perf-measure's CLI focus, debug-detective's Search Order). AGENTS.md § Semantic codebase search owns the canonical rule.
  Status: applied (94d5836)

- 2026-05-13 · debug-detective · [tooling] — NDJSON helper C++ template embedded inline in agent prompt (~85 lines)
  Details: Section 4a wrote the full `SmatchetAgentDebug.h` body inside the agent prompt, so every debug-detective invocation pulled the helper into the system prompt even when no instrumentation was needed. Externalized to `agents/_shared/templates/SmatchetAgentDebug.h.tmpl` with a one-line `sed`-based copy in the prompt. debug-detective.md shrunk 633 → 547 lines (−86); per-invocation input savings are larger since the helper body now loads only when materialised.
  Status: applied (d79a8fc)

- 2026-05-13 · orchestrator · [process] — `delegates-to:` frontmatter present on 8 of 18 agents with no documented rule
  Details: code-review, debug-detective, grid-engine, mcp-toolsmith, offline-sync, perf-detective, spike-hunter, unreal-bridge listed it; others omitted it. Field is informational (Claude Code does not consume it; orchestrators may). Audit showed allocation is intentional — agents that **directly call** another agent list targets; terminal agents (mechanic, perf-instrument, perf-measure, build-doctor, command-system, p4-blame) and orchestrator-hand-back agents (architect, security-review, tracker-backend, lua-binder) omit it. Documented in AGENTS.md § `delegates-to:` frontmatter — absence ≠ "never delegates" but "via orchestrator, not direct."
  Status: applied (d79a8fc)

- 2026-05-13 · orchestrator · [tooling] — `harness-hints.claude-code.tools:` line duplicates `capabilities:` list and goes unread by Claude Code (which parses top-level `tools:`)
  Details: ~104 chars per `tools:` line × 18 agents = ~470 tokens. Top-level `tools:` is what Claude Code actually consumes for permission restriction; the nested hint is informational only. AGENTS.md § Harness adapter is the canonical capability → tool mapping. Dropped the line everywhere; kept `model:` + `effort:` (real routing knobs).
  Status: applied (d6ba897)

- 2026-05-13 · test-author · [process] — verification-automation cadence not project-wide; manual residue could ship indefinitely
  Details: Original test-author description said "use after a feature lands its first verification round" — too reactive. No project rule made the orchestrator dispatch test-author automatically when an agent reported a manual step; no per-plan rule made architect classify §Verification items into automation buckets up front; no code-review gate blocked merge on manual-residue language. Optimized agent + AGENTS.md § Verification automation makes the cadence project-wide: plan-time + first-round + every-agent-handoff. architect.md mandates bucket-A/B/C/D/E classification of every §Verification item; code-review.md flags manual residue as Critical.
  Status: applied (a18f985)

- 2026-05-13 · test-author · [tooling] — no unified test runner; each scripts/dev/test-*.sh ran in isolation
  Details: Adding a new feature test required the dev to remember its script name; CI / pre-merge could only run one script at a time. Added scripts/dev/test-all.sh that globs scripts/dev/test-*.sh (excluding self), runs each, aggregates Passed/Failed. New tests auto-enrol by naming convention. Forwards SMATCHET_EXE / SMATCHET_TEST_PORT / PYTHON envs to children. Optional --filter for targeted runs.
  Status: applied (a18f985)

- 2026-05-13 · test-author · [new-agent / tooling] — bucket E (ImGui Test Engine) not wired; bucket-E items currently flagged as manual residue with a deferred-automation note
  Details: Smatchet has no ImGui Test Engine integration today. The first bucket-E item (e.g. "drag column header to position X" verification step) will require wiring imgui_test_engine via FetchContent, a new SmatchetUiTest target gated by SMATCHET_BUILD_UI_TESTS=ON, tests/ui/ directory, and a ui_test.run CLI command. Recipe is encoded in agents/test-author.md § Bucket E; first invocation should land the harness, not just defer.
  Status: applied (2026-05-15) · execution plan at `docs/design/imgui-test-engine-bucket-e-execution.md`. FetchContent + `cmake/ImGuiTestEngine.cmake` + `Source_Core/include/SmatchetImConfig.h` + `UiTestScenario` + `ui_test.run` CLI + `tests/ui/` enrolment + `scripts/dev/test-ui-views-columns-reorder.sh` driver all shipped against the Views → Columns reorder bug. Diverges from original 5-commit migration: in-process Standalone instead of separate exe (Q6 user choice), DX12 stub variant (Q7), `IMGUI_USER_CONFIG` for ImConfig consolidation. Phase 2 (diagnose bug) + Phase 3 (fix bug) deferred — first run reports `tested=2 failed=2`, so the engine drives the call site but either the test path expression or the bug itself needs `debug-detective` follow-up before a fix lands. Hand-off recorded in plan § Deviations.

- 2026-05-15 · orchestrator · [process] — sequential subagent dispatch loses wall-clock when delegations are contract-independent
  Details: Borrowed from Anthropic multi-agent research paper. Orchestrator now parallelises independent delegations in a single tool-use block per AGENTS.md § Parallel dispatch (examples: code-review + security-review for pre-merge gate; perf-detective + debug-detective for "slow AND wrong output").
  Status: applied (d206de5)

- 2026-05-15 · orchestrator · [tooling] — no session scratchpad; subagent N rediscovered facts subagent N-1 already surfaced
  Details: Borrowed from Anthropic multi-agent research paper. New `.session-context.md` at repo root (gitignored); SessionStart hook truncates; SubagentStop hook (agent-token-log.py) appends a header block when the subagent's report carries `## Session context append`. Subagents never read or edit the file themselves — orchestrator reads + passes inline. Avoids race when subagents run in parallel and avoids conflicting with the vexp run_pipeline-FIRST rule.
  Status: applied (6df6170 + d206de5)

- 2026-05-15 · orchestrator · [tooling] — JSONL telemetry only tracked tokens; outcome / halt-reason / version / delegation-chain invisible
  Details: Borrowed from OpenAI Agents SDK + wshobson telemetry patterns. agent-token-log.py now emits outcome, halt_reason, agent_version, delegation_chain, tools_used, tool_trace per row. Inferred from transcript tail (priority: explicit `## Outcome:` line → halt keywords → `## Self-improvement` heading → default applied). scripts/agent-tokens-report.py surfaces outcome breakdown + top halt reasons + delegation depth + version-drift detection.
  Status: applied (6df6170)

- 2026-05-15 · orchestrator · [context] — get_skeleton under-used; agents read full files for context-only inspection
  Details: AGENTS.md § Skeleton-first now codifies as hard rule: get_skeleton for inspection, Read only when editing. Cuts ~70-90% input tokens on the heaviest readers (architect, code-review, security-review, debug-detective).
  Status: applied (d206de5)

- 2026-05-15 · orchestrator · [process] — agent prompts had no version field; couldn't correlate behaviour drift with prompt edits
  Details: Borrowed from OpenAI Agents SDK. Every agent now carries `version: <N>` integer in frontmatter; bump on capability / workflow / output-shape change (not on prose / banner / token-efficiency tweaks). Mirror banner emitted by scripts/sync-agents.sh now reads `@v<N>` so drift is visible at a glance. Telemetry (agent_version field) lets the report flag version-drift within a window.
  Status: applied (d206de5)

- 2026-05-15 · orchestrator · [process] — output-shape drift across agents; downstream agents couldn't reliably parse handoff sections
  Details: AGENTS.md § Agent output contract codifies four classes (Investigator / Implementer / Helper / Maintenance) each with required minimum sections plus a mandatory `## Outcome: <state>` line so telemetry inference is deterministic. Existing prompts already mostly conformed; this closes drift without forcing rewrites.
  Status: applied (d206de5)

- 2026-05-15 · orchestrator · [tooling] — trigger keywords lived in per-agent frontmatter but no central routing table
  Details: AGENTS.md § Trigger auto-activation now publishes the keyword → agent map. Orchestrator consults the table before falling back to the heuristic block. Per-agent triggers: list mirrors the row plus agent-specific synonyms.
  Status: applied (d206de5)
