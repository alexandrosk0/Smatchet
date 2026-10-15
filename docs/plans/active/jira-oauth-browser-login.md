# Plan — Jira browser sign-in (OAuth 2.0 3LO, bring-your-own app)

> **Slug**: `jira-oauth-browser-login`
>
> **Status**: `active`

## Context

Jira auth today is a static `email:api-token` Basic header (`BuildTrackerBasicAuthHeader`, `TrackerHttpUtils.cpp`), pasted into Preferences → Tracker. The user asked to authenticate through the browser instead. After this lands, a Jira Cloud user can run **Sign in with browser**: the system browser opens Atlassian's consent page, a one-shot loopback listener receives the authorization code, and Smatchet talks to Jira with a short-lived Bearer token that it refreshes on its own. API-token auth stays as the default and the headless fallback (CLI, MCP, CI cannot open a browser).

Constraint found while researching: Atlassian Cloud OAuth 2.0 (3LO) is authorization-code with a **client secret**; it has no secretless PKCE path for public clients (unconfirmed against current docs — see Risks). A desktop binary cannot hold a secret, so the user chose **bring-your-own OAuth app**: each user/team registers an Atlassian OAuth 2.0 (3LO) app and enters its client ID and secret in Preferences. No secret ships in the binary and no relay server is needed.

## Approach

Three layers, bottom-up:

1. **`JiraOAuthPure`** — pure, I/O-free functions: authorize-URL builder, callback-query validation (state compare in constant time, `error` handling), token request/response bodies, `accessible-resources` parsing and site selection by `Domain`, refresh-skew policy, and the `api.atlassian.com/ex/jira/{cloudId}` base. Fully unit-testable on the Linux subset.
2. **`JiraOAuthSession`** — owns the in-memory access token + expiry; `Bearer(cancel)` refreshes when within the skew window, single-flight under a mutex. Atlassian refresh tokens **rotate**, so every successful refresh hands the new refresh token to a persist callback before the old one is dropped. A `invalid_grant` refresh clears the session and surfaces `TrackerError` kind Auth (user must sign in again), never a silent retry loop.
3. **Integration** — `TrackerConfig` gains `JiraAuthMode` (`apitoken` | `oauth`) and the OAuth fields; secrets (client secret, refresh token) go through the existing per-platform secret arms in `ConfigManager_Secrets.cpp`. `BuildTrackerHeaders` emits `Bearer` in oauth mode; a single `JiraApiBase(cfg)` replaces the per-call `NormalizeBaseUrl(cfg.Domain)` in the Jira TUs (oauth mode routes through `api.atlassian.com`, while `/browse/` links keep the site URL). A loopback listener (cpp-httplib, `127.0.0.1` only, fixed configurable port because Atlassian matches the callback URL exactly, one request, hard timeout) completes the flow; a registered command `jira.oauth_login` drives it so CLI, palette and MCP get it for free.

Access tokens are never persisted (memory only); only the rotating refresh token is.

## Files to modify

New (grep `JiraOAuth`/`OAuth` in `Source/` found nothing pre-existing):
1. `Source/Core/include/Tracker/JiraOAuthPure.h`, `Source/Core/src/Tracker/JiraOAuthPure.cpp` — pure helpers above.
2. `Source/Core/include/OsRandom.h`, `Source/Core/src/OsRandom.cpp` — `FillOsCsprng` + `RandomUrlSafeToken(bytes)` for the `state` value (Core needs it; the Standalone copies in `CliSpawn.cpp` are MCP-only and not linkable from Core).
3. `Source/Core/include/Tracker/JiraOAuthSession.h`, `Source/Core/src/Tracker/JiraOAuthSession.cpp` — token holder + refresh.
4. `Source/Core/include/Tracker/JiraOAuthLoopback.h`, `Source/Core/src/Tracker/JiraOAuthLoopback.cpp` — one-shot callback listener.
5. `tests/Core/JiraOAuthPure.test.cpp`, `tests/Core/JiraOAuthSession.test.cpp` — doctest.

Modified:
6. `Source/Core/include/Config/ConfigManager.h` — new `TrackerConfig` fields.
7. `Source/Core/src/Config/ConfigManager_Secrets.cpp` (+ `_Load`/`_Save` key tables) — persist client secret + refresh token on the Win32, Android and POSIX arms with the existing encrypt-or-fallback contract.
8. `Source/Core/src/Tracker/TrackerHttpUtils.cpp` / `.h` — `Bearer` branch in the header builder; `EnsureTrackerAuthConfig` accepts oauth mode.
9. `Source/Core/src/Tracker/Jira*.cpp` (`JiraClient`, `JiraIssueSearch`, `JiraIssueMutation`, `JiraUserAndMeta`, `JiraActivityFeed`) — `JiraApiBase(cfg)` at the ~27 base-URL sites.
10. `Source/Core/src/Commands/Builtin/` — `jira.oauth_login` / `jira.oauth_logout` command registration (+ CLI guide row; `cli-guide-registry-parity-gate`).
11. `tests/CMakeLists.txt`, `Source/Core/CMakeLists.txt` — register new TUs in the Linux subset and the full rig.
12. `Source/Core/src/Tracker/{README,CONTEXT}.md`, `PRIVACY.md` — document the auth modes and the extra hosts contacted (`auth.atlassian.com`, `api.atlassian.com`).

Deferred to a follow-up (visual-validation exception — touches `Smatchet*Ui*.cpp`, cannot be verified without a launched Windows exe): the Preferences → Tracker **Sign in with browser** button, status line and mode radio.

## Existing utilities reused

- `TrackerPostLogged` / `TrackerGetLogged` (`TrackerHttpUtils.h`) — all token/resource traffic, so retry, redaction, `NetworkUsageTracker` and no-redirect policy apply.
- `ClassifyTrackerResponse` / `TrackerErrorFromHttpStatus` (`TrackerError.h`, `TrackerHttpClient.h`) — error kinds preserved.
- `json_safe::ParseBounded` (`Json/BoundedJsonParse.h`) — every parse of Atlassian bytes (rule `bare-json-parse-untrusted`).
- `UrlEncode`, `Base64Encode`, `TrimCopyAsciiWhitespace` — existing string helpers.
- `ProtectSecretForConfig` / `ApplySecretPersist` — secret-at-rest contract.
- `RedactHttpBodyForLog` — already scrubs bearer shapes from logged bodies.
- `SmatchetMergeWatchNotifyServer` — precedent for a 127.0.0.1-only cpp-httplib listener.
- Browser launch: `AttachmentAppUpdateService` `OpenUrl` dep seam — injected, not re-implemented.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — adds new TUs, extracts nothing.

## UX Pillar callouts

- **Pillar 1 (perf)**: no impact on frame work; token refresh and the listener run on worker threads. `JiraApiBase` is a string concat per request, same as today.
- **Pillar 2 (UI never blocks)**: the sign-in wait (up to a 3-minute listener timeout) and every refresh happen off the UI thread; the command reports progress through the existing command-result/toast path. `Bearer()` may block a *sync worker* on a refresh round-trip, never the UI thread (all Jira calls are already worker-side).
- **Pillar 3 (never crash)**: untrusted callback query and token JSON are size-capped and parsed via `ParseBounded`; no raw `new`; listener is RAII and always stopped on every exit path.
- **Pillar 4 (accessibility)**: no UI in this slice.
- **Pillar 6 (offline-first)**: a failed refresh with a Transport kind keeps cached data and retries later; only `invalid_grant` is surfaced as Auth. Error kinds are never flattened (`tracker-error-kind-collapsed`).

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — fires through the existing tracker-sync scenario; the changed path is header construction + base URL, both O(1).
2. **Pillar 2 static scanner** — N/A: no sync I/O reachable from `ImGui::*`; all network work is on workers.
3. **Dispatcher drain** — N/A: `MainThreadDispatcher::Drain()` untouched.
4. **Visible-cue bucket-E harness** — N/A: no new UI stall path (UI deferred).
5. **Marker inventory** — N/A: no `SMATCHET_UI_PERF_SCOPE` markers added.

## Risks / non-goals

- **Atlassian may not require a secret / may support PKCE now.** Evidence is a community thread plus an open feature request, not current docs (docs host unreachable from the build container). Mitigation: the secret is user-supplied config, so if PKCE becomes available it is additive. *Verify against the live Atlassian console before release.*
- **Callback URL matching.** Atlassian matches the registered URL exactly, so the port is fixed (configurable) rather than ephemeral; a busy port fails with a clear error. Whether the console accepts `http://localhost:<port>/callback` must be confirmed live; assumed yes.
- **Rotating refresh token loss.** A crash between receiving and persisting the new refresh token forces re-login. Mitigated by persisting before the old token is dropped; accepted otherwise.
- **Plaintext fallback off Windows** (README § Platform support): the refresh token and client secret inherit that existing limitation. Accepted; same as `ApiToken` today.
- **Loopback exposure.** Bound to `127.0.0.1`, one request, random `state` (≥128 bits), constant-time compare, closes after first valid callback or timeout, serves a static page, logs nothing from the query.
- Non-goals: multi-site OAuth (one cloudId; `JiraBackends[1+]` keep API-token auth), Jira Data Center/Server OAuth, other backends, shipping a bundled client secret, a relay server, the Preferences UI (follow-up).

## Verification

- **Bucket A (pure-logic, `ninja-test-linux`)**: `JiraOAuthPure.test.cpp` — authorize URL encoding/scopes, state mismatch/missing/error callbacks, token parse (missing fields, huge body, depth bomb), resource selection by domain, skew boundary, base URL; `JiraOAuthSession.test.cpp` — refresh single-flight, rotation persisted before use, `invalid_grant` → Auth, Transport failure keeps old session, with a fake HTTP seam.
- **Bucket E**: N/A this slice (no UI).
- **Sanitizer / build gate**: `cmake --build --preset ninja-test-linux`; `posix-core-check` compile gate; dual-target `SmatchetStandalone SmatchetCore_DX12` is **Windows-only and not runnable in this Linux container** — left to CI.
- **Lint**: `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop`.
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in § Deviations.
- **Manual residue**: the live Atlassian round trip (needs a real OAuth app + Jira Cloud site) cannot be automated here; tracked as a `tooling.md` entry — a credentialled smoke like `2026-08-03-credentialled-setup-smoke`.

## Out of scope (flagged, not designed)

**Deferral residue-sweep (keep this note)** — greps of `**/CONTEXT*.md`, `docs/adr/`, `agents/*.md`, `docs/self-improvement/categories/` for references to anything deferred here to be done before finalising.

- Preferences → Tracker UI for sign-in — follow-up plan after a visual check.
- Relay-based "one-click" sign-in — revisit if BYO setup friction proves too high.
- OAuth for GitHub/Linear/Plane — separate plans.

## Implementation log
*(populated post-ship)*

## Deviations from plan
*(populated post-ship)*

## Verification (actual)
*(populated post-ship)*

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
