# Smatchet — agent + project rules

This is the canonical entry-point doc for any agentic harness (Claude Code, Codex / OpenAI Agents, Cursor, Aider, generic). Everything an agent needs lives in this repo — no external dependencies on user-global or parent-dir config.

## Operating principles

How agents *operate* here — a skimmable map over the rules below, not new rules. Grep the principle, follow the link, skip the rest. (The Quality Pillars are quality *targets*; these 5 principles are the operating *model* — a distinct axis above them.) **Navigation only — no rule detail lives here; if a line accretes detail it has failed, move it to its linked section.**

1. **Autonomous by default** — run the ship-loop end-to-end in one turn; pause only on the defined exceptions. (§ Autonomous ship-loop default)
2. **Gate, don't trust** — every invariant is code-enforced (merge-gates, delta-lint, selftests), never a prose promise. (§ Merge gates, § Project rules § Tiered enforcement)
3. **Delegate to specialists** — the orchestrator routes to `agents/`; semantic-search before text-search. (§ Delegation, § Semantic codebase search — use it first)
4. **Plan before ship** — non-trivial work gets a plan-doc + `grill-with-docs` stress-test. (§ Process rules § Plan-doc family)
5. **Self-tighten** — every delegated agent ends with `## Self-improvement`; friction becomes prompt patches. (§ Self-improvement loop)

## Quality Pillars

Five north-star quality invariants in two sub-groups:

- **UX Pillars** (1-4) — user-facing. Pillars 1-3 are **enforceable** (auto-fail PRs that violate them); Pillar 4 is **aspirational** today (backlogged until automated checks land).
- **Engineering Pillars** (5) — code-maintainability, enforced like UX 1-3. Today: **DRY** (Pillar 5; WARN-first calibration phase per [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)).

| # | Group | Pillar | Hard invariant | Primary owner |
|---|---|---|---|---|
| 1 | UX | Performance | Steady-state UI work ≤ **6.94 ms** (144 Hz); p99 ≤ 16.67 ms (60 Hz floor) | `perf-detective` (sustained), `spike-hunter` (p99) |
| 2 | UX | UI never freezes | No UI-thread block > 100 ms without visible cue; sync I/O on UI thread = code-review CRITICAL | `code-review`, `spike-hunter` |
| 3 | UX | Never crash | Sanitizer build clean; RAII + bounds-checked + no silent UB; graceful degradation in ship builds | `debug-detective`, `code-review`, `build-doctor` |
| 4 | UX | Accessibility | Keyboard nav, font scaling, WCAG AA contrast — flagged in backlog (no auto-fail yet) | none today (backlogged) |
| 5 | Engineering | DRY | No NEW copy-paste clone vs `origin/develop` (delta-gated `dup_audit.py`; **WARN-first**, calibration phase per ADR-0015) — copy-paste only (not structural similarity); exemptions cheap; a DRY refactor coupling independent subsystems = CRITICAL | `code-review` (reviewer-of-record + exemption sign-off) |

Visual-validation exception (Pillar 4 § Visual-validation acceptance): when no bucket-C/E coverage exists for a visual change, the orchestrator pauses the ship-loop and treats the user as the verifier — see [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md) § Visual-validation exception.

Full enforceable-invariant text + visual-cue contract + per-pillar tooling + agent-ownership detail: [`docs/agent-rules/quality-pillars.md`](docs/agent-rules/quality-pillars.md).

## Autonomous ship-loop default

Orchestrator runs each user task end-to-end in **one turn** without pausing per stage. Default sequence:

```
diagnose → fix → build → commit → push → open PR → [gate-check] → squash-merge → git-janitor cleanup → backlog entry
```

Clarifications batched **once at start** via `AskUserQuestion`. After the user answers, the orchestrator **MUST NOT** use `AskUserQuestion` or pause for confirmation until either a defined exception fires or the post-ship 4-option menu. Each stage proceeds to the next automatically. CodeRabbit actionable findings are triaged and fixed autonomously; merge-gate polling starts immediately after PR creation; squash-merge fires immediately on `GATES_PASSED`. Loop pauses ONLY for: (1) debug-detective triggers — the pause-loop **overrides** the ship-loop (see [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md) § Debug-mode pause-loop), (2) destructive ops outside scope, (3) cross-repo / external-service mutations, (4) anything not durably authorised, (5) **visual-validation exception** (touches `SmatchetTheme.cpp` / `Smatchet*Ui*.cpp` / `Locales/*.json` / `ImVec4` constants AND no bucket-C/E coverage — pause after build with launched exe, await user verdict).

`SMATCHET_AGENT_VCS=p4` flips the loop to the **P4-gated** variant: smoke build → shelve → user review in P4V → full tests → submit → git branch + push + PR. Git is touched **once**, at the end, after shelf approval AND test-pass. Sub-variants chosen via `AskUserQuestion`: small-change loop (single slice, `//smatchet/main`) or task-stream loop (multi-slice, `agents/scripts/project/p4-task-stream.sh`).

**Session-start self-check (mandatory, regardless of user-prompt flavour)**. The SessionStart hook (`agents/scripts/core/clear-session-context.sh`) emits a `## === p4-mode ACTIVE ===` banner into `.session-context.md` when `$SMATCHET_AGENT_VCS=p4` AND `p4 info` succeeds. When the orchestrator sees that banner, it MUST follow the P4-gated ship-loop for ALL subsequent task-loops in this session — even when the user's prompt is git-flavoured (PR numbers, gh URLs, etc.). The env-var opt-in overrides prompt-driven mode inference. On `p4 info` failure the banner reads `## === p4-mode REQUESTED but UNREACHABLE ===` and the orchestrator routes through `AskUserQuestion` per `docs/agent-rules/ship-loops.md` § P4-gated ship-loop (never silently downgrade). `agents/scripts/project/p4-git-sync-check.sh` checks git-pending vs `p4 opened` alignment and is part of the P4-mode pre-ship verification.

After the loop completes, the orchestrator emits the **post-ship 4-option `AskUserQuestion`**: Manual verify / Review PR / Register with watcher (auto-merges when gates pass) / Done. Skip-condition: if the user already said "merge when green", enter option 3 directly.

**PR batching — one PR per logical feature, not one per slice/task.** Related slices that serve a single coherent goal accumulate on **one** feature branch and ship as a **single** PR; `open PR` is reached once per logical feature, not after each stage. This respects both CodeRabbit limits at once — the review **quota** (N tiny PRs burn N reviews for one conceptual change) and the per-PR **file ceiling** (above it CR posts *review-skipped — too many files* and the gate blocks). Cohesion is the seam: unrelated work in subsystem A vs B = two PRs; if one feature's cumulative diff would exceed the file ceiling, split along natural seams (don't batch past it expecting `cr-out-of-band` to cover routine batching). Earlier slices of a feature stop at `commit`/`push` to the shared branch and defer PR creation until the feature is whole. The do-not-pause checklist + merge gates + post-ship menu fire **per PR**, so one feature-PR may cover several session tasks.

Full sequence + per-exception detail + P4-gated phases + post-ship protocol + PR-batching boundary rules: [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md).

## Merge gates

Before any squash-merge by the orchestrator, `git-janitor`, or `smatchet-merge-watcher`, the gate-poller (`agents/scripts/core/merge-gates.sh` + `agents/scripts/core/merge-gates.graphql`) checks three conditions via one `gh api graphql` call:

1. **CI** — every required check on the head commit reaches a passing terminal state (CheckRun: `conclusion ∈ {SUCCESS, NEUTRAL, SKIPPED, STALE}`; StatusContext: `state == SUCCESS`).
2. **CodeRabbit** — `APPROVED` or `COMMENTED + Actionable comments posted: 0` passes; `COMMENTED + N > 0`, `CHANGES_REQUESTED`, `DISMISSED`, `STALE_WITH_FINDINGS`, `STALE_UNKNOWN` block. `NONE` falls through after `MERGE_GATES_CR_GRACE_POLLS` (default 10) expires when CR is installed — but the poller now also auto-posts `@coderabbitai review` once per HEAD on the first blocking `NONE` poll (early-nudge, gated by `MERGE_GATES_STALE_REREVIEW_POLLS`; 0 disables), so CR usually resolves before the grace backstop. **Review-skipped (too many files)** — when CR posts a PR conversation comment saying it skipped review because the PR exceeds its file limit (marker `skip review by coderabbit.ai`; `reviewDecision` stays `NONE`), the gate **blocks** (overriding the NONE grace fall-through, which would otherwise merge a huge unreviewed PR) and suppresses the futile auto-nudge; remediation is to split the PR or apply `cr-out-of-band`. A genuine on-head review always wins over a stale skip comment. The `cr-out-of-band` PR label downgrades any CR block (state verdict, unresolved CR-authored threads, **or** a review-skipped comment) to WARN — CR gate only; CI + user-comment gates still bind.
3. **User comments** — zero unresolved non-outdated review threads from non-bot non-self authors; zero conversation-tab comments from same.

Plus: PR is OPEN, `reviewDecision ∈ {APPROVED, null}`, no GraphQL `hasNextPage` overflow. **Auto-merge applies only when explicitly authorised** (post-ship "Register with watcher" or in-session "merge when green"). When authorised, the caller sets `MERGE_GATES_FLIP_READY=true` so the poller flips draft→ready at start, letting CodeRabbit's `auto_review.drafts:false` config not bypass review on draft PRs (closes the C4 draft-PR bypass per ADR 0006 amendment). Manual CR re-review trigger: `gh pr comment <pr> --body "@coderabbitai review"`. `SKIP_MERGE_GATES=true` at session init bypasses globally; per-PR label overrides (`tests-out-of-band`, `perf-out-of-band`) downgrade specific failing CI checks to WARN, and `cr-out-of-band` downgrades a CodeRabbit block to WARN.

Halt prompts on block / timeout / API-error / closed-externally / pagination overflow route through `AskUserQuestion` with explicit return-code-keyed options. The REST squash-merge contract (`gh api -X PUT repos/.../pulls/N/merge -f merge_method=squash`) is the merge mechanism; conflicts + branch-protection are enforced by GitHub, not duplicated locally.

**Never merge past ANY red check — required or not.** GitHub's "required check" set only governs what *blocks* a merge; a **non-required** check that is RED is still a real failure, never a fake one. Before any squash-merge — and **especially** a direct `gh api -X PUT … /merge` (which bypasses the gate-poller entirely) — every check on the head must be terminal-green (`SUCCESS`/`NEUTRAL`/`SKIPPED`/`STALE`), not just the four required ones. A red non-required job (e.g. **"Doc anchors + agent contract"**, which runs the whole `test-docs.sh` doc-validation suite — `test-portable-purity` / `test-plan-index` / `test-plan-ref-integrity`) means real breakage will land on `develop`. The **only** exceptions are (a) an explicit per-PR override label (`tests-out-of-band` / `perf-out-of-band` / `cr-out-of-band`) that *names* the downgraded check, or (b) a check you have *positively confirmed* is an irrelevant flake (named, with the reason). A red check is a stop; "non-required" is not a reason to ignore it. Direct admin-merge is for breaking a **stale-BLOCKED** state where every check is actually green — not for forcing past a genuinely red job. (Incident: this discipline was violated twice in one session — #780 admin-merged past a red CodeRabbit finding, #784 past a red doc-validation job — each shipping breakage that needed a follow-up heal. Captured in [`docs/self-improvement/postmortems.md`](docs/self-improvement/postmortems.md).)

**Never trust a CodeRabbit "✅ Addressed in commit X" annotation blindly** — CR matches commit-message keywords, not the diff. When CR marks one of its own findings addressed, read the cited commit's diff against the original finding and confirm the change actually matches the requested fix before treating it resolved (observed: a "✅ Addressed" banner-contrast fix that the cited commit never made). The green annotation is a hint, not proof.

Full per-outcome semantics + halt-prompt return-code table + env-knob list + REST contract: [`docs/agent-rules/merge-gates.md`](docs/agent-rules/merge-gates.md). Tests: `tests/bats/merge_gates.bats`.

## Project rules

**Doc & agentic structure**: the normative taxonomy + the portable/project boundary live in [`docs/STRUCTURE.md`](docs/STRUCTURE.md); project-specific values (build presets, perf budgets, lint zones, env-prefix, …) live in [`project.config.json`](project.config.json). Portable dirs (`agents/core/`, `agents/_shared/`, `docs/agent-rules/`, `docs/harness/`) read values from the config — don't hardcode (guard: `test-portable-purity`).

**Build**: `cmake --build --preset ninja-iter-msvc` (iter), `ninja-debug-msvc` (debug), `ninja-publish-msvc` (publish). Clang equivalents: `ninja-iter-clang`, `ninja-debug-clang`. Exe at `build/<preset>/Smatchet.exe` (the CMake target is `SmatchetStandalone` but `OUTPUT_NAME` ships as `Smatchet`). **Light build for core-feature work** (default when the task is NOT an AI/Whisper/MCP feature — faster, fewer moving parts): add `-DSMATCHET_WITH_WHISPER=OFF -DSMATCHET_WITH_AI=OFF -DSMATCHET_WITH_MCP=OFF` to the configure step (the `_smatchet-light-features` preset fragment + `ninja-publish-light-msvc` encode the same OFF triple; CI's "Windows + MSVC (Smatchet light)" job uses exactly this flag set). Still verify the FULL config before merge when the diff could touch those subsystems — flip a full-configured `build/<preset>` by wiping it or reconfiguring with the OFF flags.

**Language**: C++14 hard (Unreal compat). Banned: `string_view`, `optional`, `variant`, structured bindings, `if constexpr`. Must compile on MSVC + Clang.

**Layout**: `Source/Core/{src,include}` is the shared core — used by both standalone and Unreal. `Source/Standalone/` builds the OpenGL exe. `Source/Plugins/{Mcp,LuaConsole}` are static plugins. `*_DX12` targets are `EXCLUDE_FROM_ALL` (Unreal only) — don't touch unless asked.

**Clearing the Unreal build — delete `lib/` only**: `Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet/` is **mixed** — `lib/Win64/{Debug,Development}/*.lib` is git-ignored (~912 MB, regenerated by the `SmatchetPackageUnrealLibs_DX12` target, safe to delete), but `include/*.h` (`imgui.h`, `imconfig.h`, `SmatchetImGuiHost.h`, `SmatchetImGuiHostC.h`, `SmatchetDefaults.h`) is **committed**. To clear the build, `rm` `.../ThirdParty/Smatchet/lib/` only — never the whole `Smatchet/` dir (removes 5 tracked headers). `git ls-files <dir>` is the only reliable tracked-vs-ignored check; scope it to the exact dir before any cleanup `rm`. Path is hardcoded in `SmatchetImGuiPlugin.Build.cs` + `CMakeLists.txt` (`SMATCHET_UNREAL_THIRDPARTY_DIR`).

**Available libs** (FetchContent, linked): nlohmann/json, cpr, SQLiteCpp, cpp-httplib, md4c, ImGui (docking), GLFW, Lua + sol2, ghc::filesystem.

**Logging**: `LOG_{DEBUG,INFO,WARN,ERROR,TRACE}` from `Logger.h` — never `printf` / `std::cerr`. Named exceptions (must have matching inline comment): `Source/Standalone/` pre-logger-init fatal paths (`// pre-logger-init — LOG_* unavailable`) and CLI stdout product output (`// CLI stdout — product output, not logging`).

**Exceptions**: catch-all policy in [`docs/agent-rules/exception-handling-policy.md`](docs/agent-rules/exception-handling-policy.md). Empty `catch (...) {}` blocks = review CRITICAL.

**nlohmann json**: `obj["k"] = v`, not `obj = {...}` (reassignment with brace-list won't compile).

**Optional plugins**: gate with `#if SMATCHET_WITH_LUA_AUTOMATION` / `#if SMATCHET_WITH_MCP`. Lua bindings split: `AppController_LuaBindings.cpp` (on) ↔ `AppController_LuaStubs.cpp` (off) — keep in sync.

**Don't**: add GLFW/OpenGL includes to `Source/Core/` headers (DX12 compiles them too); redefine `IMGUI_USE_WCHAR32` (PUBLIC on `ImGuiLib`).

**Dual-target**: `Source/Core/` compiles into both `SmatchetStandalone` (OpenGL+GLFW) and `SmatchetCore_DX12` (Unreal). Diverging macros: `SMATCHET_EMBEDDED_IN_UNREAL=1` (DX12 only); `SMATCHET_WITH_MCP=1` (Standalone only — `SMATCHET_WITH_MCP_UNREAL` is OFF). Full verify: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. In a worktree shell without `cl.exe` on PATH, wrap it: `powershell -ExecutionPolicy Bypass -File scripts/dev/with-msvc.ps1 cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (vswhere → vcvars64, toolset-pinned; PowerShell sibling of `scripts/dev/with-msvc-env.sh`).

**Quality**: RAII (no raw `new`/`delete` — use `std::unique_ptr` + `make_unique`; named exceptions must have inline comment: C ABI ownership boundaries `// C-ABI handle`, custom-deleter wraps `// custom-deleter — make_unique inapplicable`, third-party adapter seams); `const&` for non-trivial params; `std::move` on last use; small focused functions; `LOG_TRACE`/`LOG_DEBUG` in non-trivial branches. **`std::unique_ptr<T>` member in a class declared in a header — include `T`'s full definition in that header.** A forward-decl only works with an out-of-line dtor defined in a TU where `T` is complete; without it, `error: invalid application of sizeof to incomplete type` fires at *every* consumer's implicit-dtor instantiation site (not just the owning TU), so lift to the full include up-front instead of attempting forward-decl + pImpl.

**File size**: source files in `Source/Core/` and `Source/Plugins/` stay under **67 KB** (the GitHub diff-render limit; past it, review tooling truncates). When a file approaches the cap, split before adding more. Split recipe: create a `Foo_detail.h` for shared types / guards / constants; move shared free functions *out* of the anonymous namespace (gives them external linkage); keep only truly TU-private helpers anonymous. **Watch:** `thread_local` and `static` globals inside an anonymous namespace have internal linkage — each split TU gets its *own* copy (silent breakage); define them at file scope with an `extern` declaration in the `_detail.h` instead.

**ImGui draw functions** approaching **200 lines** use the section-helper pattern (`DrawCtx` + `DrawHeader`/`DrawBody`/`DrawFooter`/`DrawModals`/`HandleHotkeys`, perf scopes reused verbatim at section seams, `static` locals hoisted to a `<Foo>WindowState` member) — see [`docs/guides/imgui-draw-pattern.md`](docs/guides/imgui-draw-pattern.md); enforced by the `function-too-long` / `function-too-branchy` gate. Existing monoliths are **ride-along only** (decompose when a feature already opens the file; no dedicated sweep).

**Lint**: your harness may run an automatic lint pass after C++ edits. Claude Code does so via a `PostToolUse` hook wired by `bash agents/scripts/core/setup-harness.sh claude-code` — `clang-format -i` applies in place; `cppcheck` + `clang-tidy` report to stderr. If your harness lacks hook automation, run those three tools manually on every edited `.cpp` / `.h` in `Source/Core` / `Plugins` / `Source/Standalone` and fix all reported issues before responding.

**Shell lint**: shell scripts under `scripts/dev/` go through `agents/scripts/core/test-shell-lint.sh` (5 rules; checklist at [`docs/agent-rules/shell-script-self-review.md`](docs/agent-rules/shell-script-self-review.md)). Auto-runs via `scripts/dev/test-all.sh` at the pre-push gate. Bypass: `SMATCHET_SKIP_SHELL_LINT=1` (logged; emergency-only).

**Subagent eval (advisory)**: edits to `agents/*.md` agent prompts can be scored base-vs-head across quality dimensions instead of shipping on judgment alone — the perf-gate shape, one level up the stack. `scripts/dev/agent-eval-run.sh` (`--prompt-root` before/after seam, `--fake-runner` for tokenless CI) drives a curated case; `scripts/dev/agent-eval-score.py` diffs two result JSONs (objective inline checks + an **external** judge command) and exits `0` clean / `1` regression (WARN) / `2` malformed (FAIL). **Advisory only** — malformed artifacts FAIL, quality regressions WARN; the WARN→BLOCK graduation is deferred until judge-vs-human calibration data exists. Phase-1 MVP is `code-review` only. Full contract + two-worktree recipe + case-authoring: [`docs/agent-rules/subagent-eval.md`](docs/agent-rules/subagent-eval.md). Tests: `tests/bats/agent_eval_score.bats`, `tests/bats/agent_eval_run.bats`.

**Tiered enforcement** (high-integrity C++; plan [`docs/plans/shipped/high-integrity-cpp-enforcement.md`](docs/plans/shipped/high-integrity-cpp-enforcement.md)): `agents/scripts/project/test-lint-rules.sh` gates the **strict zone** on a delta basis — every NEW `(rule, file, snippet)` vs `origin/develop` fails CI; existing violators are grandfathered (snapshot: [`docs/high-integrity/baseline.md`](docs/high-integrity/baseline.md)). **Run this delta gate locally before every push** — `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (the exact gate CI runs, comment-bloat `comment_audit.py --diff` included), or the convenience wrapper **`bash scripts/dev/pre-ship.sh`** which `clang-format -i`s the changed first-party C++ **first**, **then** runs the delta gate — the correct order, since clang-format can reflow a comment into a flagged shape (`comment-blank-run` / `comment-commented-out-code`) *after* a hand-run gate passes (the #1 build-green-but-lint-fails footgun for decomposition work) — and **finally** runs the markdown style lint (`agents/scripts/core/md_lint.py --all`, e.g. `MD028` blockquote breaks), since docs are not covered by the C++ delta gate and a markdown issue otherwise surfaces only as a post-push CodeRabbit finding (also gated in CI by `doc-validation.yml`). A targeted `cmake --build`/`ctest` does **not** run it (only `test-all.sh` and CI do), so a slice verified by a bespoke build alone can still hit a push-time-only comment-bloat / strict-zone failure. Strict-zone delta-gated rules: `no-printf-stderr`, `define-imgui` (both have legitimate first-party uses outside the strict zone — Standalone CLI stdout, the ImGui localization-alias macro — so they stay strict-only). **First-party-wide absolute rules** (`no-raw-new`, `deviation-overdue`, `no-detach`) are enforced at **0 across ALL first-party C++** — `Source/Core`, `Source/Plugins`, `Source/Standalone` (the `comment_audit.py` SWEEP_ROOTS; ThirdParty + tests excluded), not just the strict zone: every raw `new` must use `make_unique` or carry an exemption marker, no `SMATCHET_DEVIATION` may sit past its `revisit=`, and no raw `std::thread(...).detach()` is allowed — fire-and-forget work routes through the joined `AppController::LaunchBackgroundTask` pool (joined at shutdown via `JoinBackgroundTasks`), which closes the use-after-free-on-shutdown window; anywhere. Absolute (no grandfathering); the tree is clean today, so any hit is a regression. Plus the repo-wide comment-regrowth rules `comment-commented-out-code`, `comment-decorative-banner`, `comment-blank-run` (classified by `comment_audit.py --diff`; hard-fail anywhere in first-party C++, not just the strict zone; escape with `SMATCHET_DEVIATION(rule=comment-...)`. A soft, advisory comment-ratio warning also fires — never blocking — when a touched file's comment ratio rises AND exceeds 0.50. reduce-source-comment-bloat Phase 4). The repo-wide **function-size** rules `function-too-long` and `function-too-branchy` are likewise delta-gated anywhere in first-party C++ (classified by `function_size_audit.py --diff`, keyed by `(rule, basename, qualified-name, arity)` so the existing monoliths are grandfathered; escape with `SMATCHET_DEVIATION(rule=function-too-long; ...)` above the signature — list multiple rules comma-separated to suppress both caps; snapshot [`docs/high-integrity/function-size-baseline.md`](docs/high-integrity/function-size-baseline.md); decompose-top-20-monoliths Slice 0). The line cap is **tiered** (maintainer decision 2026-06-01): `function-too-long` blocks at body **> 120 lines (non-UI)** or **> 200 lines (ImGui-draw)** — declarative UI is inherently noisier, so it gets the escape hatch; `function-too-branchy` blocks at **> 30 decision points** for all functions. A function counts as **ImGui-draw** (200-line cap) if EITHER its path is under a `Ui/` dir (`**/Ui/**`) OR its unqualified name starts with `Draw`/`Render`/`draw`/`render` (draw helpers live in non-Ui files too, e.g. `SmatchetDrawAiAssistantPanel`, `drawMainMenuBar`); everything else is non-UI (120-line cap). This rule lives in one place — `function_size_audit.py is_ui_function()` — and `--selftest` asserts it stays in sync with this section. A **soft, advisory warning** also fires — never blocking, never changes exit code — when a new/changed function exceeds **100 lines OR 20 branches** but stays under the hard caps (a `[func-size] WARN` line on stderr, mirroring the comment-ratio advisory), nudging new code toward the 40-80-line ideal. The repo-wide **`duplication`** rule (DRY — the **Engineering Quality Pillar 5**; [ADR-0015](docs/adr/0015-dry-quality-pillar-duplication-gate.md)) is delta-gated over the same first-party C++ SWEEP_ROOTS by `dup_audit.py --diff` — a token-normalized **copy-paste clone** detector (identifier + literal normalized, so copy-then-rename is caught; min ~70 tokens / ~8 lines) that grandfathers all existing duplication (snapshot [`docs/high-integrity/dup-baseline.md`](docs/high-integrity/dup-baseline.md)) and flags only NEW cross-file clones. It is **WARN-first (calibration phase)**: a `[dup] WARN` line on stderr, never blocking, until a measured false-positive rate **< 10% over ~20 PRs** graduates it to a hard block (plan [`docs/plans/active/dry-pillar-dup-gate.md`](docs/plans/active/dry-pillar-dup-gate.md) § Verification). It flags **copy-paste only**, never structural similarity; escape with `SMATCHET_DEVIATION(rule=duplication; ...)` on/above the clone — an exemption is cheap and **preferred over abstracting across unrelated subsystems** (a DRY-motivated refactor that couples two otherwise-independent subsystems is a code-review **CRITICAL**, not an improvement). Standing exemptions: dual-target forward-decls, per-backend `*Client` boilerplate, generated code. `narrowing-conversions` (clang-tidy) is **gated on a dedicated Windows job** (`high-integrity-narrowing`, develop post-merge): it configures a PCH-free clang-cl compile-DB (`ninja-iter-clang -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`) and fails on any **first-party** strict-zone narrowing — vendored `ThirdParty/` headers (e.g. `stb_image.h` pulled into a strict TU) are excluded. It stays excluded from the ubuntu `--diff` delta gate (clang-cl is Windows-only, and the base worktree has no clang DB). Reproduce locally with `SMATCHET_LINT_NARROWING=1` + a PCH-free clang DB. Zones (the scanner asserts this list matches its own copy via `--selftest`):

- **strict** (any violation fails): `Source/Core/src/Tracker/`, `Source/Core/src/Sync/`, `Source/Core/src/Persistence/`, `Source/Core/src/Config/`, `Source/Core/src/Commands/`, `Source/Plugins/Mcp/` (+ matching `Source/Core/include/` subdirs).
- **light** (not gated; existing inline exemptions apply): `Source/Core/src/Ui/`, `Source/Standalone/`.
- **exempt** (not scanned): `ThirdParty/`, `build/`, non-C++ trees.

**Subsystem guides** — heavy `Source/Core/src/<ctx>/` dirs carry their own leaf docs: an `AGENTS.md` (scoped rules — the subsystem invariants that used to live in `code-review.md`), and for the exemplar `Tracker/` also a `CONTEXT.md` (domain glossary) + `README.md` (orientation / request flow). When you touch a subsystem, read its leaf `AGENTS.md` and apply its invariants — they are the single source of truth for that dir and override any central summary. Leaves today: `Tracker/`, `Commands/`, `Persistence/`, `Sync/`, `Ui/` (the strict zones above + the light `Ui/`). The full registry + per-context coverage + harness-discovery index is [`CONTEXT-MAP.md`](CONTEXT-MAP.md); the coverage + README-staleness gate is `agents/scripts/project/test-subsystem-docs.sh`. Claude Code auto-loads each leaf via a gitignored `CLAUDE.md` shim that `setup-harness.sh claude-code` generates beside it (Claude Code reads nested `CLAUDE.md`, not nested `AGENTS.md`).

**`SMATCHET_DEVIATION` comment** — forward-only superset of the inline exemption markers, for new strict-zone deviations. Grammar (pure comment; zero compile cost; the next non-blank line is the target):

```cpp
// SMATCHET_DEVIATION(rule=<rule-id>; reason=<short>; owner=<handle-or-unowned>; revisit=<YYYY-MM-DD | YYYY-Qn | slug | never>)
int w = obj["w"].get<int64_t>();  // suppresses `narrowing-conversions` on this line
```

`rule` = scanner rule-id (suppresses that rule on the next line). `revisit` calendar markers that have passed emit `deviation-overdue` (a strict rule), forcing the audit loop; slug / `never` don't expire. Legacy markers (`// CLI stdout`, `// pre-logger-init`, `// C-ABI handle`, `// custom-deleter`) stay valid in perpetuity.

**Perf workflow**: when the user asks to optimize / profile / fix FPS / lag / hitch / "slow" / spike, read [`docs/guides/perf-workflow.md`](docs/guides/perf-workflow.md) and follow it. Don't load it for unrelated tasks.

**Golden-image approval contract**: any agent that writes or regenerates a checked-in reference artefact a regression gate diffs against (`tests/golden/*.png`, JSON snapshots, deterministic byte streams) MUST hand the file + launched-app handle to the user and wait for explicit approve-golden verdict before `git add`. Iterate the underlying fix on rejection; never amend the golden to match a buggy state. Full recipe + motivating incident + dual-capture-no-golden preference in [`docs/agent-rules/golden-image-approval.md`](docs/agent-rules/golden-image-approval.md).

## Process rules

Rules for **how agents move work through the pipeline** — plan-doc lifecycle, destructive-VCS-op discipline, cadence + verification, and the meta-rule for where future rules land. Companion files: [`docs/agent-rules/merge-gates.md`](docs/agent-rules/merge-gates.md), [`docs/agent-rules/ship-loops.md`](docs/agent-rules/ship-loops.md), [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md).

- **Plan-doc family** — every plan lives at `docs/plans/active/<slug>.md`, committed immediately with `wip(plan): <slug>` (working-tree-only files are silently lost on checkout). Post-ship § Implementation log + § Deviations + § Verification update. Stress-test via `grill-with-docs` skill before finalising. Start from `docs/plans/active/_plan-template.md`. **Perf-gate section mandatory** when diff touches `Source/Core/`. **Scope-reduction edits + final-check grep**: when a plan is slimmed by deferring a feature, grep `**/CONTEXT*.md` (covers `CONTEXT.md`, `CONTEXT-MAP.md`, per-context `src/<ctx>/CONTEXT.md`), `docs/adr/`, `agents/*.md`, and `docs/self-improvement/categories/` for **every symbol named in the now-deferred work** and revise or delete hits that describe deferred-as-current truth. Same grep after any substrate/shape/contract rewrite — hit the keyword family of the changed concept and clear every stray reference. **Plan-revision direct-pushes are PR-only**: revisions to `docs/plans/active/<slug>.md` § Implementation log / § Deviations / § Verification (actual) ship via a follow-up PR (same PR if still open, follow-up PR if already merged) — never direct-push to develop, even for one-line edits. Eliminates classifier-vs-rule drift.
- **Git/p4 discipline** — 5-step pre-flight before any destructive git op (`reset --hard`, `clean -f`, `branch -D`) against a worktree this session didn't personally check out: branch-show, status-short, stash modified, op, decide-stash-fate. Same defensive principle for destructive p4 verbs in p4-mode. Force-push banned globally except a narrow `--force-with-lease` carve-out on `claude/<id>/*` and `agent/<task-stream-id>/*` branches during API-500 recovery (ahead-range zero non-self commits; never bare `--force`). **Worktree-absolute path discipline**: when the session's working directory is under `.claude/worktrees/<id>/`, all `Edit` / `Write` absolute paths MUST use the worktree prefix — not the main-repo prefix. Main-repo-prefixed paths land changes on whatever branch main currently has checked out (often a sibling agent's branch), causing cross-branch contamination. Verify via `git rev-parse --show-toplevel` if uncertain.
- **Cadence + verification** — `cmake --build` and `scripts/dev/test-all.sh` run **at most once per slice**, after implementation is complete. Pure-docs slices skip both (`agents/scripts/core/is-pure-docs-diff.sh`). Trivial-visual envelope skips bucket-E + isolated worktree. Perf scenario auto-runs at slice boundary when the diff hits the curated map. **Stale-read recovery on `Edit`** = Re-Read → diff intended change → Re-Edit (never `replace_all` as force-write). Deferred lint drains once at end-of-turn; `clang-format -i` still runs inline.
- **Deferred plan-file rows** — optional/skipped § Files to modify rows require same-turn `## Deviations from plan` + backlog entry when follow-up work remains (`docs/agent-rules/process-rules.md`).
- **Memory drain** — the harness auto-memory inbox (`~/.claude/projects/<slug>/memory/`) is transient, not durable. A `SessionStart` nudge (`agents/scripts/core/memory-drain-nudge.sh`) fires when it holds ≥ 5 items or any item is > 7 days old; act via the `/drain-memory` skill (or "drain memory"). Triage every item: **implement** (→ `AGENTS.md` / `docs/agent-rules/*`), **backlog** (→ `docs/self-improvement/categories/*`), or **toss** — verifying each claim against the current tree first, then clearing drained rows. A cloud routine can't drain it (dir is machine-local). Spec: [`docs/agent-rules/memory-drain.md`](docs/agent-rules/memory-drain.md).
- **Where new rules go** — 1-liners stay in § Project rules above; rules that fit an extracted topic land in that file; > 30-line new topics get their own `docs/agent-rules/<topic>.md` + stub here; ≤ 30-line orphans go in `process-rules.md` (the catch-all) rather than fragmenting.

Full sub-rule text + canonical recipes + carve-out exclusion list + hot-files list + the deferred-lint pipeline: [`docs/agent-rules/process-rules.md`](docs/agent-rules/process-rules.md).

## Debug techniques

**Pink-clear UI gap detection**: for "is the background ever visible behind panels?" / "are dock gaps still leaking?" questions, set the clear color to magenta (`glClearColor(1.0f, 0.0f, 1.0f, 1.0f)` on Standalone, equivalent `ClearRenderTargetView` color on DX12). Any visible pink is a guaranteed dock gap or transparent region. Pair with a screenshot + per-pixel pink scan for objective regression tests.

**Exe staleness check**: after every rebuild, `ls -la` both the patched output and the most-likely-stale exe paths side-by-side, compare mtimes, and name the **exact** path the user should run. Multiple build outputs (`build/ninja-iter-msvc/`, `build/ninja-debug-msvc/`, `build/ninja-publish-msvc/`, worktree builds) make wrong-exe testing a common time-sink — orchestrator + perf / build agents all enforce this.

## Semantic codebase search — use it first

Every agent in this repo expects the orchestrator (or the agent itself) to use **semantic codebase search** before falling back to text-search. In practice that means:

- **Always** call the harness's indexed codebase search first for any "where is X" / "what calls Y" / "what does this touch" question. This is faster, cheaper, and more accurate than raw `grep` over a multi-MLOC codebase.
- Prefer **compact file-skeleton views** (signatures + classes only) for files you're inspecting but not editing — typically 70–90% token savings vs full reads.
- Fall back to text-search + full reads only when no semantic search is available or its index is degraded.

Under Claude Code this maps to `mcp__vexp__run_pipeline` (semantic search) and `mcp__vexp__get_skeleton` (skeleton). Other harnesses substitute their equivalents (see the Harness adapter table below). Agents whose prose mentions vexp do so as a concrete example — the capability is what matters.

## Agent file locations

Canonical, single source of truth: `agents/<name>.md` at the repo root (per the [agents.md spec](https://agents.md/)). Shared scripts + skills live at `agents/_shared/`. **Agent vs skill** — which form a new recurring procedure should take is decided by the rubric in [`docs/agent-rules/AGENT-VS-SKILL.md`](docs/agent-rules/AGENT-VS-SKILL.md) (skill when all-bounded-deterministic-inline; agent when any-exploration-loop-spawn-delegates; dual-publish to keep cross-harness discovery).

Per-harness adapter directories (`.claude/`, `.codex/`, `.cursor/`) are **gitignored** — they're regenerated locally from the canonical tree by `bash agents/scripts/core/setup-harness.sh <name>`. Adapters use directory junctions / symlinks where possible so edits to `agents/*.md` are picked up by the harness immediately, no sync step.

First-time setup or fresh clone? See [`docs/harness/SETUP.md`](docs/harness/SETUP.md).

## Delegation

**Moved to** [`docs/agent-rules/delegation.md`](docs/agent-rules/delegation.md) (~230 lines lifted out for navigability — AGENTS.md is now ~320 lines instead of 549).

Default: stay in the orchestrator's primary model for routine work. Delegate to an agent in `agents/` when the task matches.

Quick index of moved subsections — full content in `docs/agent-rules/delegation.md`:

- **Orchestrator delegation packet** — plan-lock pre-flight, shared inventory, invariant decisions, output budget, plan revision contract, subagent progress markers reminder, pure-helper TU-split recipe.
- **Parallel dispatch** — when to run multiple subagents in one tool-use block.
- **Context budget by task class** — ~80% utilization ceiling for large refactors → delegate before the last 20%; high tolerance for low-sensitivity edits.
- **Session scratchpad protocol** — `.session-context.md` lifecycle + `## Session context append` shape.
- **Subagent progress markers** — `.progress.log` via `bash agents/scripts/core/agent-progress.sh`.
- **Tool-trace contract** — hook-derived; agents don't track manually.
- **Agent output contract** — 5-class table (Investigator / Diagnostic read-edit / Implementer / Helper / Maintenance) + `## Outcome:` mandate.
- **Trigger auto-activation** — keyword → agent routing table.
- **Debug-mode pause-loop (overrides ship-loop)** — for `debug-detective` triggers.
- **API-500 mid-run recovery** — 5-step recovery for delegated agents that error API-500 after shipping file edits; `git add -A` gotcha; force-push carve-out for spawned-agent branches.
- **Skeleton-first** — `get_skeleton` for inspection, `Read` for editing.
- **Agent versioning** — when to bump `version: <N>`.
- **Cross-cutting** + **Subsystem specialists** — delegation tables.
- **Stay in the orchestrator for** — routine work list.
- **Heuristic** — when to delegate vs handle directly.
- **`delegates-to:` frontmatter** — direct call vs orchestrator-routed.
- **Why split** + **Complexity rationale** — design intent.

External references to `AGENTS.md § <subsection>` continue to resolve via this index — agents who read AGENTS.md land here, see the cross-link, and follow it to the canonical text. Don't maintain parallel copies; edit the canonical at `docs/agent-rules/delegation.md`.

## Self-improvement loop

Every delegated agent ends its report with a `## Self-improvement` section. **Empty is the common case and explicitly fine** — agents only flag real friction, never make up suggestions.

Operational rules — format, categories (`bug` / `process` / `tooling` / `infra` / `test` / `security`), priority enum (P0–P3), workflow steps, apply threshold, triage cadence — live alongside the index at [`docs/self-improvement/AGENT_SELF_IMPROVEMENT.md`](docs/self-improvement/AGENT_SELF_IMPROVEMENT.md). Live entries split per category under [`docs/self-improvement/categories/`](docs/self-improvement/categories/). Applied entries archive immediately to `self-improvement/categories/applied.md`. The goal is a self-tightening loop — agents notice friction, the orchestrator accumulates evidence, prompts get patched, friction drops.

**Gate escapes owe a postmortem.** A gate escape — something that shipped to `develop` that a gate should have caught (a non-SUCCESS check at merge, an override label, a `Revert`, an overdue deviation) — is the highest-signal lesson for a "gate, don't trust" harness. `agents/scripts/core/postmortem-owed.sh` detects them and raises a SessionStart nudge; the [`gate-escape-postmortem`](agents/_shared/skills/gate-escape-postmortem/SKILL.md) skill runs a blameless RCA whose **mandatory** `### Preventing gate` field names a new gate (filed into the categories above via the normal threshold) and appends to the [`docs/self-improvement/postmortems.md`](docs/self-improvement/postmortems.md) ledger. The postmortem is the incident *finder*; this loop is the *applier*.

## Dual-VCS topology (Perforce as opt-in local layer)

Smatchet runs git/GitHub as the **ship-line** (PR review, CI, `smatchet-merge-watcher`) and Perforce as an **opt-in local layer** (`SMATCHET_AGENT_VCS=p4`; default `git`) for agentic-WIP primitives — named server-side shelves, atomic counters as plan-locks, exclusive `+l` file locks, task streams as parallel-isolation primitives. The Perforce layer is purely additive: never required, never authoritative, never on the ship-line.

Concern-by-concern git ↔ p4 mapping + verb-level TL;DR + lock discipline + shelf-vs-stash + destructive-p4-op pre-flight: [`docs/perforce/AGENT_FLOWS.md`](docs/perforce/AGENT_FLOWS.md). Bring-up: [`docs/perforce/SETUP.md`](docs/perforce/SETUP.md). Janitor: [`agents/core/p4-janitor.md`](agents/core/p4-janitor.md). Plan: [`docs/plans/shipped/git-to-perforce-migration.md`](docs/plans/shipped/git-to-perforce-migration.md).

## Harness adapter

Each agent declares a closed set of **capability tags**. The orchestrator (and the harness) maps tags to concrete tools. Currently known mappings:

| Capability tag | Claude Code | Codex / OpenAI Agents | Cursor | Aider | Generic CLI |
|---|---|---|---|---|---|
| `semantic-code-search` | `mcp__vexp__run_pipeline` | vexp.run_pipeline (MCP) | (built-in search panel) | (not built-in — fall back to text-search) | `rg` over symbol set |
| `file-skeleton` | `mcp__vexp__get_skeleton` | vexp.get_skeleton (MCP) | — | — | `ctags -x <file>` |
| `file-read` | `Read` | `read_file` | (built-in) | (built-in) | `cat` |
| `file-edit` | `Edit` | `apply_patch` | (built-in) | (built-in) | `sed` / patch |
| `text-search` | `Grep` | `rg` (shell) | (built-in) | (built-in) | `grep` / `rg` |
| `file-glob` | `Glob` | shell `find` | — | — | `find` |
| `shell` | `Bash` | `shell` | terminal | shell | sh |
| `web-fetch` | `WebFetch` | `web.fetch` | — | — | `curl` |
| `git-history` | `Bash(git log)` | `shell(git log)` | (built-in) | (built-in) | `git log` |

**Harness notes:**

- **Claude Code** discovers agents at `.claude/agents/` — a junction into the canonical `agents/` tree created by `bash agents/scripts/core/setup-harness.sh claude-code`. Edits to `agents/*.md` are visible immediately; no sync step.
- **Codex / OpenAI Agents** reads `AGENTS.md` per the [agents.md spec](https://agents.md/). Discover individual agents at `agents/*.md`.
- **Cursor / Aider / generic** — human-driven. Agent files at `agents/` are reference docs the user pastes or invokes manually.
- Harnesses without `semantic-code-search` should fall back to text-search with the symbol set named in each agent's prose. Output is degraded but workable — expect more round-trips and larger context per query.

**Per-agent harness hints**: each agent's YAML frontmatter may carry a `harness-hints.<harness>:` block with harness-specific routing details (e.g. Anthropic model selection, MCP tool list). Harnesses ignore unknown blocks.

## Recommended companion — caveman

Output-token compressor (~75% cut, technical content preserved byte-for-byte). Install + use instructions: [`docs/guides/caveman.md`](docs/guides/caveman.md). Default: `/caveman full` at session start.

## Semantic-search exceptions

- **Exhaustive literal / symbol inventories**: use text-search (`rg` / harness equivalent), not semantic search. Graph-ranked results are not exhaustive. Run the search once in the orchestrator and pass `<file>:<line>:<role>` matches inline to delegated agents.
- **Mechanical renames and cleanup checks**: same — every occurrence must be found. `mechanic` and `perf-instrument` already use text-search per their prompts.
- **Understanding impact / ownership / surrounding logic**: semantic search stays primary. This is the default path.

## vexp — Claude-Code-only

The vexp MCP-tool guidance block (`run_pipeline`, `get_skeleton`, etc.) is Claude-Code-specific and lives in `.claude/CLAUDE.md` (regenerated by the vexp tool; sourced from `docs/harness/claude-code/CLAUDE.md.tmpl`). It is deliberately **not** mirrored here so Codex / Cursor / Aider — which read `AGENTS.md` per the [agents.md spec](https://agents.md/) — don't carry Claude-Code-only MCP guidance they cannot use. Those harnesses fall back to text-search per § Harness adapter.

The vexp tool currently auto-regenerates its block into `AGENTS.md` on every install / update, which is wrong per the above rationale. `agents/scripts/core/vexp-strip-agents-md.sh` (wired as a SessionStart hook in `.claude/settings.json`) idempotently strips the block on every Claude Code session start until the upstream tool is patched to target `.claude/CLAUDE.md` only. Tracked: `docs/plans/shipped/unblock-external-blockers-2-3-4.md` § Slice 2.
