# Debugging features — "Log a bug" hotkey → fixed GitHub dev repo (+ phase-2 crash reporter)
<!-- plan-date: 2026-05-30 -->

## Context

Smatchet has **no fast path to file a bug** and **no crash handling** (verified: zero signal handlers / `SetUnhandledExceptionFilter` / `set_terminate`; the only crash seam is `SmatchetDrawFrameWithSeh`'s `__except`, which does a bare `std::exit(1)` with no logging — `Source/Standalone/main.cpp:189-190`). Context that would make a report useful (logs, recent tracker ops, version/build) already sits in memory but isn't capturable without leaving the app.

This adds a **global in-app hotkey (Ctrl+Shift+B, configurable)** that opens a modal: the user types a description, and Smatchet files a **new GitHub issue on a fixed, configured dev repo** with runtime context inlined into the issue body. The capture/redaction/submit logic is a **reusable service** so a **phase-2 in-process crash reporter** files to the same dev repo on the next launch.

**Decided behaviour (this revision):**
- **Destination = one fixed GitHub repo, always** — bug + crash reports go to the configured dev repo regardless of the user's active tracker (Jira/Plane/GitHub). The bug-logger does **not** use the active backend.
- **GitHub issue creation is currently a stub** (`GitHubClient::BuildCreatePayload` / `CreateIssue` → `StubError`, GitHubClient.cpp:430-446) — it must be **implemented**. This also fixes GitHub create for the normal new-issue UI (bonus), not just bug reports.
- Hotkey default **Ctrl+Shift+B**, configurable, in-app (ImGui poll).
- Screenshot optional, **on by default**, with a **censored (no readable text)** variant. Embedding (corrected after checking #337): the web-UI drag-drop store (`user-attachments`) is **cookie-only — PATs get HTTP 422** ([community#29993](https://github.com/orgs/community/discussions/29993)), so the app can't post there; but a screenshot **can** be embedded inline by uploading the PNG via a documented PAT path — **Contents API** (→ `raw.githubusercontent.com` URL) or a **Release asset** (→ download URL) — then `![screenshot](url)` in the body. Needs `contents:write` on the assets repo; degrades to local-save+note if the token lacks it. (`data:` base64 images are stripped by GitHub, so upload is required.) Logs/audit/env inline as text.
- Crash reporter **in-process best-effort** (phase-2 design, deferred).

**Reuses (verified):** `GitHubClient(baseUrl, pat)` ctor + `BuildGitHubHeaders(pat)` Bearer auth + cpr (GitHubClient.cpp:53,72,91); `ParseGitHubIssueKey` for the `owner/repo#N` shape (GitHubClientHelpers.cpp:16); `IssueCreatePipeline::Run(ITrackerIssueMutations&, LocalCacheManager*, draft, RequiredFieldSet, catalog)` (cache may be null); `IssueDraft` (FieldValues `summary`/`description`, ProjectKey=`owner/repo`); `Logger::GetEntriesSnapshot()`; `BackendAuditTrail::ReadRecentEvents` + `RedactJson`; the existing screenshot path (`g_ui.requestScreenshot`); `SmatchetToastManager`; `LaunchBackgroundTask`; `RunOnUiThreadAsCommandResult`; the `std::future` poll + `#define ImGui SmatchetLocalizedImGui` idioms in `SmatchetNewIssueDraftUi.cpp`.

> On approval, copy to `docs/plans/shipped/log-a-bug-github.md` and commit `wip(plan): log-a-bug-github` per `AGENTS.md` § Process rules. Touches `Source/Core/` → perf-gate section below mandatory. Strict-lint zones touched: `Source/Core/src/Tracker/`, `Commands/`, `Config/`.

---

## Architecture at a glance

```text
Ctrl+Shift+B (poll in SmatchetUI::Draw, BackendHasBeenReachable gate)
View/Help "Report a Bug…"  ──┐
bug.report (CLI/MCP/Lua)    ─┴─→ g_ui.showBugReport = true
                                          │
              SmatchetBugReportUi (modal, Core/Ui, dual-target)
              description + screenshot toggle (full/censored) + egress PREVIEW (consent)
              Submit → worker thread:
                                          │
        diagnostics::BugReportService (Core/Diagnostics, reusable)
        GatherContext → BuildMarkdownBody(+redact, <details>, ≤64KB) → BuildDraft
                                          │
        owns a DEDICATED GitHubClient(devBaseUrl, devPat)   [NOT the active backend]
        IssueCreatePipeline::Run(devClient, /*cache*/nullptr, draft, minimalRequired, {})
                                          │
        GitHubClient::BuildCreatePayload + CreateIssue  ← NEWLY IMPLEMENTED
        POST {baseUrl}/repos/{owner}/{repo}/issues  →  owner/repo#N
```

`cache=nullptr` so dev-repo issues never pollute the user's local SQLite cache or grid. Submit bypasses `CreateIssueAsync` (which targets the active backend + refreshes/ prefetches the user's view — wrong for a foreign repo).

---

## Phase 1 — Bug-logger → fixed GitHub dev repo

### Slice 1 — Implement GitHub issue creation (tracker-backend)

Prerequisite and independently valuable (fixes the new-issue UI for GitHub too). Today every GitHub mutation is a stub.

**Pure payload-builder in `GitHubClientHelpers.{h,cpp}` (cpr-free TU → doctest-able without linking cpr, per the helper-split convention at GitHubClientHelpers.h:9-10):**
- `bool BuildGitHubCreatePayload(const IssueDraft& draft, nlohmann::json& out, std::string& err)` — map `FieldValues["summary"]`→`title` (required; empty → false), `FieldValues["description"]`→`body`, optional `labels`/`assignees` (comma-split → JSON array). Resolve the target repo from `draft.ProjectKey` (shape `owner/repo`) and carry it out-of-band as a single `out["__target"] = {owner, repo}` object so `CreateIssue` can form the URL.
- `std::string FormatGitHubIssueKey(const std::string& owner, const std::string& repo, std::int64_t number)` — inverse of `ParseGitHubIssueKey`, composes `owner/repo#N`.

**Modify `Source/Core/src/Tracker/GitHubClient.cpp` (replace the two stubs; NO new REST helper — mirror the inline `ProbeReachability` style that already uses the logged HTTP helpers):**
- `BuildCreatePayload(draft, catalog, out, err)` — delegate to `smatchet::github::BuildGitHubCreatePayload` (catalog ignored; GitHub's create surface is title/body/labels). Clear the stub.
- `CreateIssue(const json& fields, std::string& err)` — extract + `erase("__target")`, `POST {baseUrl_}/repos/{owner}/{repo}/issues` via `TrackerPostLogged("GitHubClient", url, BuildGitHubHeaders(pat_), body.dump())`; guard `pat_.empty()` → `kPatMissingError`; on 200/201 parse `number` → `FormatGitHubIssueKey`; non-2xx → `outError` via `ExtractGitHubErrorMessage(status, text)`. Audit trail (`MakeOperationId` → `AppendBegin`/`AppendResult`, source `"github_client"`) like `JiraClient::CreateIssue`. Add `#include "BackendAuditTrail.h"` + `"IssueDraft.h"`.
- `AttachFilesToIssue` — **leave unoverridden**; the base default `"not supported by this backend"` (ITrackerIssueMutations.h:40-45) is correct (no per-issue attachment endpoint — screenshots embed via the Slice-2 Contents/Release upload, not this interface). Callers treat it as non-fatal.

**Tests:** doctest the pure `BuildGitHubCreatePayload` (title/body/labels mapping, `__target`, missing-summary → false) + `FormatGitHubIssueKey` (`owner/repo#5`) — both cpr-free. `CreateIssue`'s HTTP path is integration-verified against a real repo (below), not doctested (would drag cpr into the rig). *(Not via `GitHubFixtureBackend` — that's a fetch-path fake, it doesn't exercise create.)*

### Slice 2 — Pure core + dedicated-destination submit + config

**Create:**
- `Source/Core/include/Privacy/TextRedaction.h` + `src/Privacy/TextRedaction.cpp` — `std::string RedactLogLine(const std::string&);` keyless free-text scrubber (the existing `BackendAuditTrail::RedactText` needs a *key*). C++14 `std::regex` statics: `Authorization: Bearer/Basic`, URL `token/api_key/password=` params, `user:pass@host`, emails, long hex/base64 (≥40 chars so commit SHAs survive). Pure → reused by the crash path (reads the log file, not live state).
- `Source/Core/include/Imaging/ScreenshotCensor.h` + `src/Imaging/ScreenshotCensor.cpp` — `void MosaicCensorInPlace(unsigned char* px, int w, int h, int comp, int block);` (block-average downsample → nearest-upscale, in place, edge cells in-bounds only) + `int RecommendedCensorBlock(int w,int h)` → `clamp(round(min(w,h)/64),12,48)` (12px floor guarantees ~13px body text unreadable). Core, no GL.
- `Source/Core/include/Diagnostics/BugReportService.h` + `src/Diagnostics/BugReportService.cpp` — `namespace smatchet::diagnostics`. Layered, mostly pure so it's testable without network:
  ```cpp
  struct BugReportOptions {
      std::string UserDescription, ScreenshotAbsPath; // ScreenshotAbsPath filled by standalone UI; empty otherwise
      bool IncludeScreenshot=true; std::size_t MaxLogLines=200, MaxAuditEvents=50;
      // ReportKind Kind = ReportKind::Bug;  // Bug|Crash — both route to the dev repo (phase 2)
  };
  struct ContextBundle { nlohmann::json Env; std::vector<std::string> LogLines; nlohmann::json AuditEvents; std::vector<std::string> McpActivity; };
  ContextBundle GatherContext(const AppController&, const BugReportOptions&);
  std::string   BuildMarkdownBody(const BugReportOptions&, const ContextBundle&);  // pure; redacts; GitHub-flavored <details>; hard ≤~60KB ceiling (GitHub body cap 65536)
  std::string   RedactLogText(const std::string&);
  // Resolves dev-repo target + PAT from config/env; constructs a dedicated GitHubClient; runs IssueCreatePipeline::Run.
  struct SubmitResult { bool Ok=false; std::string IssueKey, Error, Url, LocalStageDir; };
  SubmitResult  SubmitBugReport(AppController&, const BugReportOptions&);          // blocks on the pipeline → worker-thread only
  ```
  - **Dev-repo target + PAT + baseUrl resolution.** owner/repo from `cfg.BugReportGitHubOwner`/`BugReportGitHubRepo`. **PAT (secret — env/keychain first, persisted only opt-in):** env `SMATCHET_BUGREPORT_GITHUB_TOKEN` → the user's existing `cfg.GitHubPat` (already persisted by the GitHub backend — no *new* secret surface) → persisted `cfg.BugReportGitHubPat` **only when `cfg.BugReportPersistPat==true`** (default off; otherwise a token is never written to config). baseUrl = `cfg.BugReportGitHubBaseUrl` → `cfg.GitHubBaseUrl` → `https://api.github.com`, then **validate with `IsValidGitHubBaseUrl(baseUrl, err)`** (GitHubClientHelpers.h:76) *before* constructing the client. If owner/repo missing, no usable PAT, or baseUrl invalid → `SubmitResult{Error=<specific>}` (modal shows it; never silently drop, never hand a malformed URL to `GitHubClient`).
  - `SubmitBugReport`: build `IssueDraft` (`ProjectKey = owner+"/"+repo`, `FieldValues["summary"]="[Bug] "+firstLine`, `FieldValues["description"]=BuildMarkdownBody(...)`); construct `GitHubClient devClient(baseUrl, pat)`; build a **local** `RequiredFieldSet{ FieldIds={}, RequiresIssueType=false }` (GitHub has no issue type; do **not** call `AppController::GetRequiredFieldSet`, which reads the active backend's metadata); `IssueCreatePipeline::Run(devClient, nullptr, draft, required, {})`. Return key/url/error.
  - **Screenshot embed:** if `ScreenshotAbsPath` set, upload the PNG via the **Contents API** (`PUT /repos/{assetsOwner}/{assetsRepo}/contents/bug-assets/<stamp>.png`, base64 body, `contents:write`) — or a **Release asset** — then embed `![screenshot](<raw/download URL>)` in the body. Assets target = `cfg.BugReportAssetsRepo` (default = the dev issue repo) on a dedicated `bug-report-assets` branch to keep main history clean. The censored variant uploads identically (just a different PNG). On upload failure / token lacks `contents:write` → copy to the local stage dir (`ConfigManager::GetUserDataDirectory()/bug_reports/<stamp>/`) + body note "Screenshot saved locally: `<filename>`" (graceful degrade, never fatal).
  - **Body content** (`BuildMarkdownBody`): header table (`GetAppVersion`, `SmatchetHost_GetBuildTag`, OS/arch, active tracker, UTC); the user description; `<details><summary>Recent log (N lines)</summary>` fenced block (redacted via `RedactLogText`); `<details>` recent audit events (already redacted by `RedactJson`); env summary (`RedactJson`). Hard-cap total at ~60KB; truncate the log slice with a "… (truncated)" marker.

**Modify `Source/Core/include/Config/ConfigManager.h` + `ConfigManager.cpp`** (strict zone; serialization is **hand-written** — verified no `NLOHMANN_DEFINE` on `TrackerConfig` — add fields to both struct and the `Load`/`Save` mapping, `obj["k"]=v`): `std::string BugReportGitHubOwner, BugReportGitHubRepo, BugReportGitHubBaseUrl, BugReportAssetsRepo; bool BugReportPersistPat=false; std::string BugReportGitHubPat; std::string BugReportHotkey="Ctrl+Shift+B"; bool BugReportHotkeyEnabled=true; bool BugReportScreenshotDefault=true;` (`BugReportAssetsRepo`=`owner/repo` for screenshot uploads, empty → reuse the issue repo). ⚠️ **`BugReportGitHubPat` is a plaintext secret. Default token source is env/keychain (`SMATCHET_BUGREPORT_GITHUB_TOKEN`) or the existing `cfg.GitHubPat`; `BugReportGitHubPat` is read/written ONLY when the opt-in `BugReportPersistPat==true`. By default no bug-report token is persisted to `smatchet_config.json`.**

**Tests (`tests/Core/`, doctest):** `RedactLogText`; `MosaicCensorInPlace` (unreadability metric + clamps); `BuildMarkdownBody` (header, `<details>` blocks, 60KB cap + truncation marker, redaction applied); dev-repo/PAT resolver (override→env→`GitHubPat`; missing → error).

### Slice 3 — Command + config wiring

**Create `Source/Core/src/Commands/Builtin/BuiltinCommands_BugReport.cpp`** (strict zone) — `bug.report` (category `bug`):
- Params: `description` (opt), `screenshot` (bool=true), `censored` (bool=false). Flags `Destructive=true, Idempotent=false, AsyncSafe=false, DryRunSupported=true`. Template = `ticket.create` (`BuiltinCommands_TicketMutations.cpp:217-251`).
- **Modal mode** (no `description`): `RunOnUiThreadAsCommandResult(app, []{ g_ui.showBugReport=true; … })` — flag only, no submit.
- **Headless mode** (`description` set): `diagnostics::SubmitBugReport(app, opts)` (handler already off-UI-thread); returns `{ok, issueKey, url}`. No `g_ui` touch. **Text-only** — no screenshot (frame capture is a modal/standalone-GL-only step; CLI/MCP have no live frame). The `censored` param is therefore meaningful only in modal mode.
- **DryRun**: return resolved dev owner/repo + whether a PAT resolves, no submit.

**Modify:** `Commands/Builtin/BuiltinCommands_Internal.h` (declare `RegisterBugReportCommands`); `Commands/BuiltinCommands.cpp` (call it).

### Slice 4 — Hotkey + modal + screenshot wiring

**Create:**
- `Source/Core/include/Ui/ImGuiHotkey.h` + `src/Ui/ImGuiHotkey.cpp` — `struct ImGuiBugHotkey{bool ctrl,shift,alt; ImGuiKey key;}; bool ParseImGuiHotkey(const std::string&, ImGuiBugHotkey&); bool MatchHotkey(const ImGuiIO&, const ImGuiBugHotkey&);`. Tokenize on `+`, ASCII-lower, map to `ImGuiKey_*`. **Do not** reuse Whisper `HotkeyParse` (Win32 VK codes, `SMATCHET_WITH_WHISPER`-conditional plugin — verified).
- `Source/Core/src/Ui/SmatchetBugReportUi.cpp` (+ `.h`) — modal. TU head mirrors `SmatchetNewIssueDraftUi.cpp:19-22` (`#include "imgui.h"`,`"SmatchetLocalizedImGui.h"`, `#define ImGui SmatchetLocalizedImGui`). **No GL/GLFW.** Entry `void SmatchetBugReportUi_Draw(AppController&, UiDrawSession&);`.
  - Layout: dev-repo destination indicator (`Destination: owner/repo`, read-only); multiline description (lazy 64KB `std::vector<char>`, `SetKeyboardFocusHere` first frame); `Checkbox("Attach screenshot")` (default from config) → indented `Full`/`Censored (no readable text)` radios (`BeginDisabled` when off; **hidden** when no capture backend — DX12) with a tooltip noting the screenshot is uploaded as a repo/release asset and embedded inline (the GitHub drag-drop store isn't PAT-accessible); collapsible **egress preview** (the `BuildMarkdownBody` output, rebuilt only when dirty — the consent surface); error banner; `Submit` (disabled in-flight or empty desc) + `Cancel`.
  - Async submit (never blocks UI thread): on Submit, optionally request a screenshot (state machine `Idle→AwaitingShot→Submitting`, since the GL read lands post-swap), then `app.LaunchBackgroundTask([...]{ auto r = SubmitBugReport(app, opts); app.mainThreadDispatcher.PostToMainThread([r]{ /* set g_ui.bugReportResult + clear bugReportInFlight HERE, on the UI thread */ }); })`. The worker computes the result, then **posts the `g_ui` mutation back to the UI thread** via the dispatcher — no UI-thread polling of worker-written state, no data race. On the posted callback: success → toast (`SmatchetToastManager::Push("Bug report", "Filed owner/repo#N", Success)`) + close; failure → keep open, banner, preserve text.
  - Keyboard: Ctrl+Enter = Submit (plain Enter = newline); Esc = Cancel (suppressed in-flight).

**Modify:**
- `Source/Core/include/Ui/SmatchetUiSession.h` — `bugReport*` state near the other `show*` latches: `bool showBugReport, bugReportOpenLatch, bugReportInclScreenshot=true, bugReportPreviewOpen, bugReportInFlight, bugReportCrashMode/*ph2*/, bugReportPreviewDirty=true; int bugReportShotMode/*0 full,1 censored*/; std::vector<char> bugReportDescBuf; std::string bugReportStagedShotPath, bugReportPreviewText; std::shared_ptr<smatchet::diagnostics::SubmitResult> bugReportResult;` (fwd-declare the struct). `bugReportResult` + `bugReportInFlight` are mutated **only on the UI thread** (set in the dispatcher-posted callback above) — no `g_ui`-side synchronisation needed.
- `Source/Core/src/Ui/SmatchetUI.cpp` — (a) poll inside the `if (g_ui.cfg.BackendHasBeenReachable)` gate (`:394`): `if (g_ui.cfg.BugReportHotkeyEnabled && ParseImGuiHotkey(g_ui.cfg.BugReportHotkey,hk) && MatchHotkey(io,hk)) { g_ui.showBugReport=true; g_ui.bugReportOpenLatch=true; }` (config is the per-session `g_ui.cfg` copy); (b) `SmatchetBugReportUi_Draw(app, g_ui);` right after `commandPalette_.Draw(app)` (`:406`). *(Optional: gate the hotkey on `BackendHasBeenReachable`? The dev-repo destination is independent of the active backend, so consider polling it **outside** the gate so bug reporting works even when the user's tracker is unreachable — preferable for a bug tool. Decide at implementation; outside-gate is the better UX here.)*
- `Source/Standalone/main.cpp` — add `g_ui.requestScreenshotCensor`; in the capture block (`639-671`), after the vflip→RGB build and before `stbi_write_png` (~:665): `if (g_ui.requestScreenshotCensor) MosaicCensorInPlace(rgb.data(), fw, fh, 3, RecommendedCensorBlock(fw,fh));`. Temp PNG under the user-data dir.
- `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp` — *(optional)* "Report a Bug…" item → `g_ui.showBugReport=true`.
- CMake — **verified**: Core sources are `file(GLOB_RECURSE … "Source/Core/src/*.cpp" CONFIGURE_DEPENDS)` (CMakeLists.txt:606), so new `.cpp` (incl. new subdirs) are auto-compiled — no source edit. **Required edit:** include subdirs are *explicit*, not globbed (`SmatchetCoreInterface` `target_include_directories`, CMakeLists.txt:719-729 lists `Tracker/Sync/Persistence/Config/Ui`) → **add `Source/Core/include/Diagnostics`, `…/Privacy`, `…/Imaging`** there (covers both Standalone + DX12 via the INTERFACE target) so bare includes resolve, OR use subdir-qualified includes (`"Diagnostics/BugReportService.h"`) which resolve via the on-path `Source/Core/include` root. Register the new `.test.cpp` files in `tests/CMakeLists.txt` (explicit `add_executable` list).

---

> **Phase 2 IMPLEMENTED 2026-05-30** (see § Phase-2 implementation log at the end). The design below is retained for reference.

## Phase 2 — In-process crash reporter (designed, deferred)

Reuses Slice-2 service + Slice-4 modal; files to the **same fixed dev GitHub repo**. A crash report = a pre-populated bug report filed on the **next** launch.
- **Install:** top-level `SetUnhandledExceptionFilter` (Windows) + augment `SmatchetDrawFrameWithSeh`'s filter to write a marker **before** the existing `std::exit(1)` (today writes nothing); `std::set_terminate` + `std::signal(SIGSEGV/SIGABRT/SIGFPE/SIGILL)` for portable/Clang. New `Source/Standalone/SmatchetCrashHandler.cpp` + Core `Diagnostics/CrashSink.{h,cpp}`.
- **Async-safe handler:** no heap/locks/logger/UI. Only `MiniDumpWriteDump` (DbgHelp, `dbghelp.lib`, `#if _WIN32`) + raw `WriteFile`/`write` of a **pre-formatted** marker (static buffer built at startup). Dumps → `<userdata>/crashes/`, retain last 5. Symbols deferred to dev side (upload the `.dmp` as a Release asset — see § Crash-dump delivery below).
- **Survives-the-crash context:** enable `Logger::SetFileSinkPath(<userdata>/logs/smatchet.log)` at startup (the handler can't call `GetEntriesSnapshot()` — locks+allocates); a cheap per-state-change breadcrumb file. Next launch reads these **files**, `RedactLogText` the tail.
- **Next-launch flow:** detect marker → set `bugReportCrashMode`, pre-fill "Smatchet closed unexpectedly. What were you doing?", open the same modal → submit to the dev repo → delete marker immediately (no loop), keep dump until success.
- **Crash-dump delivery:** same upload path — attach the `.dmp` as a **Release asset** (binaries off the git tree) and link it in the body; inline the exception code + top module as text. (No `user-attachments` PAT path, same as screenshots.) Resolve exact mechanism when phase 2 is scheduled.

---

## Risks / gotchas

- **Image embedding (corrected, verified 2026).** The drag-drop `user-attachments` store is **cookie-only — PATs return 422** ([community#29993](https://github.com/orgs/community/discussions/29993), [cli/cli#12960](https://github.com/cli/cli/issues/12960)), not API-accessible; `data:` base64 is stripped. So screenshots embed by **uploading the PNG** (Contents API → `raw.githubusercontent.com`, or Release asset → download URL) + `![](url)`, which **does** render inline. Requires `contents:write` on the assets repo (superset of `issues:write`); if the resolved token has only `issues:write`, screenshot degrades to local-save + body note (non-fatal). Repo clutter mitigated by a dedicated `bug-report-assets` branch/repo.
- **Per-user write access + secret PAT.** Filing to a fixed dev repo requires the resolved token to have `issues:write` on it. Reusing the user's `cfg.GitHubPat` works only if they can open issues there; otherwise a dedicated `SMATCHET_BUGREPORT_GITHUB_TOKEN` (env, not plaintext config) is needed. If users are external/untrusted, a fixed repo needs a relay/bot instead of a shipped token — flag before ship.
- **GitHub create was entirely stubbed** — Slice 1 is real backend work (HTTP POST, auth, response parse, key format, audit wiring), but all substrate (headers, cpr, key parser, config) exists. It also unblocks the normal new-issue UI for GitHub (bonus; verify no regression there).
- **No active-project getter** (verified absent) — irrelevant now: the dev repo comes from config, not the active project.
- **Body 64KB cap** — `BuildMarkdownBody` truncates the log slice; no full-log attachment possible on GitHub.
- **Offline** — `QueueCreateOffline`/`OfflineQueueService` replay against the **active** backend, not the dev GitHub client → not reusable here. v1 = **online-only**; on failure, persist the stage dir + a pending marker and retry on next launch (lightweight, dedicated). Flag as a v1 limitation.
- **Dedicated-client lifetime** — `GitHubClient` constructed per-submit on the worker; capture config by value; share `SubmitResult` via `shared_ptr`; `cache=nullptr` so no cache/grid side effects.
- **Dual-target** — modal/service/censor in Core, no GL; screenshot capture stays in `main.cpp`; hide the screenshot toggle on DX12. Verify the modal + `GitHubClient` link in `SmatchetCore_DX12`.
- **Strict-lint zones** — `Tracker/GitHubClient.cpp`, `Commands/Builtin/*`, `Config/ConfigManager.{h,cpp}`: no raw `new`/`delete`, `LOG_*` only, `const&` non-trivial params, `std::move` last use, `obj["k"]=v`.
- **Log redaction best-effort** — preview + consent gate is the real safeguard; don't over-promise in UI.

---

## Verification

- **Unit (doctest, `ninja-test-msvc`):** pure `BuildGitHubCreatePayload` mapping + `FormatGitHubIssueKey` (`owner/repo#N`); `RedactLogText`; `MosaicCensorInPlace`; `BuildMarkdownBody` (cap/truncation/`<details>`/redaction); dev-repo/PAT resolver. (`CreateIssue` HTTP = integration, below, not doctest — avoids dragging cpr into the rig.)
- **GitHub create, real:** configure `BugReportGitHubOwner/Repo` + a token with `issues:write`; `smatchet-cli bug.report --description "Grid freezes sorting by date" --yes` (`--yes` is the existing **global** destructive-confirm flag — CliCommandRunner.cpp:357 → `ctx.ConfirmedDestructive` — not a `bug.report` param) → assert an issue is created in the dev repo, key `owner/repo#N` returned, body contains the header table + `<details>` log block + redaction; `--dry-run` reports resolved repo + PAT-present without posting.
- **Manual modal:** Ctrl+Shift+B → focused → toggle screenshot + censored → expand preview (secrets scrubbed) → Submit → toast with `owner/repo#N` → open the GitHub issue and confirm the screenshot **renders inline** (uploaded asset URL) and the censored variant shows no readable text; rebind via `BugReportHotkey`; `BugReportHotkeyEnabled=false` → no hotkey.
- **Token-scope degrade:** with a token that has only `issues:write` (no `contents:write`), confirm the issue still files and the body carries the local-path note instead of an embedded image (non-fatal).
- **Independent-of-active-backend:** with the active tracker set to Jira (or unreachable), confirm Ctrl+Shift+B still files to the dev GitHub repo.
- **Dual-target:** `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` — DX12 compiles, modal opens and files without a screenshot.
- **Perf gate** (touches `Source/Core/`): modal draw + hotkey poll trivial; preview rebuild gated on `bugReportPreviewDirty` (no per-frame re-scrub) to hold ≤6.94 ms; capture/submit run off the UI thread. Run the curated perf scenario at the slice boundary if the diff hits the map.

---

## Suggested agent ownership

- **tracker-backend** — Slice 1 (GitHub create impl + REST helper + key format) and the Slice-2 dedicated-client submit path.
- **command-system** — Slice 3 (`bug.report` command) + config keys + the hotkey poll/parse.
- **orchestrator / hand-built UI** — Slice 4 modal + `ScreenshotCensor` + `TextRedaction` + main-loop wiring.
- **test-rig** — the doctest files.
- Phase 2 (deferred) — `debug-detective` / `build-doctor` for crash-handler install + DbgHelp + dump delivery.

---


## Implementation log

Shipped across three PRs (newest last):

- **`6eab3dbc` · #578 — Phase 1: hotkey + modal + GitHub create + relay.** GitHub issue create (`BuildGitHubCreatePayload`/`FormatGitHubIssueKey`; `GitHubClient::BuildCreatePayload`/`CreateIssue` — POST issues, Bearer PAT, audit, `owner/repo#N`). Pure core: `Privacy/TextRedaction`, `Imaging/ScreenshotCensor`, `Diagnostics/BugReportService` split pure (`BugReportBody.cpp`) / heavy (`BugReportService.cpp`); config fields (PAT DPAPI-encrypted, opt-in). `bug.report` command (modal/headless/dry-run). `Ui/ImGuiHotkey` + `SmatchetBugReportUi` modal (async submit), hotkey poll, censor wiring, Help-menu item. **Relay mode** (`tools/bug-report-relay/` Worker + `bugreport_relay_url`/`_key`) — token server-side, never bundled.
- **`7268d13d` · #596 — polish.** Clean capture (modal excluded via arm→request + suppress), downscale to 1280px, editable egress preview (`BodyOverride`), lighter + configurable censor (`bugreport_censor_block`, default 2), and the bug reporter **never borrows the tracker `GitHubPat`**. Relay `index.js` hardened (byte-cap, branch-race, title-safety); payload cap 256 KB → 2 MB.
- **`2c714ff2` · #599 (2026-05-31) — Phase 2 crash reporter + follow-ups.** `Core/Diagnostics/CrashSink.{h,cpp}` (async-signal-safe marker + breadcrumb + next-launch consume + crashed-session **log-tail** via `session_log.txt` stash-before-overwrite). `Standalone/SmatchetCrashHandler.{h,cpp}` (`SetUnhandledExceptionFilter` + `set_terminate` + signals + `MiniDumpWriteDump`; frame-loop `__except` writes marker+dump). `main.cpp` install (after user-data dir resolves) + next-launch pre-filled modal. **Minidump → GitHub Release asset** (`crash-dumps` prerelease; direct `UploadCrashDumpRelease` + relay `uploadCrashDump`). `debug.crash` command + `CrashSink.test.cpp`. CR-round fixes: preview gen moved to a worker (no UI-thread audit I/O), relay `atob` try/catch, `UrlEncode(dumpName)`.

## Deviations from plan

1. `BuildGitHubCreatePayload` takes primitive fields, not `IssueDraft` (IssueDraft pulls SQLite; keeps the helpers TU cpr/SQLite-free).
2. Service split into pure `BugReportBody.cpp` + heavy `BugReportService.cpp`; `BuildMarkdownBody` is a pure function of a `GatherContext` bundle.
3. PAT is DPAPI-encrypted, not plaintext (matches `github_pat`); opt-in persist, default off.
4. `build_tag` — `SmatchetHost_GetBuildTag()` only links in the Unreal host; Standalone uses a `__DATE__ __TIME__` tag (guarded).
5. Hotkey polled OUTSIDE the `BackendHasBeenReachable` gate (works when the tracker is unreachable).
6. Screenshot capture: `bugReportShotArmed` → request-next-frame + modal-suppressed (clean shot); main-thread `bugReportShotReady` signal (no UI-thread filesystem poll).
7. Audit pattern mirrors `JiraIssueMutation.cpp` (plan said `JiraClient::CreateIssue`).
8. External-token risk filed P1 security backlog → resolved by relay (token never bundled).
9. Bug reporter never borrows the tracker `cfg.GitHubPat` (user directive) — `env → BugReportGitHubPat` only, else relay.
10. Censor mosaic made configurable (`bugreport_censor_block`, default 2) after "too blurry" feedback. *(Follow-up plan `bug-report-font-redaction-censor.md` replaces the mosaic with font-redaction — not in this feature.)*
11. Crash log-tail **implemented** (not dropped) via the `session_log.txt` stash-before-overwrite trick (sidesteps the per-PID can't-locate-next-launch race).
12. Crash-dump delivery **implemented** as a GitHub Release asset (not local-only).

## Verification (actual)

- **Builds:** dual-target (`SmatchetStandalone` + `SmatchetCore_DX12`) clean at each slice.
- **Unit:** full doctest suite green (877/877 at the Phase-2 close) — create-payload/key, redaction, mosaic/downscale, markdown body, `ResolveBugReportTarget` (never-tracker-PAT), `BuildRelayRequest` (+dump), `CrashSink` (marker round-trip + log-tail).
- **Lint:** `test-lint-rules.sh` PASS (no new strict-zone violations) at each slice.
- **Relay, live ✓:** Worker at `smatchet-bug-report-relay.smatchet.workers.dev`; `/health` 200; `/report` no-key → 401 (gate); keyed → filed `alexandrosk0/Smatchet#585`. 404-from-nonexistent-REPO diagnosed+fixed; 413 fixed via 1280px downscale + 2 MB cap (redeployed).
- **Modal, live ✓:** Ctrl+Shift+B → describe → attach → submit filed `#587`/`#588`; editable preview + clean-capture (no modal in shot) confirmed.
- **Token-degrade ✓:** the app PAT had Issues:write but not Contents:write → screenshot degraded to a local-save note (graceful), as designed. Inline-screenshot render still pending a Contents:write token.
- **Crash pipeline ✓:** `CrashSink` unit tests pass; `CrashSinkInit` + breadcrumb verified live in the real user-data dir; planted-marker relaunch → detect → consume → archive dump → pre-filled modal + log WARN confirmed; `debug.crash` provably kills the process. Real-fault → marker is standard Win32 (substrate unit-verified); real-crash → modal end-to-end is a user manual check.
- **Censor ✓:** lighter + config-tunable block accepted by the user.

> **Status:** Phase 1 (#578) + polish (#596) + Phase 2 (#599, `2c714ff2`) all merged to `develop` (2026-05-31); plan archived `active → shipped/`. Two user-side manual checks remain (real-fault → modal crash end-to-end; inline-screenshot render once a Contents:write-scoped token exists) — the functionality is in place; only a real fault / the privileged token is needed.
