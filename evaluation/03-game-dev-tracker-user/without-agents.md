# Smatchet — Game-Studio Tracker/Tool Evaluation (without agents.md)

*Evaluator persona: gameplay/engine programmer at a small-to-mid Unreal + Perforce studio, assessing Smatchet as a daily tracker / productivity tool. Evaluated purely as a prospective user and buyer.*

---

## 1. Executive Summary & Verdict

Smatchet is a C++14 / Dear ImGui desktop application that fuses three things a game team usually keeps in separate tabs: an issue-tracker client (Jira, Plane.so, GitHub Issues, and now Linear), a Perforce *crash-to-blame* annotate tool, and an optional embed of the whole UI **inside the Unreal Engine editor** via a DX12 ImGui plugin. The codebase is real and substantial — the Tracker subsystem alone is ~10k lines across four backends, the Perforce annotate UI is ~2,500 lines plus a threaded `p4` worker, and the Unreal plugin ships a working async command bridge to Blueprint/C++/console. This is not a tech demo; it is a genuine product mid-flight.

The standout, and the only feature here I can't get from Jira-web + P4V + Linear today, is the **Perforce annotate workflow keyed off a crash callstack**: paste (or auto-pull from a ticket field) a stack trace, and Smatchet runs `p4 annotate` on each frame's file+line to tell you which changelist and author last touched that exact line. That is a real game-dev triage accelerator. The Unreal embed is the second differentiator — running your tracker as an overlay inside the editor (`Ctrl+Shift+J`) is legitimately novel.

**Would my studio adopt this?** Not yet as a Jira/Linear replacement, but **yes as a targeted Perforce crash-triage + Unreal-overlay companion** for the engine team, *if* I'm willing to absorb the build cost. The tracker side is competent but would not pull my team off the Jira/Linear web UI for day-to-day work (no notifications, no boards/sprints, no rich comment/attachment authoring parity). The blockers to adoption are operational, not architectural: **no prebuilt binaries in-repo, a Windows-only / MSVC-or-clang-cl / CMake+Ninja build, a beta Unreal plugin, and a performance budget that is aspirational rather than enforced today.** Net: a promising "engineer's tool" worth a pilot on the engine team, not a studio-wide tracker rollout.

**Overall: 6 / 10 "would I deploy it."**

---

## 2. Scope & Method

I evaluated this as a buyer would: I read `README.md`, `BUILD.md`, `CLI_GUIDE.md`, the keyboard-shortcuts guide, the perf workflow guide, the Perforce setup runbook (`docs/perforce/SETUP.md`), the Unreal plugin manual (`Source/UnrealPlugins/SmatchetImGuiPlugin/README.md` + its `.uplugin`), and inspected the actual feature code under `Source/Core/src/` — Tracker backends, the Perforce annotate worker/UI (`Source/Core/src/Ui/AnnotateAnalysisUi*`, `Source/Core/src/P4Annotate*`), the Views/grid UI, Sync/offline (`Source/Core/src/Sync/`), and the Android shell.

**Deliberately ignored** (per the "without agents.md" constraint, and because they're irrelevant to a buyer): `AGENTS.md`, the `agents/` directory, `AI_POLICY.md`, `docs/agent-rules/**`, `docs/harness/**`, `docs/self-improvement/**`, `docs/agent-eval/**`, `.coderabbit.yaml`, `.cursor/`. I did not factor the agentic-governance meta-layer into any score; I judged only the shipping tool. I did note where the agentic machinery *leaks* into user-facing docs (e.g. perf baselines tied to a "ship-loop"), but only insofar as it affects what a user actually gets.

I could not build or run the binary in this environment (Windows toolchain), so performance claims are assessed from architecture and the project's own perf docs, not from measured frames.

---

## 3. Workflow Fit

### 3.1 Issue tracking — competent client, not a Jira/Linear replacement

The backend architecture is clean and real. There's a backend-agnostic `ITrackerBackend` interface with four concrete implementations behind `DefaultTrackerBackendFactory.cpp`:

- **Jira** — the most mature backend by far. It's not one file; it's ~3,000 lines spread across `JiraClient.cpp`, `JiraIssueSearch.cpp`, `JiraIssueMutation.cpp`, `JiraIssueMappingPure.cpp`, `JiraEditMetaPure.cpp`, `JiraUserAndMeta.cpp`, `JiraChangelogDeltaPure.cpp`, `JiraActivityFeed.cpp`, plus JQL helpers (`JqlEscape`, `JqlProjectScope`, `JqlSuggestEngine`). Editmeta-driven field editing, changelog deltas, activity feeds, and a JQL suggest engine are all present — this is a serious Jira client.
- **GitHub Issues** — also substantial (`GitHubClient.cpp` ~673 lines + `GitHubIssueSearch.cpp` ~631 + a JQL→GitHub-query translator `GitHubQueryFromJql.cpp`). Real, not a stub.
- **Plane.so** and **Linear** — present and structured the same way (`PlaneClient`, `LinearClient` ~556 lines with its own GraphQL-ish mutation/search/mapping split). Linear is explicitly described in the factory as "Slice 1… Linear as fourth tracker," so it's the newest and likely least battle-tested.

**What works for daily use:** search/create/edit issues; custom fields with inline editing and per-backend "new issue inherit" config; a **Views system** (`Views.cpp`, `SmatchetViewsDashboardUi.cpp` ~1,177 lines) that is genuinely well-thought-out — saved queries, per-tracker column layouts, sort orders, column widths, drag-and-drop reorder, `Alt+↑/↓` keyboard reorder, and an explicit *"Unsaved layout changes"* commit gate (`Save` / `Save as new` / `Discard`) instead of silent autosave. That commit-gate detail is the kind of thing only people who actually use trackers all day design. Bulk import/export exists as a real panel (`SmatchetBulkTicketsUi.cpp` ~738 lines). Offline drafting is real: `IssueDraft.cpp` (~410 lines) plus an offline queue (`OfflineQueueService.cpp`) with replay, dead-letter pruning, and a `BackendAuditTrail`. You can `ticket.create --offline=true` and replay later — genuinely useful on a flaky studio VPN or a plane.

**Where it falls short of replacing Jira-web / Linear for my team:**

- **No boards, no sprints, no swimlanes.** Game producers live in Jira/Linear board views. Smatchet is a grid-and-views client, not an agile board. For an engineer triaging their own queue it's fine; for a team's planning surface it isn't.
- **No notifications/watch feed that pulls you in.** There's a Notification Center for in-app toasts, but nothing replaces the "someone @-mentioned you" inbox that keeps people in Jira/Linear.
- **Comment/attachment authoring is thin.** `ticket.add_comment` posts *plain text* only (CLI guide says so explicitly); attachments can be previewed/opened but rich authoring is limited. Linear/Jira-web Markdown comment composer is richer.
- **Multi-backend is switch-one-at-a-time**, not unified. Views and field catalogs are stored per-backend and you switch in Preferences (some keys need a restart — `trackerType` is "restart required"). A studio that runs Jira for production and GitHub Issues for an open-source tool can't see both at once.

Verdict: a fast, keyboard-driven **read/triage/quick-edit client** that I'd happily use as a *secondary* surface, especially paired with the annotate feature. It will not retire the Jira/Linear web UI for planning.

### 3.2 Perforce — the killer feature for game devs

This is where Smatchet earns its place on an engine programmer's machine. The "annotate" feature (`Source/Core/src/Ui/AnnotateAnalysisUi*`, backed by `Source/Core/src/P4Annotate.cpp` / `P4AnnotateParse.cpp`) is **not** a generic `p4 annotate` viewer — it's a **crash-callstack-to-blame pipeline**:

1. Paste a crash callstack (or auto-pull it from a configured tracker field — `CallstackTrackerFieldId`; it even auto-detects a field literally named "callstack"). `OpenAnnotateAnalysisForGridIssue()` wires this straight from a grid row, so you can go ticket → annotate in one click.
2. It parses the stack into frames (`ParseCallstackText`), applies workspace **path-remap rules** (depot↔workspace mapping — essential since crash dumps carry build-machine paths), and filters noise frames via an ignore-keyword list.
3. A background worker thread (`WorkerThreadMain` in `AnnotateAnalysisUi_Worker.cpp`) runs `P4AnnotateLine(cfg, path, line, atChangelist)` per frame, resolving *which changelist and author last touched that exact line*, with a bounded `P4ChangelistDescribeCache` (default 512 entries) to avoid re-describing the same CL. There's an `@changelist` field so you can annotate *as of* a specific build's CL, not just head — exactly right for "this shipped in build X."
4. Results render with C++ syntax highlighting (`CppSyntaxHighlight.cpp`), per-line detail expansion (full-file annotate loaded lazily via `std::async`), and launch-outs to P4V timelapse / `p4vc` change views (`P4vLaunch.cpp`, configurable command templates).

For a studio drowning in crash reports, "who broke this line and in which CL" is the single most common triage question, and Smatchet automates it across a whole callstack. P4V's annotate is per-file and manual; nothing in native Perforce tooling does the callstack-wide blame sweep. **This is the feature I'd install Smatchet for.**

Caveats: it shells out to a real `p4.exe` (configurable path) and inherits `P4PORT`/`P4USER` environment, so it needs a working Perforce client — which every game dev already has. The VCS submission/commit helpers (`Source/Core/src/Vcs/VcsSubmission.cpp`, plus GitHub-commit parsers) are thin (~240 lines total) and clearly secondary; the annotate path is the deep one. The `docs/perforce/SETUP.md` runbook is about standing up a *p4d server for Smatchet's own dev*, not about using the annotate feature — a buyer can ignore it.

### 3.3 Unreal integration — real, beta, and the second differentiator

The Unreal plugin (`Source/UnrealPlugins/SmatchetImGuiPlugin`) is a genuine UE plugin, not a wrapper sketch. The `.uplugin` is `VersionName 0.6.7`, `IsBetaVersion: true`, `EnabledByDefault: false`, Category UI, Runtime module. It embeds the *same* ImGui UI inside the editor via a platform render backend (`SmatchetImGuiRenderBackend_WinDx12.cpp`; the description even names PS5/Xbox backends as targets), a Slate input processor (`SmatchetImGuiInputProcessor.cpp`), and a view extension hooking the render thread.

Crucially, it exposes the **unified command system** three ways inside Unreal:
- **Human overlay**: `Ctrl+Shift+J` toggles the Smatchet UI over the viewport — your tracker and annotate tool live *in the editor*.
- **Blueprint/C++ bridge**: `USmatchetImGuiCommandBridge` with `EnqueueSmatchetCommandWithCallback`, returning JSON envelopes asynchronously. Gameplay or editor-utility Blueprints can query/drive Smatchet.
- **Console**: `smartchat.<command>` (with a `smatchet.` alias) in the UE console, results to the Output Log.

The same 56+ command catalog that feeds the CLI and palette feeds Unreal — so `view.activate`, `ticket.create`, `tickets.search_active` all work from Blueprint. That's architecturally elegant.

**How real/usable for me?** The mechanism is real and the docs are honest about its rough edges (the README's whole "Host Availability"/"Troubleshooting" section: the native host inits lazily on first overlay show, `RequestId == 0` if libs are missing, must repackage `ThirdParty/Smatchet/lib/Win64/...` when stale). It's a **light profile** in Unreal: Lua + commands on, but MCP/AI/Whisper off. Integration friction is non-trivial — you package the DX12 core libs via `SmatchetPackageUnrealLibs_DX12` / `package_unreal_plugin_msvc.ps1`, drop them in `ThirdParty/Smatchet`, enable a beta plugin, and rebuild. For an engine team that builds the editor from source already, that's a normal Tuesday; for a content team it's too much. The beta flag and lazy-init footguns mean I'd pilot it with one engineer before pushing to the team.

### 3.4 Performance — architecturally credible, but the budget is *aspirational, not enforced*

The architecture supports the speed claims: ImGui immediate-mode UI, a SQLite `LocalCacheManager` caching field catalogs/user metadata/recent issues for near-instant cold load and offline reads, all HTTP and `p4` work pushed onto worker threads (the annotate worker, `std::async` detail loaders, off-thread config writes). There's a real perf-instrumentation system: `SMATCHET_UI_PERF_SCOPE` markers, a `perf.snapshot`/`scenario.run` CLI path, a `priority-grid-scroll` scenario, and even a real-FPS measurement env hook (`SMATCHET_FPS_MEASURE_SECONDS`) in the standalone loop. That's more perf discipline than most internal tools have.

**But the headline "144 Hz / 6.94 ms steady-state" number is a design pillar, not a measured guarantee.** The project's own active plan docs are blunt about it: the perf gate is *"a guaranteed-pass no-op today (zero `ci-windows-latest` baselines… 6.94 ms is not encoded in `perf-compare.py`/`regression-policy.json`)"* and is "PARKED" pending human baseline approval. The perf-workflow guide also notes the scenario path measures **UI-thread CPU under headless software GL (Mesa)** — "it does NOT capture real framerate (no GPU, no vsync, no present)." So as a buyer I read 144 Hz as "the team cares about responsiveness and built tooling to chase it," not "verified 6.94 ms frames on my data set." The SQLite-cache "near-instant load" claim is the more credible and more relevant one for daily use — that I believe.

For an ImGui tracker the practical reality is almost certainly "feels instant," which is all I need. I just wouldn't quote the 144 Hz figure to my team as a guarantee.

---

## 4. Adoption Friction & Risks

- **No prebuilt binaries / installer in the repo.** There are release/signing/installer *scripts* (`scripts/publish/release_github.ps1`, `SIGNING.md`, installer NSIS/smoke-test tooling) — so a signed installer and portable ZIP are *intended* to exist via GitHub Releases — but a buyer cloning this repo gets source, not a `.exe`. First contact is a build, not a double-click. That alone gates non-engineer adoption.
- **Build is Windows-centric and toolchain-heavy.** CMake ≥3.24 + Ninja + **MSVC (VS 2022) or clang-cl**. MSYS2 is explicitly retired/unsupported. First configure pulls and builds a dozen deps via FetchContent (~5 min). It's well-documented (`BUILD.md`, a `doctor.sh` preflight, `build_and_run.ps1` that auto-bootstraps vcvars) and "zero manual dependency downloads" is a real nicety — but it's still a from-source C++ build. My engine team can do this in their sleep; nobody else on the studio will.
- **Learning curve.** The UI is dense (docking panes, views editor, command palette, annotate config with path-remap rules). Powerful for an engineer, intimidating for an artist or producer. Fully **rebindable keyboard shortcuts** (every shortcut maps to a command id, editable in Preferences, conflicts warned-not-blocked) is a strong point for power users.
- **Localization** is en-US / fr-FR only, with JSON override files for custom wording — fine, covers a bilingual studio, but it only localizes app-owned UI chrome, not tracker data.
- **Maturity/version signals are early.** Unreal plugin is `0.6.x` and flagged beta; Linear backend is "Slice 1"; the perf gate is parked. This is pre-1.0 software. Issue references in code comments (`Issue #1459`, etc.) show active churn.
- **Single-author / internal-project smell.** The Perforce setup runbook is written around one developer's box ("Brick"), one user (`alexk` / the evaluator's own email), one depot. That's fine — it tells me this is a focused tool built by someone who actually does this work — but it means I'm an early adopter depending on a small team, with the support risk that implies.
- **The CLI requires a *running app instance*.** The CLI is not standalone; it attaches to a running Smatchet over its MCP HTTP endpoint (`127.0.0.1:42360`), or `--spawn`s a hidden one. Good for scripting against your open session, but it's not a headless `p4`-style binary.

---

## 5. Standout Features vs Irrelevant Extras

**Genuinely useful for my studio:**
- **Callstack-driven Perforce annotate** — the reason to install it. Nothing in P4V matches it.
- **Unreal in-editor overlay + command bridge** — tracker and annotate inside UE, scriptable from Blueprint. Novel and on-pipeline.
- **Views system with explicit commit-gate** — the best-designed part of the tracker UI; faster than Jira-web for a power user's saved queries/columns.
- **SQLite cache + offline drafting/replay** — real value on flaky studio networks and laptops.
- **Fully rebindable shortcuts + command palette (`Ctrl+Shift+P`)** — keyboard-first, fast.

**Impressive but mostly irrelevant to me as a game dev:**
- **Lua automation (sol2, in-app console)** — nice for a tinkerer; my team won't write Lua to manage tickets. Noise for most users, a toy for one or two.
- **MCP server + AI assistant side panel (OpenAI/Anthropic/Ollama/DeepSeek) + push-to-talk Whisper dictation** — this is the "AI productivity" surface. For a studio standardizing on Jira/P4, it's optional garnish, not a buying reason; the AI/Whisper/MCP plugins are all CMake-gated and *off* in the Unreal light profile, which is the right default. I'd ship without them.
- **Mobile companion (`Source/Mobile/AndroidApp`, `SmatchetMobileShellUi.cpp`)** — a real Android shell exists, but it's early ("Phase-1 adds the replay/sync drive… no mobile perf scenario exists yet"). A phone tracker client is not something a game studio needs; I'd ignore it entirely.

The unified command system (one `RegisterCommand` surfaces in CLI + palette + MCP + Lua + Unreal) is the architectural spine that makes all of the above cheap to expose. As a buyer I don't care about the elegance, but it does mean the tool is consistent across surfaces, which I do care about.

---

## 6. Scorecard

| Dimension | Score | Rationale |
|---|---:|---|
| **Tracker functionality** | 7/10 | Four real backends, strong Views system, offline drafting, bulk import/export, inline field edit. Loses points for no boards/sprints, plain-text comments, one-backend-at-a-time, weak notifications. A great *triage client*, not a planning replacement. |
| **Perforce / game-pipeline fit** | 9/10 | Callstack→`p4 annotate`→blame-CL across a whole stack, with path-remaps, @CL pinning, describe-cache, P4V launch-outs, syntax highlighting. Best-in-class for game crash triage; nothing native does this. |
| **Unreal integration** | 7/10 | Real DX12 ImGui plugin embedding the full tool in-editor + Blueprint/C++/console command bridge over the unified catalog. Docked a point or two for beta status, packaging friction, and lazy-init footguns. |
| **Performance / responsiveness** | 6/10 | Architecture (ImGui + SQLite cache + worker threads) is credibly fast and cold-load is near-instant. The 144 Hz/6.94 ms budget is an *aspiration*; the perf gate is self-described as a "guaranteed-pass no-op today." Believe "feels instant," not the headline number. |
| **Ease of adoption** | 4/10 | No in-repo binary; Windows + MSVC/clang-cl + CMake/Ninja from-source build; dense UI; beta Unreal plugin. Trivial for an engine team, a wall for everyone else. |
| **Maturity / trustworthiness** | 5/10 | Substantial, actively developed, honest docs — but pre-1.0, single-author/internal-project signals, parked perf gate, "Slice 1" backends. Early-adopter risk. |
| **Overall — would I deploy it** | **6/10** | A compelling *engineer's companion* for Perforce crash triage and an Unreal-embedded tracker; not a studio-wide Jira/Linear replacement. Pilot on the engine team. |

---

## 7. What Would Make Me Adopt vs Walk Away

**I'd adopt (pilot) today if:**
- My pain is **crash triage on a Perforce depot**. The callstack annotate alone justifies installing it on the engine team's machines. I'd pair it with our existing Jira/Linear, not replace them.
- I have engineers comfortable building from source and an Unreal project built from source — the in-editor overlay is a real workflow win for them.

**What would move it from "pilot" to "studio-wide deploy":**
1. **Shipped, signed binaries + a one-click installer on GitHub Releases** (the scripts exist — just publish artifacts). This single change unblocks non-engineer adoption.
2. **Unreal plugin out of beta** (past 0.6.x), with a Fab/marketplace drop and a smoke-tested packaged payload, so I don't hand-package `ThirdParty/Smatchet/lib`.
3. **An enforced, measured perf baseline on real GPU frames** — turn the parked perf gate into a published number so the 144 Hz claim is a guarantee, not a goal.
4. **Richer tracker parity**: Markdown comments, board/sprint view, and a notifications/mention feed — enough to actually pull people off Jira/Linear-web.
5. **Multi-backend visible at once** (Jira + GitHub side by side) for studios that split production vs. open-source tooling.

**I'd walk away if:**
- My studio doesn't use Perforce (the killer feature evaporates; a generic ImGui tracker isn't worth the build cost vs. Jira/Linear-web).
- I need a planning/board surface for producers — this isn't that.
- I can't tolerate depending on a pre-1.0, small-team internal tool for a daily-driver workflow, with no binary distribution and a beta engine plugin.

**Bottom line:** Smatchet is a sharp, opinionated *engineer's tool* with one genuinely differentiated game-dev feature (callstack-driven P4 annotate) and one genuinely novel delivery (in-Unreal overlay). For a Perforce-based Unreal studio's engine team, it's worth a pilot. For studio-wide tracking, it's not there yet — the gap is distribution and maturity, not capability.
