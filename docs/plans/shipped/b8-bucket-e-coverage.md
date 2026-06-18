# Plan — B8: bucket-E coverage (post-verification re-scope)

> **Slug**: `b8-bucket-e-coverage`. Sub-plan of [`agentic-backlog-campaign.md`](../active/agentic-backlog-campaign.md) batch **B8**.
>
> **Status**: `shipped` (2026-06-18) — Phase 0 (#711/#715) + Phase 1 all five TUs landed: **L1 #1383, L2 #1372, L3 #1364, L4 #1382, L5 #1384**. Campaign complete; no Phase 2.
>
> Mandatory cross-link: `AGENTS.md` § Project rules § Plan-doc family, § Verification automation.

## Context

The campaign plan framed B8 as the **infra keystone** — three `--spawn`/bucket-E infra blockers (`spawn flake`, `SmatchetTests /EHsc`, `perf-run.sh` worktree handshake) gating ~10 deferred coverage entries. A pre-plan verification sweep (2026-06-02) against the tree shows **that framing is obsolete**: the infra is already unblocked, and most coverage dependents are already shipped or moot. B8 collapses to a small set of genuinely-live bucket-E authoring tasks plus a backlog archival.

Intended outcome: the bucket-E backlog reflects reality — stale/moot entries archived, and the handful of real coverage gaps either written or explicitly deferred with a tracked reason.

## Verification sweep (every B8-related entry, vs the tree)

**Infra "blockers" — all resolved:**

| Entry | Verdict | Evidence |
|---|---|---|
| infra P2 `--spawn` flake (2026-05-17) | **STALE** | Deterministic warmup gate shipped (slice 9 of `autonomous-debugging-no-creds`): `tests/ui/spawn_warmup_deterministic_gate.test.cpp` + `scripts/dev/test-ui-spawn-warmup-deterministic-gate.sh` whose header says *"Closes infra.md P2 line 16"*. Entry never archived. |
| test P2 `SmatchetTests` `/EHsc` local link (2026-05-31) | **FIXED** | Was real, not stale: on a newer MSVC toolset (VS18 / 14.38) the `ninja-test-msvc` cache lost the default `/EHsc` (empty `CMAKE_CXX_FLAGS`), and cpr's `-Werror` (`CMakeLists.txt:488`) turned the resulting `C4530` fatal → every doctest TU failed `static_assert "Exceptions are disabled"` (C2338). Pinned `/EHsc` explicitly + MSVC-guarded on `SmatchetTests` (`tests/CMakeLists.txt`) **and** `SmatchetLuaTests` (`tests/Lua/CMakeLists.txt`) so it's immune to cache state + flag pollution. Verified: fresh `ninja-test-msvc` builds both targets clean (582/582, no C2338/C4530) + the doctest rig runs. |
| tooling P2 `perf-run.sh` worktree handshake (2026-06-01) | **MOSTLY STALE** | File-result mode exists: `scenario.run --outPath` + `WaitForFile` poll (`CliCommandRunner.cpp:285,852`); `perf-run.sh` uses `--outPath`. Residual is narrow (scenario *completion* in a headless worktree-GL sandbox), not the missing file-result mode the entry asked for. |

**Coverage dependents — mostly shipped or moot:**

| Entry | Verdict | Evidence |
|---|---|---|
| test:70 AI Prefs batch 1+2 (6 flows) | **STALE** | 6× `tests/ui/ai_assistant_preferences_*.test.cpp` shipped (docking, enter_send, save_discard, test_connection, validation_banner, verify_on_save). |
| test:46 description grid-cell tooltip markdown | **STALE** | `tests/ui/description_tooltip_markdown_render.test.cpp` shipped. |
| test:52 AgentProposalStore SQLite bucket-E lane | **MOOT** | `AgentProposalStore` removed from the tree (agentic runtime deleted, per `applied.md` banner). No store → no lane needed. |
| tooling:87 Preferences Agentic tab (T7) | **MOOT** | Agentic tab/poll UI removed — only a vestigial `ConfigManager` field + a localization string remain; no panel to drive. |
| tooling:73 coderabbit-react-loop live-PR probe | **MOOT** | React-loop runtime deleted (`applied.md` deleted-runtime banner); only a leftover config field remains. |

**Genuinely LIVE (the real B8 work):**

| # | Entry | What's missing |
|---|---|---|
| L1 | test:118 Command Palette inline typing (2026-05-15) | No `command_palette_inline_typing.test.cpp`; the inline menu-bar palette → modal-filter path is manual-verified only. |
| L2 | tooling:80 DeepSeek "[model changed - chat cleared]" strip (2026-05-20) | No TU exercises the rendered orange strip after a Send-with-different-model. |
| L3 | tooling:101 tooltip-content-identity helper (2026-05-17) | `BucketE::TooltipContentMatches(ctx, sentinel)` helper not added; `callstack_tooltip_hover.test.cpp` uses the replica-flag workaround and dropped a production-driven variant. |
| L4 | test:22 slice-9 `VerifyOnSave_TestConnection_SetsResult` flake (2026-05-24) | The variant exists in `ai_prefs_autosave_flow.test.cpp:216` (downgraded to "informational"), but still uses a 240-yield poll instead of a deterministic dispatcher wait → flake-prone. |
| L5 | tooling:300 memory-hardening residue (2026-05-31) | The bucket-E "loading thumbnails" cue — a passive `TextDisabled` driven off `pendingThumbnailUploads` — has no test. **In B8 scope** (pulled in per decision). The sibling ASan live-swap + S4 bounded-read residues stay on tooling:300 (sanitizer/unit, not bucket-E). |

## Approach

Two phases, one PR each (or one combined PR if the live set stays small).

**Phase 0 — archive the resolved/moot entries (pure-docs).** Archive the 3 infra entries + the 5 stale/moot coverage entries (8 total) to `applied.md` with per-entry tree evidence; `--fix` counts. This is the bulk of B8's backlog impact and is risk-free. The `perf-run.sh` entry archives with a residual note pointing at the (separate, low-priority) worktree-GL-completion gap rather than claiming full closure.

**Phase 1 — author the live bucket-E TUs (L1, L2, L4; L3 as shared helper).** Each new TU mirrors the established scaffold: register via `IM_REGISTER_TEST` in `tests/ui/<name>.test.cpp`, wire into `tests/ui/ui_tests_registry.cpp` + `tests/ui/CMakeLists.txt`, add a `scripts/dev/test-ui-<name>.sh` driver, build with `cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone`, run via the `--spawn` ephemeral instance. The warmup gate (now shipped) makes these runs deterministic — the prior flake rationale for deferring is gone.

- **L4 first (cheapest, highest-confidence)** — replace the 240-yield poll in the existing `VerifyOnSave_TestConnection_SetsResult` with a deterministic wait: drive `app.MainThreadDispatcher().Drain()` after the worker join (verified: the API is `Drain()`, FIFO-until-budget — there is no `DrainOnce()`), or arm a deterministic post-condition before `TriggerProbe`. Promotes a flaky informational variant to a real assertion. ~1 h.
- **L3 — shared `BucketE::TooltipContentMatches(ctx, sentinel)` helper** in `tests/ui/_helpers/` (walk the tooltip window's `DrawList` `CmdBuffer` for a text command containing `sentinel`). Unblocks production-driven hover assertions; retrofit `callstack_tooltip_hover.test.cpp` variant 4. ~2 h. Do before L1/L2 if either needs content-identity.
- **L1 — `command_palette_inline_typing.test.cpp`** — minimal `CommandRegistry` with 1-2 synthetic commands; `ItemInput` into the inline menu-bar palette; assert the filter state updates pre-Enter. **Verified: `CommandPaletteUi` exposes `SetFilterText()` but NO getter** — L1 adds a `const char* FilterText() const { return filterBuf_; }` accessor to `Source/Core/include/Commands/CommandPaletteUi.h`. That makes L1 the one Phase-1 item touching a `Source/Core/` header → its PR needs the **dual-target** build (`SmatchetStandalone SmatchetCore_DX12`), not test-only. ~2 h.
- **L2 — `ai_assistant_model_change_strip.test.cpp`** — seed `g_ui.assistantHistory` with one stub message; flip `cfg.AiProviderKind`; drive a synthetic Send through `AiClientFactory::SetTestOverride` (the test seam already exists — see infra applied entry); assert the strip renders `"[model changed - chat cleared]"` after the second turn. ~2 h.
- **L5 — `attachment_thumbnail_loading_cue.test.cpp`** — open the attachment preview with `pendingThumbnailUploads` non-empty; assert the passive cue renders. **Verified: the cue exists** at `SmatchetAttachmentPreviewUi.cpp:643` — `ImGui::TextDisabled(SmatchetLocalization::T("attachment.thumbnails.loading", "loading thumbnails..."))`, gated on the `pendingThumbnailUploads` count. Assert via `BucketE::TooltipContentMatches`-style DrawList scan for the localized string (or the `attachment.thumbnails.loading` key), and that it clears once pending drains. **Open seam check**: confirm a deterministic way to hold `pendingThumbnailUploads` non-empty for ≥1 frame (seed the count directly via `g_ui`/preview state, or an artificially-slow decode hook); if neither is cleanly drivable, fall back to a unit assertion on the gating predicate + leave the rendered-cue as the one deferred item. ~1.5 h.

## Files to modify

**Phase 0 (docs):**
1. `docs/self-improvement/categories/{infra,test,tooling}.md` — remove the 8 stale/moot entries.
2. `docs/self-improvement/categories/applied.md` — 8 resolution lines; `AGENT_SELF_IMPROVEMENT.md` § Index — `--fix`.

**Phase 1 (tests only — no `Source/` production change expected; L1 may add one accessor):**
3. `tests/ui/_helpers/BucketETooltip.h` (new) — `TooltipContentMatches` (L3).
4. `tests/ui/command_palette_inline_typing.test.cpp` (new, L1) + `scripts/dev/test-ui-command-palette-inline-typing.sh`.
5. `tests/ui/ai_assistant_model_change_strip.test.cpp` (new, L2) + `scripts/dev/test-ui-ai-assistant-model-change.sh`.
6. `tests/ui/ai_prefs_autosave_flow.test.cpp` (edit, L4) — deterministic wait.
7. `tests/ui/callstack_tooltip_hover.test.cpp` (edit, L3 retrofit) — production-driven variant 4.
8. `tests/ui/attachment_thumbnail_loading_cue.test.cpp` (new, L5) + `scripts/dev/test-ui-attachment-thumbnail-cue.sh`.
9. `tests/ui/ui_tests_registry.cpp` + `tests/ui/CMakeLists.txt` (explicit per-TU source list, verified lines 20+) — register the 3 new TUs.
10. `Source/Core/include/Commands/CommandPaletteUi.h` (L1, confirmed) — add `const char* FilterText() const { return filterBuf_; }` getter (the class has `SetFilterText` but no reader). Only production-header touch in B8.

## Existing utilities reused

- `tests/ui/views_columns_reorder.test.cpp` — canonical bucket-E scaffold shape.
- `tests/ui/spawn_warmup_deterministic_gate.test.cpp` — proof the warmup gate is wired; reference for deterministic frame-gating.
- `AiClientFactory::SetTestOverride` (shipped per infra applied 2026-05-18) — stub `IAiClient` for L2 without live HTTP.
- `scenario.run --outPath` + `WaitForFile` (`CliCommandRunner.cpp`) — file-result harness (already present).
- `ninja-ui-test-msvc` preset (`CMakePresets.json:105`) — bucket-E build/run.

## UX Pillar callouts

- **Pillar 1/2/3**: N/A — test-only (Phase 1) + docs (Phase 0). No production runtime path changes (L1's possible `FilterText()` accessor is a const getter).
- **Pillar 4**: N/A.

## Perf-review-system gates

*(This is the plan template's `§ Perf-gate` contract section — the canonical heading per `docs/plans/active/_plan-template.md`. It covers the one `Source/Core/` touch in this plan.)*

**§ Perf-gate** — **N/A (no hot-path impact)**. The only `Source/Core/` change is L1 adding a `FilterText()` const getter to `CommandPaletteUi.h` — a one-line accessor returning `filterBuf_`, zero runtime cost, not on any per-frame/render path. No `SMATCHET_UI_PERF_SCOPE` added, no scenario in the curated diff→scenario map exercises it. It triggers the dual-target **build** gate (above) but no perf-scenario gate. Owner: orchestrator.

## Decisions (locked 2026-06-02)

1. **Phase 1 scope = all five** — L1 (command palette) + L2 (model-change strip) + L3 (tooltip-content helper) + L4 (deterministic VerifyOnSave) **+ L5** (memory-hardening "loading thumbnails" cue, pulled in per decision 2). Order: L3 helper → L4 → L1 → L2 → L5.
2. **L5 pulled into B8** — author the loading-thumbnails bucket-E cue here. Its sibling residues on tooling:300 (ASan live-swap, S4 bounded-read) stay on that entry (sanitizer/unit, not bucket-E); only the bucket-E cue moves to B8.
3. **Split PRs** — Phase 0 (archival) ships as its own risk-free pure-docs PR immediately; Phase 1 (TUs) ships separately after bucket-E build + run iterations.

## Risks / non-goals

- **Residual `--spawn` flake** — the warmup gate reduced but may not have eliminated intermittency. Mitigation: author against the gate; if a new TU flakes, that's a real gate regression to file, not a reason to defer. Re-confirm the gate holds by running the new TUs ~4× back-to-back (the old repro recipe).
- **Worktree-GL completion (perf-run residual)** — NOT a B8 deliverable; bucket-E TUs run from the main checkout, not worktree-isolated subagents. Noted so Phase 0's perf-run archival carries the residual honestly.
- **Non-goal**: redesigning the bucket-E harness, a SQLite bucket-E lane (moot — store removed), Agentic-tab / react-loop coverage (moot — features removed).
- **Non-goal**: the ASan live-swap + S4 bounded-read residues (tooling:300) — sanitizer/unit work, not bucket-E.

## Verification

- **Bucket E**: each new/edited TU runs green via its `scripts/dev/test-ui-*.sh` on `ninja-ui-test-msvc`, ~4× back-to-back to confirm non-flaky.
  - **L5** — `scripts/dev/test-ui-attachment-thumbnail-loading-cue.sh` drives `tests/ui/attachment_thumbnail_loading_cue.test.cpp` (`Attachment/ThumbnailLoadingCue_VisibleWhileLoading`).
  - **L3** driver: `scripts/dev/test-ui-callstack-tooltip-hover.sh` (covers all 4 variants incl. the re-enabled production-driven `CallstackTooltipHover_Production_ContentIdentity`). Verified 4× green, 0 flake (2026-06-17). Helper: `tests/ui/_helpers/BucketETooltip.h`.
- **Bucket A**: none (no new pure helpers).
- **Build gate**: `cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone` (bucket-E build). **L1 confirmed adds a `CommandPaletteUi.h` accessor → its PR runs dual-target** `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`. L2/L3/L4/L5 are test-only (no dual-target needed).
- **Phase 0**: `test-backlog-counts.sh` 8/0; pure-docs.
- **Manual residue**: none intended — the whole point is converting manual-verify gaps to bucket-E. Any item that can't be made deterministic stays a tracked entry with a concrete reason (no silent drop).

## Out of scope (flagged, not designed)

- `perf-run.sh` worktree-GL scenario completion — separate, low-priority; bucket-E doesn't need it.
- Coverage-threshold advisory→blocking flip — time-gated, unrelated.

## Implementation log

- #711 · **Phase 0** — archived the 8 stale/moot bucket-E entries (3 infra "blockers" + 5 coverage dependents) to `applied.md` after re-verifying against current develop; count index synced (test 19→15, applied 173→181, 8/0). Pure-docs. Merged.
- **Phase 1 L1** — `command_palette_inline_typing.test.cpp` authored + builds clean on `feat/b8-l1-command-palette-typing` (adds `CommandPaletteUi::FilterText()` getter). **Held, not shipped** — see Deviations.
- **Phase 1 L5** — `tests/ui/attachment_thumbnail_loading_cue.test.cpp` + `scripts/dev/test-ui-attachment-thumbnail-loading-cue.sh` authored on `b8-l5-thumbnail-cue`. Drives the **real** production gate: seeds `smatchet::memtel::PendingThumbnailUploads()` (the live global atomic the production cue at `SmatchetAttachmentPreviewUi.cpp:653-659` reads) > 0, asserts the "loading thumbnails..." cue renders, then drains to 0 and asserts it clears. Macro pre-check: `SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS` is an unconditional `#define` (`:26`, inside `#if defined(_WIN32)`), so it IS live in the MSVC bucket-E build — the cue compiles in, not vacuously green. Test-only (no `Source/Core/` change). Built clean on `ninja-ui-test-msvc`; driver 4/4 green back-to-back (ports 58761/63/65/67), zero flake. Lint + shellcheck clean. Merged #1384.
- **Phase 1 L1 (re-authored 2026-06-18)** — prior L1 branch was lost (see Deviations); re-authored from scratch on `b8-l1-palette-typing`. Two TUs under category `CommandPalette` (`InlineTyping_NarrowsFilterPreEnter`, `InlineTyping_PreseedThenTypeAppends`) using the `GuiFunc`-replica idiom (TU-owned `CommandPaletteUi` drawn against the live `AppController` registry — no new SmatchetUI accessor needed). Production change is exactly the planned `CommandPaletteUi::FilterText()` const getter. Driver: `scripts/dev/test-ui-command-palette-inline-typing.sh`. **Verified ×4 clean (ports 58814-58817), 2/2 every run, zero flake.** Dual-target build green (Standalone exe + `SmatchetCore_DX12.lib`). Lint + shellcheck clean. Shipped #1383.
- **Phase 1 L3** — shipped the shared content-identity helper `tests/ui/_helpers/BucketETooltip.h` (`BucketE::TooltipContentMatches(ctx, sentinel)` + `EmitTooltipContentMarker` / `FillTooltipMarkerPayload`) and RE-ENABLED variant 4 of `callstack_tooltip_hover.test.cpp` as the production-driven `CallstackTooltipHover_Production_ContentIdentity` test. Driver: `scripts/dev/test-ui-callstack-tooltip-hover.sh`. Built clean on `ninja-ui-test-msvc`; ran 4× back-to-back green (4/4 passed each run, 0 flake). Lint clean. **Mechanism note:** the plan's literal "walk CmdBuffer for a text command" is infeasible on ImGui 1.92.8 (ImDrawCmd retains only rasterised glyph geometry, no source string; no Test-Engine text-capture API) — implemented instead via a DrawList `UserCallback` sentinel marker (size==0 form, caller-owned persistent payload) that lives in the tooltip window's own drawlist, giving the content-identity property variant 4 needed. See the BucketETooltip.h header comment for the full rationale.

## Deviations from plan

- **Phase-0 over-archived the spawn-flake entry — re-filed.** Phase 0 archived the `--spawn` flake as stale ("warmup gate closed it"). During L1 authoring the bucket-E harness flaked ~80%; isolating with the existing `Views` TU (1/6 green under the same path) proved the flake is **harness-wide, not eliminated** — only reduced + tolerated-with-retries. Re-filed as a live 2026-06-02 infra P2 (warmup gate reduced-not-removed; empty-child-log diagnosability gap). The other two infra archivals (/EHsc, perf-run file-result) still hold.
- **L1 held pending a verifiable harness.** L1 passes intermittently exactly like the 15 shipped bucket-E TUs, but a clean ×4 local verify isn't achievable under this session's machine load, so per the #707 "don't ship unverified tests" rule it stays on its branch until the harness is idle/fixed. L2-L5 not started.
- **L1 branch lost (stale ref).** The held L1 work lived on `feat/b8-l1-command-palette-typing`, which **no longer exists** (neither local nor remote as of 2026-06-15) — the held TU + `CommandPaletteUi::FilterText()` accessor appear lost/unrecovered and would need re-authoring to land L1. Only Phase 0 backlog-archival actually shipped (#711/#715); L2-L5 were never started.
- **tooling:300 S4 bounded-read residue is now DONE** — develop `#657` (memory-budget hardening) shipped `ImageDimensionsPure.{h,cpp}` + `tests/Core/ParseImageDimensions.test.cpp`, closing the S4 bounded-read unit test. So L5's scope narrows to the rendered "loading thumbnails" cue only; the tooling:300 entry retains just its ASan live-swap residue (not bucket-E).
- Re-verified all Phase-0 verdicts on current develop (post #701/#703/#704-707/#657) before archiving — the coverage-dependent ones held; only the spawn-flake infra one was wrong (above).

## Verification (actual)

- Phase 0: pure-docs. `test-backlog-counts.sh` 8/0. (Phase 1 verification populated when its PR ships.)
