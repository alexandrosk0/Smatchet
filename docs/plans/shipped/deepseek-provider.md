# DeepSeek first-class provider + auto-clear chat on model change
<!-- plan-date: 2026-05-19 -->

## Goal

Two shippable features in one squash-merged PR:

- **F1**: Add `AiProvider::DeepSeek = 4` modelled exactly on the Anthropic shape — enum, factory routing (uses `OpenAiClient` since DeepSeek's wire is OpenAI-compatible), persisted config (DPAPI-encrypted key + legacy-plaintext migration + base URL + model), Preferences UI section, test-connection branch, validator branch, model catalog (`deepseek-chat`, `deepseek-reasoner`), and tests. Default base URL when blank = `https://api.deepseek.com`.
- **F2**: Auto-clear `g_ui.assistantHistory` + `assistantStreamBuf` + `assistantLastError` whenever the effective provider OR effective model identifier changes between turns. First-turn (empty cache) is not a "change". Reasoning-effort-only changes are not a "change". Per-turn model overrides (`req.ModelOverride`) compose into the signature.

Scope note: schema-version is NOT bumped — every new JSON key is additive optional, hydrated via `j.value(..., default)`.

## Background

DeepSeek serves an OpenAI-compatible API at `https://api.deepseek.com/v1/chat/completions` with models `deepseek-chat` and `deepseek-reasoner`. Today users can only reach it via the generic OpenAI provider, but the model dropdown (`Source_Core/src/AiModelCatalog.cpp` line 9-22) hardcodes OpenAI IDs only — so the user picks `gpt-4o`, DeepSeek returns HTTP 400 "Model not exist", failure mode is `Failed: chat: HTTP 400`. They can work around it via the "Custom model ID (advanced)" collapsing header but it's hidden.

## Affected components

| Component | Path | Change shape |
|---|---|---|
| Provider enum | `Source_Core/include/AiTypes.h` | Add `DeepSeek = 4` to `AiProvider` enum |
| Model catalog | `Source_Core/src/AiModelCatalog.cpp` | New `case AiProvider::DeepSeek:` returning `deepseek-chat` + `deepseek-reasoner` |
| Factory | `Source_Core/src/AiClientFactory.cpp` | 4 sites — `MakeAiClient` (returns OpenAiClient), `ProviderToString` ("deepseek"), `ProviderFromString`, `EnumeratedProviders` (display "DeepSeek") |
| Config struct | `Source_Core/include/ConfigManager.h` | New fields `AiDeepSeekApiKey`, `AiDeepSeekBaseUrl`, `AiModelDeepSeek = "deepseek-chat"`; enum-range clamp update (line 666: `kProviderMax` → `AiProvider::DeepSeek`) |
| Config save/load | `Source_Core/src/ConfigManager.cpp` | Save: encrypt + erase legacy on Win32, plaintext on non-Win32; Load: decrypt + legacy fallback + migration flag; field-array load for `ai_deepseek_*` keys; extend `migrateAny` boolean + `LOG_INFO` format string |
| Validator field keys | `Source_Core/include/AiPrefsValidator.h` | Add `kAiDeepSeekApiKey`, `kAiModelDeepSeek` constants |
| Validator logic | `Source_Core/src/AiPrefsValidator.cpp` | `ClampProvider` case 4 → DeepSeek; key-required branch; model branch; format sniff (DeepSeek keys start with `sk-`) |
| Controller config build | `Source_Core/src/AiAssistantController.cpp` | `BuildClientConfig` switch arm: `ApiKey = SanitizeHeaderValue(cfg.AiDeepSeekApiKey)`, `BaseUrl = SanitizeBaseUrlOrLog(...)` with fallback literal `"https://api.deepseek.com"` when raw empty; `ProviderFromConfig` case 4 → DeepSeek; `RunRequest` model picker case → `cfg.AiModelDeepSeek` |
| Controller model-change clear (F2) | `Source_Core/src/AiAssistantController.cpp` + header | Cached `lastModelSignature_` member; signature check in `RunRequest` after model resolution; clear via `MainThreadDispatcher` only on real change |
| Pure helper (F2) | `Source_Core/include/AiModelSignature.h` + `Source_Core/src/AiModelSignature.cpp` | `DetectModelChange(prev, provider, model) -> {ShouldClear, NewSignature}` for unit testability |
| Test-connection probe | `Source_Core/src/AiPrefsTestConnection.cpp` | Provider switch arm: pick `AiDeepSeekApiKey` + `AiDeepSeekBaseUrl` + `AiModelDeepSeek`; URL fallback `https://api.deepseek.com` |
| Preferences UI | `Source_Core/src/SmatchetPreferencesUi.cpp` | New static buffers `s_deepseekKeyBuf` + `s_deepseekModelBuf` + `s_deepseekBaseUrlBuf`; seed/reseed block extension; provider-arm `else if (selectedKind == AiProvider::DeepSeek)` block rendering key/model/base-URL inputs |
| Agentic inference | `Source_Core/src/AgenticInferenceClient.cpp` | DeepSeek arms (key = AiDeepSeekApiKey, baseUrl = AiDeepSeekBaseUrl, model = AiModelDeepSeek) |
| Lua AI command | `Source_Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp` | DeepSeek arms in `AiProviderDisplayName`, `BuildClientConfigForProvider` |
| Tests — factory | `tests/Source_Core/AiClientFactory.test.cpp` | Round-trip + enumeration cover DeepSeek |
| Tests — catalog | `tests/Source_Core/AiModelCatalog.test.cpp` | DeepSeek catalog assertions |
| Tests — validator | `tests/Source_Core/AiPrefsValidator.test.cpp` | DeepSeek branches |
| Tests — config migration | `tests/Source_Core/ConfigMigration.test.cpp` | Round-trip new fields |
| Tests — chat clear (F2) | `tests/Source_Core/AiModelSignature.test.cpp` (new) | Pure helper coverage |

## Decisions (open questions resolved)

- **Q1 — separate `AiDeepSeekBaseUrl`**: YES. Per spec; preserves per-provider URL across switches.
- **Q2 — clear on base-URL-only edit**: NO. Endpoint changes typically mean same-model-different-region; clearing would surprise users.
- **Q3 — clear notice surface**: `g_ui.assistantLastError` string `"[model changed — chat cleared]"`. Less disruptive than synthetic history message.

## Risks

- **R1 — Duplicated Test-Connection logic**: `AiPrefsTestConnection.cpp` and `SmatchetPreferencesUi.cpp::runProbe` are near-identical clones (~160 LOC dup). Adding DeepSeek means both get a new switch arm. Out of scope to unify in this PR.
- **R2 — Many clamp/switch sites**: ~22 distinct switch sites per architect inventory. Missing any one = silent fallback to OpenAI for DeepSeek users.
- **R3 — Dual-target compile**: Controller change is inside `#if defined(SMATCHET_WITH_AI)` (standalone only). ConfigManager new fields are unconditional (safe — plain std::strings). `AgenticInferenceClient` is `#if defined(SMATCHET_WITH_AGENTIC)` — standalone-only.
- **R4 — Wire format**: DeepSeek `/v1/chat/completions` is OpenAI-protocol-compatible (model, stream, messages, max_tokens, etc.). `reasoning_effort` accepted by `deepseek-reasoner`. ProbeReachability `/v1/models` supported.
- **R5 — F2 race**: Cancel + immediate Send-with-different-model could land a stale `[model changed]` strip. Acceptable; transient.
- **R6 — F2 first turn**: empty `lastModelSignature_` correctly skips first-run clear.

## Verification plan

| Bucket | Coverage |
|---|---|
| A — doctest | `EnumeratedProviders` returns 5 entries; round-trip `"deepseek"` ↔ enum; `KnownModels(DeepSeek)` returns 2 IDs; validator branches; config round-trip with new keys; `DetectModelChange` pure helper across 5 scenarios |
| D — sanitizer | `ninja-test-msvc` ctest under ASan/UBSan via `bash scripts/dev/test-all.sh` |
| E — ImGui Test Engine | Deferred — flagged to `docs/self-improvement/categories/tooling.md`. Pure-helper bucket-A covers load-bearing F2 logic; bucket-E only adds rendered-strip verification. |

## Implementation log

- `25491330` · `wip(plan): deepseek-provider` — plan doc committed pre-implementation to guarantee plan-doc safety across the spawned-handoff session.
- `593a1f83` · `feat(ai): add DeepSeek provider + auto-clear chat on model change` — F1 + F2 implemented in a single squash-ready commit (21 files changed, +572 / −145).
  - F1: `AiProvider::DeepSeek = 4`; factory routes to `OpenAiClient`; catalog seeds `deepseek-chat` + `deepseek-reasoner`; new `TrackerConfig` fields `AiDeepSeekApiKey` (DPAPI-encrypted on Win32, legacy-plaintext migration) + `AiDeepSeekBaseUrl` + `AiModelDeepSeek = "deepseek-chat"`; validator branch (key-required / catalog-known / `sk-` sniff); controller `BuildClientConfig` arm with empty-URL fallback to `https://api.deepseek.com` (always through `SanitizeAiEndpointUrl`); `AiPrefsTestConnection` + `SmatchetPreferencesUi::runProbe` parallel arms; per-provider UI render with key / model picker / base URL inputs; `AgenticInferenceClient` (4 sites: provider clamp + URL + key + model) and `BuiltinCommands_Ai` Lua glue (display + client config + model) parity.
  - F2: new pure helper `Source_Core/{include,src}/AiModelSignature.{h,cpp}` returning `{ShouldClear, NewSignature}`; controller caches `lastModelSignature_`, runs the helper in `RunRequest` after model resolution, posts a `MainThreadDispatcher` clear of `g_ui.assistantHistory` + `assistantStreamBuf` + sets `assistantLastError = "[model changed - chat cleared]"` when the signature changes. Posted lambda captures nothing by reference (worker-local-to-UI UAF avoided).
  - Tests: 18 new doctest cases (AiModelSignature 6, AiClientFactory DeepSeek round-trip + factory, AiModelCatalog DeepSeek seed + IsKnownModel cross-misses, AiPrefsValidator DeepSeek 6 branches, ConfigMigration DeepSeek round-trip + plaintext migration). 168 assertions across the 34-case selector pass.

## Deviations from plan

- Pre-existing style-lint cleanups applied opportunistically (cppcheck `useStlAlgorithm` warnings) in files I edited for DeepSeek: `Source_Core/src/AiModelCatalog.cpp::IsKnownModel`, `Source_Core/src/SmatchetPreferencesUi.cpp` Whisper label loop, `tests/Source_Core/AiModelCatalog.test.cpp::ContainsId`, `tests/Source_Core/AiPrefsValidator.test.cpp::ContainsSubstring`. The pre-commit lint hook blocks any further edit on the file until the lint warnings clear — these were collateral cleanup, not plan deviations. Marked here for traceability.
- ConfigMigration round-trip test for DeepSeek lives in `ConfigMigration.test.cpp` (per plan); the legacy-plaintext migration test is `#if defined(_WIN32)` only because the `_enc` path itself is Win32-only (matches the existing AiAnthropicApiKey pattern even though no Anthropic round-trip test existed yet — the DeepSeek pair establishes the template).
- Bucket-E (ImGui Test Engine) rendered-strip verification of the `"[model changed - chat cleared]"` warning banner deferred per plan § Verification. Logged to [`docs/self-improvement/categories/tooling.md`](../../self-improvement/categories/tooling.md) (2026-05-19 entry under `## Parked`, P3, owner `handoff-implementer`).

## Verification

- **Standalone build**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone` — clean, `Smatchet.exe` linked.
- **DX12 (Unreal) target**: builds 432/651 TUs then halts on a **pre-existing, unrelated** failure in `Source_Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp` (`g_ui.assistantPanelOpen` referenced unconditionally from a `SMATCHET_WITH_WHISPER`-gated TU; the field is declared inside `#if defined(SMATCHET_WITH_AI)`, but the DX12 build defines `SMATCHET_WITH_WHISPER=1` without `SMATCHET_WITH_AI=1`). Reproduced on `develop` without these changes (stash + rebuild) — same diagnostic on lines 112, 113, 309. Not introduced by this slice; surfaced because it lives within ~20 TUs of where my AI changes compile. Out of scope for this PR; tracked separately for a Whisper-side fix.
- **doctest rig**: `cmake --build --preset ninja-test-msvc --target SmatchetTests` builds clean; full ctest reports **757 passed, 7 failed, 0 skipped** (4224 assertions). The 7 failures are all `AgentProposalStore` SQLite ":memory:" `unable to open database file` errors — pre-existing on `develop` (verified with stash baseline, same 746/739/7 vs my 764/757/7). None of the 18 new tests added by this slice fail.
- **Targeted selector** `SmatchetTests.exe --test-case="*DeepSeek*,*AiModelSignature*,AiClientFactory*,AiModelCatalog*,*model change*,ConfigMigration DeepSeek*"`: **34 cases, 168 assertions, all pass**.
- **Bucket-E**: pending — rig is wired (`docs/plans/shipped/imgui-test-engine-bucket-e-execution.md`); rendered-strip test promoted to live P2 in `docs/self-improvement/categories/tooling.md` (2026-05-20 — DeepSeek auto-clear strip).
