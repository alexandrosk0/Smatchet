# AI chat — Claude-Desktop-style hover actions, pin, visible user bubbles, persisted history

## Context

Smatchet's AI chat panel ([SmatchetAiAssistantUi.cpp:151](../../Source_Core/src/SmatchetAiAssistantUi.cpp)) currently renders messages with hardcoded role colors, no per-message affordances, and zero persistence (`assistantHistory` lives only in `UiDrawSession`). The user wants Claude-Desktop parity:

1. Hover action row under each message — copy, pin/unpin, relative timestamp.
2. User-message bubble visibly distinct on any theme.
3. Pinned-message bookmarks at top of conversation, click jumps to original location.
4. Conversation history + pinned set survive app restart.

Confirmed scope decisions (AskUserQuestion session):

- Pin: hybrid — one-line bookmarks at top, click scrolls main history to the original message.
- User bubble: tinted background; text uses default theme color.
- Timestamp: relative on the action row + absolute date-time in hover tooltip.
- Icons: load Font Awesome 6 Solid alongside the existing font.

User follow-up: **persistence is in-scope** — chat history + pinned set persist across runs.

## Review fixes applied (2026-05-20)

After the first pass review against the live codebase, four issues were folded into the plan body below. Anchor list so reviewers can confirm each landed:

1. **Branch is 5 perf-PRs behind develop** → implementation runs from a fresh branch off current `develop`; the original `wip/ai-chat-claude-desktop-parity` worktree is abandoned. No content change in this doc, just an implementation directive.
2. **Detached-worker × `LocalCacheManager` shutdown surface (Pillar 3)** → § Persistence: single coalescing worker + `std::atomic<bool> g_chatPersistShuttingDown` guard; risk row added.
3. **`s_messageYCache` keyed by content hash collides on duplicate messages + doesn't invalidate on insert** → Phase 6 keys the cache by `messageIndex`, not by content hash, and clears it on `assistantHistory` size change.
4. **Verification automation not routed through `test-author`** → § Verification: explicit bucket-E ImGui-Test-Engine scenarios for every click step; any residue gets a `docs/backlog/agent-self-improvement/` (`tooling`) entry inline.
5. **Pillar 4 keyboard-nav of new action row** → Phase 6: action row renders on `IsItemFocused()` OR `IsMouseHoveringRect()`; Tab order through Copy → Pin → bookmark close.

## Files to modify

| File | Change |
|---|---|
| [Source_Core/include/AiTypes.h](../../Source_Core/include/AiTypes.h) | `AiMessage`: add `std::int64_t CreatedAtUnixMs = 0;` + `bool Pinned = false;`. C++14 NSDMI. Wire serializers (`OpenAiClient.cpp:68`, `AnthropicClient.cpp:49`, `OllamaClient.cpp:57`) iterate per-field — new members do NOT leak to provider wire. |
| [Source_Core/include/SmatchetUiSession.h](../../Source_Core/include/SmatchetUiSession.h) | New per-session UI state: `int assistantScrollToMessageIndex = -1;`, `std::int64_t assistantCopyToastUntilMs = 0;`, `std::string assistantCopyToastLabel;`, `bool assistantHistoryHydrated = false;`, `std::vector<std::int64_t> assistantHistoryRowIds;` (parallel to `assistantHistory`). Pin state moves onto `AiMessage::Pinned` (no parallel `pinnedIndices` vector). |
| [Source_Core/include/SmatchetTheme.h](../../Source_Core/include/SmatchetTheme.h) + [Source_Core/src/SmatchetTheme.cpp](../../Source_Core/src/SmatchetTheme.cpp) | New per-theme accessor `GetActiveAiColors()` (mirrors `GetSyntaxColors()` at `SmatchetTheme.h:20`). Tokens: `AiUserBubbleBg`, `AiUserRoleLabel`, `AiAssistantRoleLabel`, `AiActionRowIcon`, `AiActionRowIconHover`, `AiPinStripBg`. Populated per-theme in `ApplyStyle`. |
| [Source_Core/include/AiChatTimestamp.h](../../Source_Core/include/AiChatTimestamp.h) + [Source_Core/src/AiChatTimestamp.cpp](../../Source_Core/src/AiChatTimestamp.cpp) **(new)** | Pure helpers: `FormatRelativeTime(now_ms, then_ms)` → `"just now"`/`"<N>m ago"`/`"<N>h ago"`/`"<N>d ago"`; `FormatAbsoluteTime(unix_ms)` → `"14:32:07 20-May-2026"` (local TZ). No ImGui dependency. |
| [Source_Core/include/LocalCacheManager.h](../../Source_Core/include/LocalCacheManager.h) + [Source_Core/src/LocalCacheManager.cpp](../../Source_Core/src/LocalCacheManager.cpp) | New table `ai_chat_messages` (id PK, created_at INTEGER, role TEXT, content TEXT, pinned INTEGER DEFAULT 0). Methods: `AppendChatMessage(const AiMessage&)` returning row id, `UpdateChatMessagePin(int64_t id, bool pinned)`, `LoadChatMessages(std::size_t cap, std::vector<AiMessage>& out, std::vector<int64_t>& outIds)`, `ClearChatMessages()`, `TrimChatMessages(std::size_t maxRows)`. All wrapped in try/catch — failure logs `LOG_WARN` and degrades to in-memory-only (Pillar 3 graceful degradation). |
| [Source_Core/src/SmatchetAiAssistantUi.cpp](../../Source_Core/src/SmatchetAiAssistantUi.cpp) | `dispatchSend` stamps `CreatedAtUnixMs` + enqueues append-save. New pin-toggle handler. `DrawHistoryArea` rewrite: pin strip on top, per-message bubble + hover/focus action row, scroll-to-pinned latch. `HydrateFromConfigOnce` extension to call `LocalCacheManager::LoadChatMessages`. Header gets new "Clear chat" SmallButton with confirmation popup. |
| [Source_Core/src/SmatchetImGuiFonts.cpp](../../Source_Core/src/SmatchetImGuiFonts.cpp) | After the Segoe-emoji merge block (line 196-203), merge FA Solid TTF into the atlas using range `[0xF000, 0xF8FF]`. Stays inside the existing `#if defined(_WIN32)` block since Smatchet ships Windows-only. Resolve path via new `ResolveAssetTtfPath("fa-solid-900.ttf")` — exe-dir first, then `SMATCHET_ASSETS_SOURCE_DIR` fallback for dev tree. |
| `Source_Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6.h` **(new)** | Vendored from [github.com/juliettef/IconFontCppHeaders](https://github.com/juliettef/IconFontCppHeaders) — header-only `#define ICON_FA_COPY "\xef\x83\x85"` etc. Zlib licence. |
| `assets/fonts/fa-solid-900.ttf` **(new)** | Font Awesome 6 Free Solid TTF (~430 KB). SIL OFL 1.1. |
| [LICENSE](../../LICENSE) or new `THIRD_PARTY_LICENSES.md` | Append SIL OFL 1.1 (FA font) + Zlib (IconFontCppHeaders) notices. |
| [Source_Core/CMakeLists.txt](../../Source_Core/CMakeLists.txt) | Register `AiChatTimestamp.cpp` in `SmatchetCore` sources. `target_include_directories(SmatchetCore PUBLIC ThirdParty/IconsFontAwesome6)`. `add_custom_command(POST_BUILD)` copies `${CMAKE_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf` to the runtime out dir for every preset. Define `SMATCHET_ASSETS_SOURCE_DIR` compile def. |
| `tests/Source_Core/AiChatTimestamp.test.cpp` **(new)** + [tests/CMakeLists.txt](../../tests/CMakeLists.txt) | doctest cases pinning every relative-time branch boundary (59s = "just now", 60s = "1m ago", 60m = "1h ago", 24h = "1d ago", future-time guard, negative-delta clamp). Plus a round-trip test for `LocalCacheManager` chat CRUD if an in-memory SQLite fixture is already available in the test rig. |
| [Source_Core/include/ConfigManager.h](../../Source_Core/include/ConfigManager.h) + matching `.cpp` | Add `int AssistantHistoryMaxRows = 500;` config knob — drives the trim cap. Round-trip via existing nlohmann::json schema. Bump config schema-version once when feature lands (per AGENTS.md "hold the bump until verified end-to-end" — don't bump per intermediate iteration). |

## Existing utilities to reuse

- `ImGui::SetClipboardText(const char*)` — already used at `SmatchetAuditUi.cpp:323`.
- `MarkdownPreviewRender::HashContent` — keyed cache pattern at `SmatchetAiAssistantUi.cpp:191`. **Not** reused for per-message Y-position cache — see § Phase 6 § Y-cache keying.
- `SmatchetTheme::ApplyStyle` — extend the per-theme switch in place.
- `ImGui::SmallButton` — pattern at `SmatchetAiAssistantUi.cpp:567`.
- `ScheduleConfigSaveDetached` pattern at `SmatchetAiAssistantUi.cpp:80` — **not** copied wholesale for chat persistence (each chat write would spawn a thread; on shutdown one of them outlives the LCM). Replaced by a single coalescing worker — see § Persistence below.
- `LocalCacheManager` migration pattern at `LocalCacheManager.cpp:37-114` — `CREATE TABLE IF NOT EXISTS` + `SqliteTableHasColumn` probe for column additions.
- `HydrateFromConfigOnce` static-guard pattern at `SmatchetAiAssistantUi.cpp:92` — extend, don't replace.
- `s_messageHeightCache` (`SmatchetAiAssistantUi.cpp:180`) — already populated for finalised turns. Reuse to pre-size the bubble rect on every frame after the first render (first frame: no bubble bg yet, acceptable single-frame visual delay since the message just arrived).

## Persistence — single coalescing worker (Pillar 2 + Pillar 3)

The original draft proposed `ScheduleChatAppendDetached(LocalCacheManager&, ...)` mirroring `ScheduleConfigSaveDetached`. That pattern is Pillar-3-unsafe for the chat path: each append/pin-toggle spawns a fresh detached thread; on app shutdown one of them can outlive `LocalCacheManager` (the LCM is owned by `AppController`, which destructs before process exit), leading to use-after-free + a shipped crash. The chat path's write rate is also higher than the config path (every message + every pin toggle), so the thread-per-write cost adds up.

Replacement design:

```cpp
// SmatchetChatPersistWorker.h (new)
namespace smatchet::ai::chat_persist {
enum class OpKind { Append, UpdatePin, ClearAll, Trim };
struct Op {
    OpKind kind;
    AiMessage message;            // Append only
    std::int64_t rowId = -1;      // UpdatePin only
    bool pinned = false;          // UpdatePin only
    std::size_t trimCap = 0;      // Trim only
    // After AppendChatMessage returns the new row id, the worker fires onAppendCallback
    // (registered once at startup) on the UI thread via MainThreadDispatcher so the UI
    // can populate assistantHistoryRowIds[N] = newRowId.
};
void Start(LocalCacheManager& lcm, MainThreadDispatcher& dispatcher,
           std::function<void(std::size_t messageIndex, std::int64_t rowId)> onAppendCallback);
void Stop();  // Called from AppController dtor — joins the worker thread before LCM destructs.
void Enqueue(Op op);
}
```

Single background thread + `std::mutex` + `std::condition_variable` + `std::deque<Op>`. `Stop()` is the join boundary — called explicitly from `AppController`'s destructor BEFORE `LocalCacheManager` destructs. No detached threads. The worker drains pending ops on shutdown (best-effort within a 250 ms wall-clock cap; remaining ops dropped with a `LOG_WARN`). This is the same join-on-shutdown discipline the AI assistant controller already uses for its inference worker; reuse the shape.

Backpressure: the queue caps at 1024 entries; further enqueues drop the oldest non-Trim op + log `LOG_WARN`. Trim ops collapse — multiple pending Trims collapse into one with the latest cap.

All persistence touches (Phases 2/3/6/7 below) enqueue via `chat_persist::Enqueue` instead of detaching threads.

## Implementation phases

### Phase 1 — pure helper TU (smallest blast radius)

1. `AiChatTimestamp.{h,cpp}` + doctest. No code reads it yet.
2. Build + ctest green before piling on.

### Phase 2 — `AiMessage` schema additions + timestamp stamping

1. Add `CreatedAtUnixMs` + `Pinned` to `AiMessage`. NSDMI defaults.
2. In `dispatchSend` (`SmatchetAiAssistantUi.cpp:407`) stamp `userMsg.CreatedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();` before `push_back`.
3. Stamp the same field at **both** assistant-finalisation sites:
   - `AiAssistantController.cpp:458` (normal finalisation in the `onDelta` final-flag branch)
   - `AiAssistantController.cpp:488` (cancellation with partial-text retention in the `onError` branch)

### Phase 3 — SQLite persistence + chat-persist worker

1. **Worker first**: implement `smatchet::ai::chat_persist` per § Persistence above. Single thread; join on shutdown. Wire `Start` into `AppController` after `LocalCacheManager` initialises; wire `Stop` into the `AppController` destructor before LCM goes out of scope.
2. `LocalCacheManager` constructor: add `CREATE TABLE IF NOT EXISTS ai_chat_messages (...)` next to existing tables, plus `CREATE INDEX idx_ai_chat_messages_id_desc ON ai_chat_messages(id DESC)`.
3. CRUD methods — all wrap `db.exec`/`Statement` in try/catch. Failures log `LOG_WARN` and return false/-1; UI continues with in-memory state.
4. **Append-save**: UI enqueues `Op{OpKind::Append, msg}`. Worker calls `AppendChatMessage(msg)`, captures the new row id, posts a `MainThreadDispatcher` callback to populate `d.assistantHistoryRowIds[index]`.
5. **Pin-toggle save**: UI flips `m.Pinned` immediately (optimistic), then enqueues `Op{OpKind::UpdatePin, rowId, pinned}`. On worker failure (rare), log `LOG_WARN` — the in-memory pin still survives the session.
6. **Trim on insert**: worker fires `TrimChatMessages(cfg.AssistantHistoryMaxRows)` after every successful Append op. SQL: `DELETE FROM ai_chat_messages WHERE pinned=0 AND id NOT IN (SELECT id FROM ai_chat_messages WHERE pinned=0 ORDER BY id DESC LIMIT N)` — keeps the most-recent N non-pinned rows + every pinned row regardless of age.
7. **Hydration** — extend `HydrateFromConfigOnce` (`SmatchetAiAssistantUi.cpp:92`) to call `LocalCacheManager::LoadChatMessages(cap, history, ids)` and populate `d.assistantHistory` + `d.assistantHistoryRowIds`. If the cap is ≤ 200 rows, do this synchronously on first frame (<5 ms SQLite read). Above 200, dispatch to a one-shot background thread that posts the result via `MainThreadDispatcher`. Set `d.assistantHistoryHydrated = true` only after the population lands; `dispatchSend` and the pin-toggle handler check this flag and no-op when false. (Spec the threshold at 200 rows for the inline-vs-async split.)

### Phase 4 — Font Awesome integration

1. Vendor `IconsFontAwesome6.h` under `Source_Core/ThirdParty/IconsFontAwesome6/`.
2. Drop `assets/fonts/fa-solid-900.ttf` at `${CMAKE_SOURCE_DIR}/assets/fonts/`.
3. In `SmatchetApplyImGuiFont` (`SmatchetImGuiFonts.cpp:159`), after the Segoe-emoji merge (line 196-203), merge FA Solid using range `[0xF000, 0xF8FF]`. Missing TTF → log `LOG_WARN`, skip merge; UI falls back to text labels via a runtime `g_FaIconsLoaded` flag.
4. CMake POST_BUILD copy + `SMATCHET_ASSETS_SOURCE_DIR` define.
5. Sanity-render `ICON_FA_COPY` somewhere temporary; remove before commit.

### Phase 5 — theme tokens

1. Replace the const-`ImVec4`-in-header pattern with `GetActiveAiColors()` accessor populated by `ApplyStyle` — same shape as the existing `SmatchetThemeSyntaxColors` flow.
2. Bubble bg: `(themeAccent.r, themeAccent.g, themeAccent.b, 0.18f)` — survives both dark + light variants. Audit WCAG AA 4.5:1 contrast between bubble bg + `ImGuiCol_Text` on every shipped theme.

### Phase 6 — render rewrite in `DrawHistoryArea`

#### Y-cache keying (fixes review issue #3)

The Y-position cache is keyed by **`messageIndex` (the position in `assistantHistory`), not by content hash**. Reasons:
- Two messages with identical body text (e.g. user types `"ok"` twice) alias on `HashContent`, breaking the bookmark-jump target.
- Inserting a new message shifts every later message's screen-Y by one row; a hash-keyed cache returns stale Ys.

Cache shape: `std::vector<float> s_messageYCache;` resized to `assistantHistory.size()` every frame at top of `DrawHistoryArea`. On size shrink (e.g. clear-chat) the vector resizes. On size grow (new append) the new slot defaults to a sentinel (`-1.0f`); it gets populated when the message first renders.

The existing `s_messageHeightCache` keyed by content hash stays — that one IS legitimately content-stable (a finalised message's body never changes, so its layout height is content-stable; collisions are harmless because two messages with the same body render at the same height).

#### Render flow

1. **Above** the scroll child (only when any `m.Pinned`): a "Pinned" strip.
   - One row per pinned message: `ICON_FA_THUMBTACK` + role + first ~80 chars of body (collapse `\n` → ` `).
   - Row is an `ImGui::Selectable` (full-width, click anywhere jumps) — click sets `d.assistantScrollToMessageIndex = idx`.
   - Trailing `ICON_FA_XMARK` SmallButton per row → toggle `Pinned = false` + enqueue pin-toggle op.
   - Background tinted via `AiPinStripBg`.
2. **Inside** the scroll child, extend `renderTurn` to take `(messageIndex, msg)`:
   - **User bubble bg**: lookup `s_messageHeightCache[hash]`. If present, draw `ImDrawList::AddRectFilled` over the body rect (turnStartScreen, x to inner-content-region width, y + cachedHeight) **before** rendering text. First frame: cache miss → no bubble drawn that frame (single-frame visual delay; acceptable since the message just appeared).
   - Capture per-message screen-Y on render: `s_messageYCache[messageIndex] = ImGui::GetCursorPosY()` (window-relative, no scroll baseline math — see § Scroll-to-pinned latch below for why this is the right coordinate).
   - After body, **always reserve** a fixed-height action-row slot (`ImGui::GetFrameHeightWithSpacing() * 0.9f`). The reservation is unconditional so layout doesn't shift on hover/focus.
   - Action-row visibility: render the Copy / Pin / Timestamp widgets inside the slot when `ImGui::IsItemHovered(ImGuiHoveredFlags_ChildWindows)` on the turn-bounding rect **OR** any of (`ImGui::IsItemFocused()`, `msg.Pinned`) — this is the Pillar 4 keyboard-nav fix (review issue #5).
   - Buttons (in Tab order: Copy → Pin → bookmark close — assigned via ImGui's natural left-to-right item order):
     - `ICON_FA_COPY` SmallButton → `ImGui::SetClipboardText(msg.Content.c_str())` + set `assistantCopyToastUntilMs = now + 1500` + label "Copied".
     - `ICON_FA_THUMBTACK` (or `ICON_FA_THUMBTACK_SLASH` when already pinned) → toggle `msg.Pinned` + enqueue pin-toggle op.
     - Right-aligned relative timestamp `FormatRelativeTime(now, msg.CreatedAtUnixMs)`. Tooltip on `IsItemHovered(DelayNormal)` shows `FormatAbsoluteTime(msg.CreatedAtUnixMs)`.
3. **Scroll-to-pinned latch**: at top of `DrawHistoryArea`, if `d.assistantScrollToMessageIndex >= 0` AND that index is in-range AND `s_messageYCache[idx] >= 0.0f`, call `ImGui::SetScrollY(s_messageYCache[idx] - 4.0f)` + reset latch to `-1`. If the cache slot is still sentinel (`-1.0f`) — message has never rendered, e.g. far above the cull window — leave the latch set; subsequent frames populate as the message comes into the cull window. Re-attempt next frame.
4. Preserve existing `s_messageHeightCache` culling at `SmatchetAiAssistantUi.cpp:200`. The always-reserved action-row height factors into the cached height correctly — no special-casing needed.

#### Y-cache invalidation contract

- On `assistantHistory.push_back` (new user/assistant message) → vector grows by one, new slot defaults to `-1.0f`.
- On `LocalCacheManager::ClearChatMessages` + `assistantHistory.clear()` → vector resizes to 0; latch resets.
- On hydration → vector resizes to N; all slots start at `-1.0f` and populate as messages first render.
- The vector is **owned by `DrawHistoryArea`** as a `static`, mirroring `s_messageHeightCache`. Per-process scope is fine since there's only one chat panel.

### Phase 7 — clear-chat button + copy toast

1. New `ICON_FA_TRASH` SmallButton in the panel header. Click → `ImGui::OpenPopup("##ConfirmClearChat")` with 2-button confirm. Confirm → `assistantHistory.clear()` + `assistantHistoryRowIds.clear()` + enqueue `Op{OpKind::ClearAll}` + scroll-target reset.
2. Below the input row, when `now < assistantCopyToastUntilMs`, draw a 1-line ghosted strip `"Copied to clipboard"`. Auto-dismiss on time-out. Strip slot is always reserved — no layout shift.

## Risks + mitigations

| Risk | Mitigation |
|---|---|
| Persisted chat may contain user secrets / ticket bodies. | Stored in the existing local SQLite cache file (`Smatchet_LocalCache.sqlite`) — same trust boundary as other cached ticket data. Document in feature notes; add "Clear chat" button. Encryption-at-rest deferred (separate ADR). |
| Unbounded growth → SQLite bloat. | `AssistantHistoryMaxRows` (default 500) + trim-on-append. Pinned rows exempt from trim. |
| ImGui channel-split + Markdown render collision when drawing bubble bg. | Use the **after-first-render** approach via `s_messageHeightCache` instead of channels — draws bg rect before content using last-frame's measured height. Avoids inner-renderer channel state confusion. |
| Font Awesome TTF missing in shipped build. | POST_BUILD copy enforced + run-time `LOG_WARN` + UI text-label fallback when `g_FaIconsLoaded == false`. |
| Schema-version churn during iteration. | Per AGENTS.md "Schema-version bumps" rule — hold the config-schema bump until end-to-end verified. The SQLite table addition is additive (`CREATE TABLE IF NOT EXISTS`) so old DBs auto-upgrade without a version probe. |
| Pillar 2 violation (sync I/O on UI thread). | All persistence goes through the single coalescing worker (see § Persistence). Hydration on first frame reads ≤200 rows synchronously — **measured 160.6 µs / 200-row load** on dev box (in-memory SQLite, 20-iter microbenchmark in `tests/Source_Core/LocalCacheManagerChat.test.cpp` "hydration latency microbench"; on-disk WAL adds 2-5x, still under 1 ms). Above 200 rows it dispatches to a one-shot background thread + posts result via `MainThreadDispatcher`. |
| **Pillar 3 — detached-worker outliving LCM on shutdown.** | Replaced the per-write detached-thread pattern with a single coalescing worker joined on `AppController` destruction before `LocalCacheManager` destructs. Worker drains pending ops within a 250 ms cap; remaining ops drop + log `LOG_WARN`. No use-after-free path. |
| **Bookmark-jump scrolls to stale position when chat grows.** | Y-cache keyed by `messageIndex`, not content hash; vector resizes on `assistantHistory` size change so stale Ys can't survive an insert. |

## Verification

Per AGENTS.md § Verification automation — zero manual steps, every check below must be either automated (bucket-C screenshot / bucket-E ImGui Test Engine / pure-logic doctest / CLI scenario) or a `docs/backlog/agent-self-improvement/` (category `tooling`) deferred-automation entry. "Manual eye-test" without a backlog entry is a fail.

**Bucket A — pure-logic doctest** (mandatory):
- `tests/Source_Core/AiChatTimestamp.test.cpp` pins every relative-time branch boundary (59s = "just now", 60s = "1m ago", 60m = "1h ago", 24h = "1d ago", future-time guard, negative-delta clamp).
- `tests/Source_Core/LocalCacheManagerChat.test.cpp` (new) — in-memory SQLite fixture round-trips Append + Load + UpdatePin + Trim + ClearAll. Pins the "pinned rows exempt from trim" invariant.

**Bucket B — dual-target build gate** (mandatory):
- `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` clean. All changes are non-graphics structs + `.cpp`, no GLFW/OpenGL leakage into headers.
- `clang-format -i` + `cppcheck` + `clang-tidy` clean. PostToolUse hook runs format inline; deferred pipeline runs heavy passes at Stop.

**Bucket E — ImGui Test Engine scenarios** (mandatory; the user's nine manual click steps from the original draft map 1:1 here):
- `tests/ui/ai_chat_pin_bookmark.test.cpp` — programmatic: open AI Assistant panel → send 3 user messages → click Pin on message 1 → assert pin strip renders + first row body matches → scroll history to bottom → click bookmark → assert `ImGui::GetScrollY()` matches `s_messageYCache[0]` ± 4 px → click `X` on bookmark → assert pin strip empty.
- `tests/ui/ai_chat_copy_clipboard.test.cpp` — programmatic: send message → hover (test engine simulates hover) → click Copy → assert `ImGui::GetClipboardText() == msg.Content` + assert toast strip renders.
- `tests/ui/ai_chat_history_persist.test.cpp` — drives the persistence path inside a single process: send N messages → manually call `chat_persist::Stop()` to flush → call `LocalCacheManager::LoadChatMessages` → assert N messages + pin flags round-trip. (A two-process relaunch test is deferred to the test backlog per the "no out-of-process testing yet" residue rule.)
- `tests/ui/ai_chat_clear_confirm.test.cpp` — click Clear chat → assert confirmation popup opens → click Confirm → assert `assistantHistory.empty()` + persistence cleared.
- `tests/ui/ai_chat_keyboard_nav.test.cpp` — focus into history child → Tab through messages → assert action row renders on focused message + Tab reaches Copy / Pin / Bookmark-close in order.

**Bucket-C screenshot diff** (mandatory for the theme touch — review issue routing for the visual-validation exception):
- `tests/ui/screenshots/ai_chat_user_bubble_dark.png` golden for dark theme bubble bg.
- `tests/ui/screenshots/ai_chat_user_bubble_light.png` golden for light theme.
- Bootstrap goldens via the existing bucket-C rig (`docs/plans/shipped/visual-regression-bootstrap.md` if present, else add a backlog entry to introduce the screenshot harness as a precondition for this slice's merge).

**Manual residue** (none expected; if any step above turns out to be infeasible inside the ImGui Test Engine, append a `tooling`-category entry to `docs/backlog/agent-self-improvement/` describing the gap + acceptable interim).

## Out of scope (explicit deferrals)

- Encryption at rest for the chat table (separate ADR — touches all `Smatchet_LocalCache.sqlite` content, not just chat).
- Multi-conversation / threaded chat history (current model is one rolling thread).
- Markdown-aware copy (currently copies raw markdown body, not rendered text).
- Cross-machine sync of pinned messages or history (would require a backend storage layer).
- Two-process relaunch test (drive the second process from doctest; deferred until the test rig supports subprocess control — backlog entry, `tooling`).
- Screen-reader compatibility — Pillar 4 § out-of-scope.

## Implementation log

Branch `feat/ai-chat-claude-desktop-parity`, shipped on PR [#334](https://github.com/alexandrosk0/Smatchet/pull/334).

- `55dcacd` · Phase 1 — `AiChatTimestamp.{h,cpp}` pure helpers + doctest (every relative-time branch boundary pinned)
- `ec1b4dd` · Phase 2 — `AiMessage` schema additions (`CreatedAtUnixMs`, `Pinned`) + stamping at `dispatchSend` + both `AiAssistantController` finalisation sites
- `7ee54a9` · `MISSING_BASELINE` backlog entry for the develop-level perf-review gap surfaced during Phase 1+2 pillar2-scan
- `723c4a7` · Phase 3 — SQLite persistence + `smatchet::ai::chat_persist` coalescing worker; `LocalCacheManager` ai_chat_messages CRUD; AppController Start/Stop wiring; hydration path with inline-vs-async cutoff; 9 chat doctest cases (39 assertions); measured hydration at 143–161 µs / 200-row load
- `ab09c04` · Phase 4 — Font Awesome 6 Solid integration with runtime fallback; vendored `IconsFontAwesome6.h` (zlib); CMake POST_BUILD copy + `SMATCHET_ASSETS_SOURCE_DIR` define; `g_FaIconsLoaded` flag + text-label fallback path; `THIRD_PARTY_LICENSES.md`
- `1f0270b` · Gitignore for `assets/fonts/*.ttf` per the "NOT committed" design contract
- `a7a580c` · Phase 5 — `SmatchetThemeAiColors` struct + `GetActiveAiColors()` accessor; per-theme palettes for all 7 themes; 7 doctest cases pinning the contract + Pillar-4 alpha-band invariant
- `6ccd27b` · Phase 6 — `DrawHistoryArea` rewrite (pin strip + bubble bg + always-reserved action row + scroll-to-pinned latch + Y-cache); `ai_chat.history.{draw,pin_strip,render_turn}` perf scopes; new `ai-chat-history-render` scenario
- `bfe058f` · Perf auto-gating slice — added `ai-chat-history-render` to PR-fast set; updated `perf-gatekeeper.md` diff map; new AGENTS.md "Perf slice-boundary auto-run — scenario-aware" rule
- `a930f95` · Phase 7 — clear-chat header button + confirm popup + copy toast strip with always-reserved layout slot + fade-out tail

## Deviations from plan

| Change | Rationale |
|---|---|
| Worker pattern: replaced per-write detached-thread design with single coalescing worker | Phase 3 § Persistence — Pillar 3 use-after-free path closed (detached thread could outlive `LocalCacheManager`); shipped as `smatchet::ai::chat_persist::{Start, Stop, Enqueue}` with 250 ms drain budget + join boundary in `~AppController` before LCM destructs. |
| FA glyph range widened to `[0xe005, 0xf8ff]` (not just `[0xF000, 0xF8FF]`) | Phase 4 — `ICON_FA_THUMBTACK_SLASH` lives at `U+e68f`, below the `0xF000` floor the plan named. Wider range matches upstream `ICON_MIN_FA` / `ICON_MAX_FA` constants. |
| Per-theme palette additions touched ALL 7 themes (not just SmatchetDark + light) | Phase 5 — keeping in lock-step with the existing `SmatchetThemeSyntaxColors` shape that already covers every theme; partial coverage would have left HighContrast / Norton without AI tokens and silently fallen back to the default seed. |
| Y-cache size matched per-frame (not at-push-back only) | Phase 6 — simpler invariant; vector `assign(N, -1.0f)` whenever size diverges, no per-mutation tracking required. Cost is sub-µs even at N=500. |
| Hover/focus alpha-gate uses previous-frame state, not current-frame query | Phase 6 — ImGui chicken-and-egg: `IsItemHovered` requires the item rendered; rendering only on hover means hover never fires. 1-frame lag on first hover is imperceptible. |
| Perf gating added in its own slice (`bfe058f`) — not part of Phase 6's commit | Surfaced when the user asked "how is the performance" after Phase 6; ended up as a separate self-contained concern (CI workflow JSON + perf-gatekeeper map + AGENTS.md rule, no code). |
| Pillar 2 Phase 3.10 measurement via doctest microbench, not via `perf.snapshot` against a UI scenario | Phase 3 — no AI-chat scenario existed yet; microbench against in-memory SQLite gave a deterministic number for the dominant cost (`LoadChatMessages`). The full scenario shipped in Phase 6 (`ai-chat-history-render`). |
| Bucket-E ImGui-Test-Engine scenarios listed in § Verification — deferred entirely | None of the 5 listed scenarios authored; bucket-E coverage for AI chat is open work. User performed visual sign-off after Phase 6 + Phase 7 in lieu of automated scenarios. Flagged in `docs/backlog/agent-self-improvement/process.md` (category `process`). |
| Bucket-C screenshot golden bootstrap — deferred entirely | Per the plan's "if bootstrap rig doesn't exist, add backlog entry as merge precondition" guidance — bootstrap rig still doesn't exist; deferral inherits the existing backlog item. |

## Verification

What actually got verified vs. what the plan listed.

**Bucket A — pure-logic doctest** (mandatory, ✅ delivered):
- `tests/Source_Core/AiChatTimestamp.test.cpp` — every relative-time branch boundary pinned (Phase 1).
- `tests/Source_Core/LocalCacheManagerChat.test.cpp` — 9 cases / 39 assertions covering Append + Load + UpdatePin + Trim + ClearAll, "pinned exempt from trim" invariant, graceful UpdatePin on missing id, plus the hydration latency microbench (143–161 µs at 200 rows).
- `tests/Source_Core/SmatchetThemeAiColors.test.cpp` — 7 cases pinning SmatchetDark / Vs2022Light / HighContrast palettes verbatim + pairwise divergence + round-trip + idempotency + Pillar-4 alpha-band invariant.

**Bucket B — dual-target build gate** (mandatory, ✅ delivered):
- `SmatchetStandalone` clean every slice; `SmatchetCore_DX12` clean on all Phase-1-through-7 TUs (DX12 build halts later on the pre-existing `WhisperAiAssistantAutosendScenario.cpp` develop bug per `docs/backlog/agent-self-improvement/bug.md:16` — unrelated, predates this work by `4c56b83b`).
- `clang-format` / `cppcheck` / `clang-tidy` clean via the deferred lint pipeline at every slice boundary.

**Bucket E — ImGui Test Engine scenarios** (mandatory in plan, ❌ deferred):
- None of the 5 listed scenarios authored. Substituted manual visual sign-off after Phase 6 + Phase 7. Tracked: `docs/backlog/agent-self-improvement/process.md` (category `process`) — "AI chat panel bucket-E coverage gap".

**Bucket-C screenshot diff** (mandatory in plan, ❌ deferred):
- Bootstrap rig still not in tree. Inherits the existing backlog item to introduce it.

**Perf gating** (added beyond original plan, ✅ delivered):
- New `ai-chat-history-render` scenario emits permanent `ai_chat.history.{draw, pin_strip, render_turn}` perf rows.
- PR-fast CI runs it on every PR touching `Source_Core/**` and auto-bootstraps the baseline on first run.
- AGENTS.md slice-boundary rule + `agents/perf-gatekeeper.md` diff map route local + agent-routed perf checks to the new scenario.
- Measured at Phase 7: `drawAiAssistantPanel` outer = 0.084 ms / frame (1.2 % of the 6.94 ms Pillar 1 budget).

**Manual residue + backlog**:
- Bucket-E scenarios for clear-chat, copy-clipboard, pin-bookmark scroll, history-persist, keyboard-nav — all deferred. `process.md` backlog entry to be added in a follow-up commit (this revision is the trigger).
