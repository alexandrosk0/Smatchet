#include "SmatchetAiAssistantUi.h"

#if defined(SMATCHET_WITH_AI)

#include "Ui/SmatchetAiAssistantUi_detail.h"

#include "AiAssistantController.h"
#include "AiAssistantInputSeedDecision.h"
#include "AiAssistantSendButton.h"
#include "AiChatTimestamp.h"
#include "AiClientFactory.h"
#include "MarkdownPreviewRender.h"
#include "SelectableTextRun.h"
#include "AiContextBuilder.h"
#include "AiModelCatalog.h"
#include "AiOutboundConsent.h"
#include "AiTypes.h"
#include "AppController.h"
#include "CacheEvictionPolicy.h"
#include "ConfigManager.h"
#include "ConfigSaveWorker.h"
#include "DictationInsertionRouter.h"
#include "Logger.h"
#include "MemoryTelemetry.h"
#include "SmatchetChatPersistWorker.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetHelpMarker.h"
#include "SmatchetImGuiFonts.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "SpreadsheetState.h"
#include "UiPerfMonitor.h"
// Vendored Font Awesome 6 icon-name macros (zlib licence). Pulled in
// unconditionally — when the FA TTF is missing at runtime the UI falls
// back to short text labels via SmatchetAreFaIconsLoaded().
#include "IconsFontAwesome6.h"

#include "imgui.h"
// `imgui_internal.h` must be pulled BEFORE the `#define ImGui SmatchetLocalizedImGui`
// macro below — same ordering as SmatchetUI_MainMenu.cpp. We need it for
// `ImGui::GetInputTextState(id)->ReloadUserBufAndMoveToEnd()`, which is the
// only sanctioned way to force ImGui to re-read an externally-modified `buf`
// on the next InputText call when the widget is currently active. Without
// this reload, the Whisper dictation router splices text into `s_inputCharBuf`
// while the chat input is focused, then `ImGui::InputTextMultiline` below
// overwrites the splice with the stale internal `state->TextA`. See
// imgui_widgets.cpp:4821 (init_reload_from_user_buf) for the upstream branch.
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr float kInputRowsTall = 4.0f;
constexpr int kInputBufCap = 8 * 1024;

// Single source of truth for the first-send outbound-context consent popup id.
// Used by both the OpenPopup in DispatchAiSend's gate and the BeginPopupModal in
// DrawOutboundConsentModal — a shared constant so the two can never drift apart.
const char* const kOutboundConsentPopupId = "##AiOutboundConsent";

// Process-static char buffer for the chat input. Hoisted to namespace scope
// (originally a function-static inside DrawInputAndButtons) so the panel-level
// dictation register / unregister can address it without exposing a getter.
// Lifetime is for the life of the process; explicit clearing happens on Send
// and on panel close.
std::array<char, kInputBufCap> s_inputCharBuf{};
bool s_inputCharBufSeeded = false;

// Bytes the most recent InputText frame had to drop because a paste / splice
// would have pushed the text past the fixed `s_inputCharBuf` cap. Set inside
// InputBufferResizeCallback (ImGui's CallbackResize path), consumed + cleared
// by DrawInputAndButtons after the InputText call to raise the user-facing
// truncation toast. Single-threaded UI-thread access — no atomic needed.
std::size_t s_inputTruncatedDroppedBytes = 0;

// CallbackResize handler for the chat input. ImGui invokes this when a paste (or
// the dictation splice-reload) would grow the text past the buffer capacity. The
// stdlib pattern would resize a std::string here, but we keep a hard fixed cap
// on purpose (the buffer is process-static and dictation-spliced). So instead of
// growing we measure the would-be overflow via the pure
// AiTruncatedPasteDroppedBytes helper, stash it for the draw code to toast, and
// leave Buf and BufTextLen untouched — ImGui then clamps the insert to the
// existing capacity exactly as it did before this callback existed
// (behaviour-preserving for the non-overflow path).
int InputBufferResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data != nullptr && data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        // data->BufSize is the requested new capacity (capacity+1); BufTextLen is
        // the desired text length in bytes (excludes the NUL). Both already exceed
        // our fixed cap here, hence the callback firing.
        const std::size_t dropped = smatchet::ai::AiTruncatedPasteDroppedBytes(data->BufTextLen, kInputBufCap);
        if (dropped > 0) {
            s_inputTruncatedDroppedBytes = dropped;
        }
    }
    // Return 0: we did NOT resize. ImGui keeps the original buffer + clamps.
    return 0;
}

// Phase F — auto-send-on-punctuation hand-off slot. The dictation router calls
// the registered send callback from `RunHotkeyRelease_Worker`'s UI-thread
// post-insertion path (MainThreadDispatcher drain); the callback flips this
// atomic. The next AI-assistant panel draw observes the flag and invokes
// `dispatchSend()` with the just-inserted text. Atomic so a flip-and-poll race
// across frames is well-defined; the actual send still runs on the UI thread.
std::atomic<bool> s_pendingAutoSend{false};
bool s_autoSendCallbackRegistered = false;

// UX pillar 2: ConfigManager::Save performs a synchronous JSON encode + atomic
// file replace; even when the user is only toggling a checkbox the resulting
// disk write can easily breach the 6.94 ms per-frame UI budget on a slow disk.
// Hand the save to the single coalescing config-save worker (snapshot by value,
// latest-per-kind wins, write serialized + off the UI thread). Replaces the
// former per-save detached-thread shim now that the shared worker exists.
void ScheduleConfigSaveDetached(const TrackerConfig& cfg) { smatchet::config_save::EnqueueTrackerConfig(cfg); }

// Inline-vs-async hydration split. Caps ≤ this threshold load synchronously on
// first frame; SQLite reads of ~200 rows are well under 5 ms on warm disk so a
// brief one-frame stall is preferable to the cost of an extra background-thread
// + MainThreadDispatcher round-trip. Above the threshold the load goes to a
// detached worker that posts the result back via the dispatcher. The exact value
// is documented in `docs/plans/shipped/ai-chat-claude-desktop-parity.md` § Phase 3 step
// 7; the measured latency for the inline path is recorded next to the call site
// via the Pillar-2 annotation below (see comment block).
constexpr std::size_t kHydrateInlineRowThreshold = 200;

void HydrateFromConfigOnce(AppController& app, UiDrawSession& d) {
    static bool s_hydrated = false;
    if (s_hydrated) {
        return;
    }
    s_hydrated = true;
    d.assistantPanelOpen = d.cfg.AssistantPanelOpen;
    if (d.assistantInputBuf.capacity() < static_cast<size_t>(kInputBufCap)) {
        d.assistantInputBuf.reserve(kInputBufCap);
    }

    // Phase 3 of ai-chat-claude-desktop-parity. Load persisted chat history through
    // the AppController wrapper (which forwards to LocalCacheManager). Until the
    // load completes, `assistantHistoryHydrated = false` gates dispatchSend +
    // pin-toggle so a Send during the hydrate window doesn't insert a duplicate.
    // Pillar 2 — UI thread budget 6.94 ms. The inline-vs-async split below honours
    // the contract: ≤200-row read is a single SQLite SELECT against the indexed
    // `idx_ai_chat_messages_id_desc`. Phase 3.10 measured 160.6 µs / load average
    // for 200 rows on dev box (in-memory SQLite, 20-iter benchmark in
    // `tests/Core/LocalCacheManagerChat.test.cpp` "hydration latency
    // microbench") — 2.3% of frame budget, comfortably under threshold. On-disk
    // WAL is typically 2-5x slower; even 800 µs leaves > 6 ms of headroom. Above
    // 200 rows the read goes to a one-shot worker thread + MainThreadDispatcher
    // post so the UI thread never blocks on the SELECT.
    const std::size_t cap =
        static_cast<std::size_t>(d.cfg.AssistantHistoryMaxRows > 0 ? d.cfg.AssistantHistoryMaxRows : 500);
    if (cap <= kHydrateInlineRowThreshold) {
        app.LoadAiChatMessages(cap, d.assistantHistory, d.assistantHistoryRowIds);
        d.assistantHistoryHydrated = true;
        return;
    }
    // Async hydration. Snapshot the cap by value (the worker thread cannot read
    // d.cfg safely). Pillar 3 — the worker captures `app` by reference; AppController
    // outlives any panel frame because `~AppController` joins `chat_persist::Stop()`
    // before the LCM destructs, and the SmatchetUI `g_ui` is process-static. The
    // dispatched callback may run after the panel was closed and reopened — that's
    // fine because it only writes `d.assistantHistory` / `d.assistantHistoryRowIds`
    // / `d.assistantHistoryHydrated`, all of which live on the same `UiDrawSession`.
    AppController* appPtr = &app;
    // Joined background-task pool, not a raw detached thread — the no-detach lint forbids that.
    // Joined at shutdown, which is exactly the lifetime guarantee the comment above relies on.
    app.LaunchBackgroundTask([appPtr, cap]() {
        try {
            std::vector<AiMessage> loaded;
            std::vector<std::int64_t> ids;
            appPtr->LoadAiChatMessages(cap, loaded, ids);
            appPtr->mainThreadDispatcher.PostToMainThread([loaded = std::move(loaded), ids = std::move(ids)]() mutable {
                g_ui.assistantHistory = std::move(loaded);
                g_ui.assistantHistoryRowIds = std::move(ids);
                g_ui.assistantHistoryHydrated = true;
            });
        } catch (...) {
            // Graceful degradation — leave hydrated false so further dispatches no-op
            // until the next session retries. The LCM already logged the LOG_WARN.
            appPtr->mainThreadDispatcher.PostToMainThread([]() { g_ui.assistantHistoryHydrated = true; });
        }
    });
}

void PersistOpenStateImmediate(UiDrawSession& d) {
    if (d.cfg.AssistantPanelOpen != d.assistantPanelOpen) {
        d.cfg.AssistantPanelOpen = d.assistantPanelOpen;
        ScheduleConfigSaveDetached(d.cfg);
    }
}

// Per-message md4c plan cache. Pillar 1 opt #1 — finalised (non-streaming)
// chat messages have immutable content; cache their parsed plan and replay it
// each frame instead of re-running md_parse. Keyed by 64-bit content hash;
// `contentLen` retained as a cheap collision detector (same hash + same len
// is treated as a hit; hash collisions on different lengths fall through to
// rebuild). FIFO cap at kMaxCachedPlans keeps memory bounded across long
// chat sessions.
constexpr std::size_t kMaxCachedPlans = 256;
// Aggregate byte cap (Phase 4), alongside the entry cap. Proxies the parsed-plan AST size by the
// cached message's source length — the AST is larger but tracks content length, and the source
// length is free to measure. 4 MiB of summed markdown source is well past any sane chat history.
constexpr std::size_t kMaxPlanCacheBytes = 4u * 1024u * 1024u;
struct CachedPlanEntry {
    std::size_t contentLen = 0;
    MarkdownPreviewRender::PreviewPlanPtr plan;
};
// Process-static — one AI panel exists in the app at a time.
std::unordered_map<std::uint64_t, CachedPlanEntry> s_planCache;
std::vector<std::uint64_t> s_planCacheInsertionOrder;
// Running Σ contentLen over s_planCache, maintained on insert/evict/clear. Mirrored to the
// memtel atomic so the perf.memory gauge reads it without reaching into this TU.
std::size_t s_planCacheBytes = 0;

// Per-message render caches. File-scope (not function-local statics) so the "Clear conversation"
// action can drop them in one call — once history is wiped their content/index-keyed entries are
// pure garbage that would otherwise linger for the rest of the session.
//   - height cache: realised layout height per finalised message, for off-screen culling.
//   - hover cache: whether the action row was active last frame, per message index, for the
//     show-on-hover alpha gate; the 1-frame lag avoids an ImGui hover-detection chicken-and-egg.
std::unordered_map<std::uint64_t, float> s_messageHeightCache;
std::unordered_map<std::size_t, bool> s_turnActiveLastFrame;

void PublishPlanCacheBytes() {
    smatchet::memtel::PlanCacheApproxBytes().store(s_planCacheBytes, std::memory_order_relaxed);
}

const MarkdownPreviewRender::PreviewPlan& GetOrBuildPlanForMessage(const std::string& content) {
    const std::uint64_t key = MarkdownPreviewRender::HashContent(content);
    auto it = s_planCache.find(key);
    if (it != s_planCache.end() && it->second.contentLen == content.size()) {
        return *it->second.plan;
    }
    // Cache miss or hash collision on different length. Build the plan first, evict second, then
    // insert — so the new entry is never a candidate for its own eviction pass. Post-insert figures
    // are passed to CacheOverCap so the loop makes room for the new item's size; only pre-existing
    // entries are ever dropped because the new key is not yet in the insertion-order vector.
    CachedPlanEntry entry;
    entry.contentLen = content.size();
    entry.plan = MarkdownPreviewRender::MakePlan();
    MarkdownPreviewRender::BuildPlan(content, *entry.plan);
    while (!s_planCacheInsertionOrder.empty() &&
           smatchet::CacheOverCap(s_planCacheInsertionOrder.size() + 1, s_planCacheBytes + content.size(),
                                  kMaxCachedPlans, kMaxPlanCacheBytes)) {
        const std::uint64_t evictKey = s_planCacheInsertionOrder.front();
        s_planCacheInsertionOrder.erase(s_planCacheInsertionOrder.begin());
        const auto victim = s_planCache.find(evictKey);
        if (victim != s_planCache.end()) {
            s_planCacheBytes -= victim->second.contentLen;
            s_planCache.erase(victim);
        }
    }
    const auto inserted = s_planCache.emplace(key, std::move(entry));
    s_planCacheInsertionOrder.push_back(key);
    s_planCacheBytes += content.size();
    PublishPlanCacheBytes();
    return *inserted.first->second.plan;
}

// Drop every per-conversation render cache. Called when history is cleared so stale entries do
// not survive the session.
void ClearAiRenderCaches() {
    s_planCache.clear();
    s_planCacheInsertionOrder.clear();
    s_planCacheBytes = 0;
    PublishPlanCacheBytes();
    s_messageHeightCache.clear();
    s_turnActiveLastFrame.clear();
}

// True (and updates the stored basis) when the layout basis for cached message heights has
// changed since the last call — font size or content-wrap width. Cached heights are pixel
// layouts, so a font or width change makes every cached value stale; the caller clears the
// height cache on a true return. The >1px width threshold ignores sub-pixel jitter, so a
// steady-state frame returns false and pays nothing.
bool AiLayoutBasisChanged(float fontSize, float wrapWidth, float& lastFontSize, float& lastWrapWidth) {
    if (fontSize != lastFontSize || std::fabs(wrapWidth - lastWrapWidth) > 1.0f) {
        lastFontSize = fontSize;
        lastWrapWidth = wrapWidth;
        return true;
    }
    return false;
}

// Helper: collapse newlines + truncate to a single-line preview for the pin strip
// row label. ~80 chars matches the design doc; tail ellipsis added when truncated.
std::string MakePinStripPreview(const std::string& body) {
    constexpr std::size_t kMax = 80;
    std::string preview;
    preview.reserve((std::min)(body.size(), kMax + 4));
    for (char c : body) {
        if (preview.size() >= kMax) {
            break;
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            // Collapse runs of whitespace into a single space so the preview
            // reads as one continuous line.
            if (!preview.empty() && preview.back() != ' ') {
                preview.push_back(' ');
            }
        } else {
            preview.push_back(c);
        }
    }
    if (body.size() > preview.size()) {
        preview.append("...");
    }
    return preview;
}

// Toggle the Pinned flag on a chat message and route the persistence side-effect
// through the chat_persist worker. No-op (warn) when the row hasn't been
// persisted yet (rowId == -1 means the worker hasn't reported back the SQLite
// id for this freshly-appended message). On the next launch, hydrate will pick
// up the row with its post-restart id and the toggle will work.
void TogglePinAndPersist(UiDrawSession& d, std::size_t messageIndex, bool newPinnedState) {
    if (messageIndex >= d.assistantHistory.size()) {
        return;
    }
    d.assistantHistory[messageIndex].Pinned = newPinnedState;
    if (messageIndex >= d.assistantHistoryRowIds.size()) {
        return;
    }
    const std::int64_t rowId = d.assistantHistoryRowIds[messageIndex];
    if (rowId <= 0) {
        // Persist hasn't completed yet for this message; pin survives the
        // session but not a restart. Log so a tester can correlate the
        // missing-on-restart pin with this race condition.
        LOG_TRACE("ai-chat: pin toggle on un-persisted message (idx=%zu); in-memory only", messageIndex);
        return;
    }
    smatchet::ai::chat_persist::Op op;
    op.kind = smatchet::ai::chat_persist::OpKind::UpdatePin;
    op.rowId = rowId;
    op.pinned = newPinnedState;
    smatchet::ai::chat_persist::Enqueue(std::move(op));
}

// Render the pinned-bookmarks strip above the history scroll child.
// Only emits anything when at least one message is pinned; the slot is otherwise
// zero-height (no layout reservation, no separator). Click on a row scrolls the
// history to the corresponding message; the trailing X unpins.
void DrawPinStripIfAny(UiDrawSession& d) {
    SMATCHET_UI_PERF_SCOPE("ai_chat.history.pin_strip");
    if (d.assistantHistory.empty()) {
        return;
    }
    // Count once via std::count_if; reused below to size the strip.
    const auto pinnedCount = std::count_if(d.assistantHistory.begin(), d.assistantHistory.end(),
                                           [](const AiMessage& m) { return m.Pinned; });
    if (pinnedCount == 0) {
        return;
    }

    const SmatchetThemeAiColors& ai = SmatchetTheme::GetActiveAiColors();
    const ImVec4 stripBg(ai.AiPinStripBg[0], ai.AiPinStripBg[1], ai.AiPinStripBg[2], ai.AiPinStripBg[3]);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, stripBg);
    // Auto-height: rows are full-text-line + tiny vertical pad. AlwaysAutoResize-like
    // behaviour via height=0 only works for vertical layout, so we measure: each row
    // is one TextLineHeightWithSpacing + IconSpacing. Cap visible rows at 4 so a
    // user who pins 50 messages doesn't lose the entire history panel.
    constexpr int kMaxVisiblePinRows = 4;
    const int visibleRows = (std::min)(static_cast<int>(pinnedCount), kMaxVisiblePinRows);
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    const float stripH = rowH * static_cast<float>(visibleRows) + 4.0f;
    ImGui::BeginChild("##AiAssistantPinStrip", ImVec2(0.0f, stripH), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const bool faLoaded = SmatchetAreFaIconsLoaded();
    for (std::size_t i = 0; i < d.assistantHistory.size(); ++i) {
        AiMessage& m = d.assistantHistory[i];
        if (!m.Pinned) {
            continue;
        }
        ImGui::PushID(static_cast<int>(i));
        const std::string preview = MakePinStripPreview(m.Content);
        const char* roleIcon = faLoaded ? ICON_FA_THUMBTACK : "*";
        const char* role = (m.Role == "user") ? "You" : "Assistant";
        // Render full-width Selectable so click anywhere triggers the jump.
        // The trailing X close button is laid out OVER the Selectable's right
        // edge via SameLine — same pattern as ImGui's CloseButton on tabs.
        char rowLabel[256];
        std::snprintf(rowLabel, sizeof(rowLabel), "%s  %s: %s", roleIcon, role, preview.c_str());
        const float closeBtnW =
            ImGui::CalcTextSize(faLoaded ? ICON_FA_XMARK : "X").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float rowAvailW = ImGui::GetContentRegionAvail().x - closeBtnW - ImGui::GetStyle().ItemSpacing.x;
        if (ImGui::Selectable(rowLabel, false, 0, ImVec2(rowAvailW, 0.0f))) {
            d.assistantScrollToMessageIndex = static_cast<int>(i);
        }
        ImGui::SameLine();
        const char* closeLabel = faLoaded ? ICON_FA_XMARK : "X";
        if (ImGui::SmallButton(closeLabel)) {
            TogglePinAndPersist(d, i, /*newPinnedState=*/false);
            // If this pinned row was also the active scroll target, reset
            // the latch — there's nothing to scroll to once it's unpinned
            // (the message itself still exists in history, but the user's
            // intent was to jump TO it via the bookmark; bookmark gone).
            if (d.assistantScrollToMessageIndex == static_cast<int>(i)) {
                d.assistantScrollToMessageIndex = -1;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// Per-frame shared state for the history scroll-child + its turn renderer.
// Captured once at the top of DrawHistoryArea so each turn helper reads
// snapshots rather than re-fetching theme / clock / selection context.
struct AiHistoryDrawCtx {
    UiDrawSession& d;
    const SmatchetThemeAiColors& palette;
    SelectableText::Context& selCtx;
    MarkdownPreviewRender::Options& renderOpts;
    std::vector<float>& messageYCache;
    bool faLoaded;
    std::int64_t nowMs;
};

// Always-reserved per-turn action row (Copy / Pin / right-aligned timestamp).
// Alpha-gated so the slot height is constant whether or not the row is visible.
// `bodyHovered` / `bodyFocused` feed the next-frame show-on-hover latch.
// Extracted verbatim from RenderHistoryTurn to keep both helpers under the
// branch cap.
void RenderTurnActionRow(AiHistoryDrawCtx& ctx, int messageIndex, AiMessage* msgPtr, float actionRowSlotH,
                         bool bodyHovered, bool bodyFocused) {
    UiDrawSession& d = ctx.d;
    const SmatchetThemeAiColors& aiPalette = ctx.palette;
    const bool faLoaded = ctx.faLoaded;
    const std::int64_t nowMs = ctx.nowMs;

    const bool wasActive = s_turnActiveLastFrame.count(static_cast<std::size_t>(messageIndex)) != 0u &&
                           s_turnActiveLastFrame[static_cast<std::size_t>(messageIndex)];
    const bool showRow = msgPtr->Pinned || wasActive;
    const ImVec2 actionRowStart = ImGui::GetCursorScreenPos();

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, showRow ? 1.0f : 0.0f);

    // Copy button.
    const char* copyLabel = faLoaded ? ICON_FA_COPY : "Copy";
    char copyId[64];
    std::snprintf(copyId, sizeof(copyId), "%s##AiCopy%d", copyLabel, messageIndex);
    bool copyHovered = false;
    bool copyFocused = false;
    if (ImGui::SmallButton(copyId)) {
        if (showRow) {
            // Guard the click on alpha=0 — invisible buttons still
            // receive clicks in ImGui, but actions should only fire
            // when the row is visible. Cheap defence vs accidental
            // off-screen clipboard writes.
            ImGui::SetClipboardText(msgPtr->Content.c_str());
            d.assistantCopyToastUntilMs = nowMs + 1500;
            d.assistantCopyToastLabel = "Copied";
        }
    }
    copyHovered = ImGui::IsItemHovered();
    copyFocused = ImGui::IsItemFocused();

    ImGui::SameLine();

    // Pin / Unpin button.
    const char* pinIcon =
        msgPtr->Pinned ? (faLoaded ? ICON_FA_THUMBTACK_SLASH : "Unpin") : (faLoaded ? ICON_FA_THUMBTACK : "Pin");
    char pinId[64];
    std::snprintf(pinId, sizeof(pinId), "%s##AiPin%d", pinIcon, messageIndex);
    bool pinHovered = false;
    bool pinFocused = false;
    if (ImGui::SmallButton(pinId)) {
        if (showRow) {
            TogglePinAndPersist(d, static_cast<std::size_t>(messageIndex), !msgPtr->Pinned);
        }
    }
    pinHovered = ImGui::IsItemHovered();
    pinFocused = ImGui::IsItemFocused();

    // Right-aligned relative timestamp + absolute-time tooltip.
    // Canonical ImGui right-align: SameLine to anchor on the Pin button's
    // row, then SetCursorPosX to (current + remaining - textW - padding).
    // `GetCursorPosX() + GetContentRegionAvail().x` always equals the
    // window content's right-edge X regardless of how the cursor got
    // there (auto-advanced or via SameLine), so the math survives both
    // paths. The earlier `SameLine(0.0f, hugeSpacing)` approach
    // computed availW from the wrong baseline and clipped the timestamp
    // off-screen to the right.
    if (msgPtr->CreatedAtUnixMs > 0) {
        ImGui::SameLine();
        const std::string rel = smatchet::ai::FormatRelativeTime(nowMs, msgPtr->CreatedAtUnixMs);
        const float relW = ImGui::CalcTextSize(rel.c_str()).x;
        const float curX = ImGui::GetCursorPosX();
        const float availFromHere = ImGui::GetContentRegionAvail().x;
        const float targetX = curX + availFromHere - relW - ImGui::GetStyle().ItemSpacing.x;
        if (targetX > curX) {
            ImGui::SetCursorPosX(targetX);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(aiPalette.AiActionRowIcon[0], aiPalette.AiActionRowIcon[1],
                                                    aiPalette.AiActionRowIcon[2], aiPalette.AiActionRowIcon[3]));
        ImGui::TextUnformatted(rel.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && showRow) {
            const std::string abs = smatchet::ai::FormatAbsoluteTime(msgPtr->CreatedAtUnixMs);
            ImGui::SetTooltip("%s", abs.c_str());
        }
    }

    ImGui::PopStyleVar();

    // Snap cursor to the bottom of the reserved slot so the next turn
    // starts at a predictable Y (the row's natural cursor advance may
    // be slightly less than actionRowSlotH because SmallButton is
    // shorter than a full frame).
    const float currentY = ImGui::GetCursorScreenPos().y;
    const float targetY = actionRowStart.y + actionRowSlotH;
    if (targetY > currentY) {
        ImGui::Dummy(ImVec2(0.0f, targetY - currentY));
    }

    s_turnActiveLastFrame[static_cast<std::size_t>(messageIndex)] =
        bodyHovered || bodyFocused || copyHovered || copyFocused || pinHovered || pinFocused;
}

// Render one chat turn into the history scroll-child. messageIndex is the
// position in assistantHistory (or -1 for the in-flight streaming tail, which
// has no persistence row). `body` is mutable on persistent rows so the
// pin-toggle can flip Pinned. Extracted verbatim from the former renderTurn
// lambda in DrawHistoryArea — behaviour is byte-identical.
// Off-screen culling for a finalised (cacheable) history turn with a known height. Computes the
// body hash into outBodyKey, then — when the turn is fully off-screen — emits a Dummy of the
// realised height and returns true (caller should early-return). Returns false otherwise.
bool TryCullHistoryTurn(const std::string& body, bool cacheable, float actionRowSlotH, std::uint64_t& outBodyKey) {
    outBodyKey = 0;
    if (!cacheable) {
        return false;
    }
    outBodyKey = MarkdownPreviewRender::HashContent(body);
    const auto hit = s_messageHeightCache.find(outBodyKey);
    if (hit != s_messageHeightCache.end() && hit->second > 0.0f) {
        const float labelH = ImGui::GetTextLineHeightWithSpacing();
        // Include the always-reserved action-row slot so the culling
        // height matches the realised layout (otherwise a culled turn
        // skips by labelH+body while a rendered turn advances by
        // labelH+body+actionRow, drifting the scroll position).
        const float totalH = labelH + hit->second + actionRowSlotH;
        if (!ImGui::IsRectVisible(ImVec2(1.0f, totalH))) {
            ImGui::Dummy(ImVec2(0.0f, totalH));
            return true;
        }
    }
    return false;
}

// Pre-draw the user-message bubble background via ImDrawList before the text renders. Uses the
// message-height cache populated on the FIRST render. A fresh user message misses the cache on its
// first frame, so it has no bubble that frame — an acceptable single-frame delay since it just
// appeared.
void DrawUserBubbleBackground(const SmatchetThemeAiColors& aiPalette, std::uint64_t bodyKey,
                              const ImVec2& turnStartScreen) {
    const auto hit = s_messageHeightCache.find(bodyKey);
    if (hit != s_messageHeightCache.end() && hit->second > 0.0f) {
        const float labelH = ImGui::GetTextLineHeightWithSpacing();
        const float bubbleH = labelH + hit->second;
        const float bubbleW = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 bubbleColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(aiPalette.AiUserBubbleBg[0], aiPalette.AiUserBubbleBg[1],
                                                  aiPalette.AiUserBubbleBg[2], aiPalette.AiUserBubbleBg[3]));
        dl->AddRectFilled(turnStartScreen, ImVec2(turnStartScreen.x + bubbleW, turnStartScreen.y + bubbleH),
                          bubbleColor, ImGui::GetStyle().FrameRounding);
    }
}

void RenderHistoryTurn(AiHistoryDrawCtx& ctx, int messageIndex, const char* roleLabel, const ImVec4& roleColor,
                       AiMessage* msgPtr, const std::string& body, bool cacheable) {
    SMATCHET_UI_PERF_SCOPE("ai_chat.history.render_turn");
    const SmatchetThemeAiColors& aiPalette = ctx.palette;
    const ImVec2 turnStartScreen = ImGui::GetCursorScreenPos();
    const float turnStartCursorY = ImGui::GetCursorPosY(); // window-relative for Y-cache
    const bool isUser = msgPtr ? (msgPtr->Role == "user") : false;

    // Capture this turn's window-relative Y for the scroll-to-pinned latch.
    // Persistent messages only (streaming tail has no stable Y until finalised).
    if (messageIndex >= 0 && static_cast<std::size_t>(messageIndex) < ctx.messageYCache.size()) {
        ctx.messageYCache[messageIndex] = turnStartCursorY;
    }

    // Off-screen culling: only for cacheable (finalised) turns with a
    // known height. Streaming tail + unmeasured turns always render
    // fully so the height cache populates on first sight.
    std::uint64_t bodyKey = 0;
    const float actionRowSlotH = ImGui::GetFrameHeightWithSpacing() * 0.9f;
    if (TryCullHistoryTurn(body, cacheable, actionRowSlotH, bodyKey)) {
        return;
    }

    // For user messages: pre-draw the bubble bg via ImDrawList before the text renders.
    if (cacheable && isUser) {
        DrawUserBubbleBackground(aiPalette, bodyKey, turnStartScreen);
    }

    ImGui::BeginGroup();
    const ImVec2 labelPos = ImGui::GetCursorScreenPos();
    const float labelLineH = ImGui::GetTextLineHeight();
    ImGui::PushStyleColor(ImGuiCol_Text, roleColor);
    ImGui::TextUnformatted(roleLabel);
    ImGui::PopStyleColor();
    ImFont* labelFont = ImGui::GetFont();
    const float labelWidth = ImGui::CalcTextSize(roleLabel).x;
    SelectableText::RegisterSegment(ctx.selCtx, roleLabel, roleLabel + std::strlen(roleLabel), labelPos, labelLineH,
                                    labelFont, labelWidth, nullptr);
    SelectableText::EndBlock(ctx.selCtx);
    if (cacheable) {
        const MarkdownPreviewRender::PreviewPlan& plan = GetOrBuildPlanForMessage(body);
        MarkdownPreviewRender::RenderPlan(plan, ctx.renderOpts);
    } else {
        MarkdownPreviewRender::Render(body, ctx.renderOpts);
    }
    SelectableText::EndBlock(ctx.selCtx);
    ImGui::EndGroup();

    // Capture the realised height for future culling. Only for cacheable
    // turns (streaming tail's height changes per frame).
    if (cacheable) {
        const ImVec2 turnEndScreen = ImGui::GetCursorScreenPos();
        const float labelH = ImGui::GetTextLineHeightWithSpacing();
        const float realisedHeight = (turnEndScreen.y - turnStartScreen.y) - labelH;
        if (realisedHeight > 0.0f) {
            s_messageHeightCache[bodyKey] = realisedHeight;
        }
    }

    // Body hover/focus capture for the action-row alpha gate. Read AFTER
    // EndGroup so the IsItem* queries test the combined turn-group rect.
    // `ImGuiHoveredFlags_ChildWindows` is NOT valid for `IsItemHovered`
    // (it's a `IsWindowHovered` flag) — ImGui asserts "Invalid flags for
    // IsItemHovered()!" if we pass it. The group itself is the rect we
    // want to test; no flag needed.
    const bool bodyHovered = ImGui::IsItemHovered();
    const bool bodyFocused = ImGui::IsItemFocused();

    // -------- Always-reserved action row --------
    // Layout-invariant on hover/focus: the slot's height is reserved
    // unconditionally, so the message below doesn't shift when the row
    // appears. Alpha-gate (instead of conditional render) keeps the
    // buttons in tab order so Pillar 4 keyboard nav works regardless of
    // whether the user is mousing or tabbing.
    if (msgPtr != nullptr && messageIndex >= 0) {
        RenderTurnActionRow(ctx, messageIndex, msgPtr, actionRowSlotH, bodyHovered, bodyFocused);
    }
}

// Consume the scroll-to-pinned-bookmark latch. Resizes + resets the per-message
// Y-cache on a history-size change, then jumps the scroll-child to the target
// message's last-known Y when available. Extracted verbatim from DrawHistoryArea.
void ApplyScrollToPinnedLatch(UiDrawSession& d, std::vector<float>& messageYCache) {
    if (messageYCache.size() != d.assistantHistory.size()) {
        messageYCache.assign(d.assistantHistory.size(), -1.0f);
    }

    // Scroll-to-pinned latch — consumed once a frame at top of DrawHistoryArea.
    // The latch is set by either (a) a click on a pin-strip row, (b) the future
    // bookmark / command-palette jump from another path. If the Y-cache slot
    // is still sentinel `-1.0f` (message has never rendered — e.g. off-screen
    // above the cull window), leave the latch set; next frames populate the
    // cache as the message comes into view, and we re-attempt the scroll then.
    if (d.assistantScrollToMessageIndex >= 0) {
        const std::size_t targetIdx = static_cast<std::size_t>(d.assistantScrollToMessageIndex);
        if (targetIdx >= messageYCache.size()) {
            // Stale latch — index points past the current history (clear-chat
            // raced the click). Discard.
            d.assistantScrollToMessageIndex = -1;
        } else if (messageYCache[targetIdx] >= 0.0f) {
            ImGui::SetScrollY(messageYCache[targetIdx] - 4.0f);
            d.assistantScrollToMessageIndex = -1;
            // Releasing auto-scroll-to-tail pin since the user explicitly
            // navigated mid-history — pinning back to bottom on the next
            // frame would fight the bookmark jump.
            d.assistantAutoScrollAtTail = false;
        }
    }
}

void DrawHistoryArea(AppController& app, UiDrawSession& d, float availY) {
    SMATCHET_UI_PERF_SCOPE("ai_chat.history.draw");
    (void)app;
    const float headerH = ImGui::GetTextLineHeightWithSpacing() * 1.4f;
    const float inputH = ImGui::GetFrameHeightWithSpacing() * kInputRowsTall +
                         ImGui::GetFrameHeightWithSpacing() * 1.2f; // input + buttons row
    const float errorStripH = d.assistantLastError.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    // Pin strip is rendered ABOVE the history scroll-child when any msg is
    // pinned. Reserve its height from the body so the scroll-child shrinks
    // to fit. Re-compute the same height the strip uses internally to keep
    // the layouts in sync; this avoids a one-frame mismatch on the frame
    // a message is first pinned.
    const auto pinnedCount = std::count_if(d.assistantHistory.begin(), d.assistantHistory.end(),
                                           [](const AiMessage& m) { return m.Pinned; });
    constexpr int kMaxVisiblePinRows = 4;
    const int pinVisibleRows = smatchet::ai::AiPinStripVisibleRows(static_cast<long>(pinnedCount), kMaxVisiblePinRows);
    const float pinStripH = smatchet::ai::AiPinStripReservedHeight(pinVisibleRows, ImGui::GetFrameHeightWithSpacing(),
                                                                   ImGui::GetStyle().ItemSpacing.y);
    const float bodyH = smatchet::ai::AiHistoryBodyHeight(availY, headerH, inputH, errorStripH, pinStripH);

    DrawPinStripIfAny(d);

    // Slice 5 + perf Opt 1: rich-rendered selectable AI chat with per-message
    // BuildPlan / RenderPlan caching. Finalised history messages have
    // immutable content — their parsed plan is computed once + cached. Only
    // the streaming tail re-parses every frame.
    // Honor BeginChild's contract: when the history scroll-child is fully
    // collapsed/clipped (e.g. the omnibar shrinks the assistant panel below the
    // child's reserved height at small window sizes), BeginChild returns false
    // and sets SkipItems — every turn would otherwise render into a zero-height
    // clip rect, defeating the off-screen cull and paying per-turn hash + plan
    // cost for nothing. Skip the whole render body when not visible; the empty
    // child still needs its matching EndChild() below.
    const bool historyVisible = ImGui::BeginChild("##AiAssistantHistory", ImVec2(0.0f, bodyH), true);
    if (historyVisible) {
        // Phase 6 of ai-chat-claude-desktop-parity. Per-message Y-position cache
        // keyed by `messageIndex` (NOT content hash — review issue #3 fix). Used
        // by the scroll-to-pinned-bookmark latch to jump the scroll-child to the
        // message's last-known window-relative Y. Reset on history size change
        // (push_back grows; clear-chat / hydrate clears). Static is fine because
        // exactly one AI chat panel exists in the app at a time.
        static std::vector<float> s_messageYCache;
        ApplyScrollToPinnedLatch(d, s_messageYCache);

        auto& selCtx = SelectableText::Begin("##AiAssistantHistorySel");

        MarkdownPreviewRender::Options renderOpts;
        renderOpts.mode = MarkdownPreviewRender::Mode::Full;
        renderOpts.clickableLinks = true;
        renderOpts.existingSelCtx = &selCtx;

        // s_messageHeightCache + s_turnActiveLastFrame are file-scope (above) so the clear-history
        // action can drop them; the off-screen-cull height cache (Opt #4) and the show-on-hover alpha
        // gate read them exactly as before. Invalidate the height cache when the layout basis changes
        // (font size or wrap width) — cached heights are pixel layouts that go stale otherwise, which
        // would mis-cull. Cheap epoch compare: steady-state frames clear nothing.
        static float s_lastLayoutFontSize = 0.0f;
        static float s_lastLayoutWrapWidth = 0.0f;
        if (AiLayoutBasisChanged(ImGui::GetFontSize(), ImGui::GetContentRegionAvail().x, s_lastLayoutFontSize,
                                 s_lastLayoutWrapWidth)) {
            s_messageHeightCache.clear();
        }

        const SmatchetThemeAiColors& aiPalette = SmatchetTheme::GetActiveAiColors();
        AiHistoryDrawCtx ctx{
            d, aiPalette, selCtx, renderOpts, s_messageYCache, SmatchetAreFaIconsLoaded(), smatchet::ai::NowUnixMs()};

        const ImVec4 userColor(aiPalette.AiUserRoleLabel[0], aiPalette.AiUserRoleLabel[1], aiPalette.AiUserRoleLabel[2],
                               aiPalette.AiUserRoleLabel[3]);
        const ImVec4 asstColor(aiPalette.AiAssistantRoleLabel[0], aiPalette.AiAssistantRoleLabel[1],
                               aiPalette.AiAssistantRoleLabel[2], aiPalette.AiAssistantRoleLabel[3]);
        for (std::size_t i = 0; i < d.assistantHistory.size(); ++i) {
            AiMessage& m = d.assistantHistory[i];
            const bool isUser = (m.Role == "user");
            RenderHistoryTurn(ctx, static_cast<int>(i), isUser ? "You:" : "Assistant:", isUser ? userColor : asstColor,
                              &m, m.Content, /*cacheable=*/true);
            ImGui::Spacing();
        }
        if (d.assistantInFlight && !d.assistantStreamBuf.empty()) {
            // Streaming tail mutates per frame — bypass cache. No messageIndex
            // (msgPtr nullptr) → no Y-cache write, no action row (the tail isn't
            // finalised and has no persistence row to pin).
            RenderHistoryTurn(ctx, -1, "Assistant (streaming...):", asstColor, nullptr, d.assistantStreamBuf,
                              /*cacheable=*/false);
        }

        SelectableText::End(selCtx);

        // Tail tracking: update for next frame based on user's current scroll.
        // Auto-scroll-to-tail re-pins when the user is already at the bottom.
        const float scrollY = ImGui::GetScrollY();
        const float scrollMax = ImGui::GetScrollMaxY();
        const bool wasAtTail = d.assistantAutoScrollAtTail;
        d.assistantAutoScrollAtTail = (scrollMax <= 0.0f) || (scrollY >= scrollMax - 1.0f);
        if (wasAtTail && scrollMax > 0.0f && d.assistantScrollToMessageIndex < 0) {
            // Don't auto-pin to tail when a bookmark jump is queued for next frame
            // (e.g. Y-cache wasn't populated this frame); the bookmark wins.
            ImGui::SetScrollY(scrollMax);
        }
    }
    ImGui::EndChild();
}

void DrawErrorStrip(UiDrawSession& d) {
    if (d.assistantLastError.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
    ImGui::TextWrapped("%s", d.assistantLastError.c_str());
    ImGui::PopStyleColor();
}

void DrawContextBlockCheckboxes(UiDrawSession& d) {
    // Five-checkbox row driving the `cfg.AssistantContextBlock*` toggles shipped in
    // Phase A'. Changes persist to disk immediately so the next launch reflects the
    // user's pick; per-block defaults are all `true`.
    bool dirty = false;
    auto draw = [&](const char* label, bool& flag) {
        if (ImGui::Checkbox(label, &flag)) {
            dirty = true;
        }
    };
    draw("Selection##AiCtxSel", d.cfg.AssistantContextBlockSelection);
    ImGui::SameLine();
    draw("Visible##AiCtxVis", d.cfg.AssistantContextBlockVisibleRows);
    ImGui::SameLine();
    draw("Ticket##AiCtxTicket", d.cfg.AssistantContextBlockActiveTicket);
    ImGui::SameLine();
    draw("View##AiCtxView", d.cfg.AssistantContextBlockActiveView);
    ImGui::SameLine();
    draw("Audit##AiCtxAudit", d.cfg.AssistantContextBlockAuditTrail);
    if (dirty) {
        ScheduleConfigSaveDetached(d.cfg);
    }
}

std::vector<AiContextBlock> BuildSendContext(AppController& app, const UiDrawSession& d,
                                             const ViewDefinition* activeView) {
    AiContextBuilder::Inputs inputs;
    // AI auto-context targets the FOCUSED pane (ADR-0018 focused-pane semantics).
    const GridPane& pane = d.focusedPane();
    inputs.Tickets = app.GetActiveTicketsSnapshot();
    inputs.SortedIndices = &pane.cachedSortedIndices;
    inputs.SelectedRows = &pane.gridState.RectSel.Rows;
    inputs.ActiveIssueId = pane.gridState.ActiveIssueId;
    inputs.ActiveView = activeView;
    // VisibleRows = the same sort-order list (capped internally at kRowsCap). Phase B
    // intentionally does not yet plumb the precise top-of-viewport range — using the
    // sorted-index list gives a deterministic "top N rows" approximation matching the
    // grid's natural rendering order.
    inputs.VisibleRows = &pane.cachedSortedIndices;
    inputs.EnableSelection = d.cfg.AssistantContextBlockSelection;
    inputs.EnableVisibleRows = d.cfg.AssistantContextBlockVisibleRows;
    inputs.EnableActiveTicket = d.cfg.AssistantContextBlockActiveTicket;
    inputs.EnableActiveView = d.cfg.AssistantContextBlockActiveView;
    inputs.EnableAuditTrail = d.cfg.AssistantContextBlockAuditTrail;
    // UX pillar 2: BackendAuditTrail::ReadRecentEvents performs synchronous
    // SQLite + filesystem reads that must not run on the UI thread. The
    // sentinel body is replaced on the worker thread inside
    // AiAssistantController::RunRequest before the request is dispatched.
    inputs.DeferAuditTrailFetch = true;
    return AiContextBuilder::BuildAll(inputs);
}

// Direction-aware seed of the process-static char buffer from the model-side
// `assistantInputBuf`, running before InputTextMultiline draws. Extracted from
// DrawInputAndButtons; behaviour byte-identical. See AiAssistantInputSeedDecision.h.
void SeedInputBufferFromModel(UiDrawSession& d) {
    // Char buffer for InputTextMultiline lives at namespace scope so the
    // panel-open / panel-close path can drive dictation register / unregister
    // without exposing a getter. Mirroring through a std::string per-frame
    // keeps the rest of the codepaths (Send-button snapshot, Lua glue) free
    // of ImGui-specific resizable callbacks. Pre-seeded from
    // d.assistantInputBuf the first time the panel renders so Lua-supplied
    // text (Phase E) survives a panel reopen.
    // Re-seed when (a) first frame, or (b) the model-side `assistantInputBuf`
    // diverged from the char buffer (e.g. Lua glue poked it between frames,
    // or the panel was closed + reopened and external code edited the field).
    // Without this check, the char buffer kept the previous session's text
    // and Lua-supplied input on Phase E was silently lost.
    const std::size_t bufLen = std::strlen(s_inputCharBuf.data());
    const bool bytesDiffer = (d.assistantInputBuf.size() != bufLen) ||
                             (std::memcmp(s_inputCharBuf.data(), d.assistantInputBuf.data(), bufLen) != 0);
    // Direction-aware re-seed (see AiAssistantInputSeedDecision.h). The
    // decision is factored into a pure helper so the unit test can pin every
    // branch — the regression that motivated it was the Whisper dictation
    // router splicing text directly into `s_inputCharBuf` between frames
    // followed by the next frame unconditionally re-seeding from the still-
    // stale model, silently clobbering the splice.
    const bool willSeed = smatchet::ai::ShouldSeedAssistantInputFromModel(s_inputCharBufSeeded, bufLen,
                                                                          d.assistantInputBuf.size(), bytesDiffer);
    if (willSeed) {
        s_inputCharBufSeeded = true;
        const size_t copy = (std::min)(d.assistantInputBuf.size(), s_inputCharBuf.size() - 1);
        std::memcpy(s_inputCharBuf.data(), d.assistantInputBuf.data(), copy);
        s_inputCharBuf[copy] = '\0';
    }
}

// Drain the dictation router's pending-reload item id and ask ImGui to re-read
// the externally-spliced char buffer on the next InputText call. Extracted from
// DrawInputAndButtons; behaviour byte-identical.
void ProcessPendingDictationReload() {
    // Splice-reload bridge for Whisper dictation. The router calls
    // `InsertIntoFocusedInputText` from the MainThreadDispatcher drain at the
    // top of the frame; when the splice target is the AI Assistant chat input
    // AND that widget is currently the active ImGui item, ImGui's
    // `InputTextMultiline` below would silently overwrite the freshly-spliced
    // `s_inputCharBuf` with its stale internal `state->TextA` (see
    // imgui_widgets.cpp:4821 — without `WantReloadUserBuf` the active widget
    // ignores `buf`). The router records the target widget's item id; we
    // drain it here and ask ImGui to re-read `s_inputCharBuf` on the next
    // InputText call. `MoveToEnd` matches the post-splice cursor (`*Cursor`
    // is set to `insertAt + copyLen` by the router) so the caret lands after
    // the dictated text the user just spoke.
    const unsigned int pendingReloadId = g_dictationRouter.ConsumePendingReloadItemId();
    if (pendingReloadId != 0u) {
        // `ImGui::GetInputTextState` resolves through the
        // `#define ImGui SmatchetLocalizedImGui` macro + the
        // `using namespace ::ImGui;` in that namespace, so the underlying
        // call lands in the real `::ImGui::GetInputTextState`. Returns nullptr
        // when no input-text widget has been active for `id` (e.g. the panel
        // was just opened and the AI input has never received focus) — that's
        // a no-op for us: a never-active widget has no internal state to
        // overwrite the splice with.
        if (ImGuiInputTextState* state = ImGui::GetInputTextState(pendingReloadId)) {
            state->ReloadUserBufAndMoveToEnd();
        }
    }
}

// Measure the context the next turn would send, mapping each block's real body
// size into the pure consent inputs (security.md 2026-05-17 P2). Builds the same
// snapshot as BuildSendContext (audit-trail fetch deferred), so the modal shows
// the genuine byte counts that will leave the machine — not an estimate. The
// audit-trail body is the deferred sentinel here; its row is flagged deferred by
// BuildOutboundConsentRows and excluded from the displayed total.
smatchet::ai::OutboundConsentInputs MeasureOutboundConsentInputs(AppController& app, const UiDrawSession& d,
                                                                 const ViewDefinition* activeView) {
    smatchet::ai::OutboundConsentInputs in;
    in.EnableSelection = d.cfg.AssistantContextBlockSelection;
    in.EnableVisibleRows = d.cfg.AssistantContextBlockVisibleRows;
    in.EnableActiveTicket = d.cfg.AssistantContextBlockActiveTicket;
    in.EnableActiveView = d.cfg.AssistantContextBlockActiveView;
    in.EnableAuditTrail = d.cfg.AssistantContextBlockAuditTrail;
    const std::vector<AiContextBlock> blocks = BuildSendContext(app, d, activeView);
    for (std::vector<AiContextBlock>::const_iterator it = blocks.begin(); it != blocks.end(); ++it) {
        const std::size_t n = it->Body.size();
        if (it->Name == "selected_tickets") {
            in.SelectionBytes = n;
        } else if (it->Name == "visible_rows") {
            in.VisibleRowsBytes = n;
        } else if (it->Name == "active_ticket") {
            in.ActiveTicketBytes = n;
        } else if (it->Name == "active_view") {
            in.ActiveViewBytes = n;
        }
        // "audit_trail" body is the deferred sentinel — size unknown until the
        // worker fetches it at send time; shown as "fetched at send" in the modal.
    }
    return in;
}

// Dispatch a chat turn — snapshot the input + context, call Submit, and on a
// successful ack append the user message to history + arm autoscroll. Extracted
// verbatim from the former dispatchSend lambda in DrawInputAndButtons.
void DispatchAiSend(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                    AiAssistantController* ctrl) {
    // NOTE — the previous implementation flipped `assistantInFlight = true`
    // here unconditionally, BEFORE calling `ctrl->Submit(...)`. When Submit
    // short-circuited (no live client / queue shuttingDown), the flag
    // stayed true and the Send button stuck disabled for the rest of the
    // session — even after the user fixed their LLM setup in Preferences.
    // The fix: Submit now returns bool; we mutate UI state only after a
    // successful ack. On failure we surface a recoverable error strip and
    // leave the input buffer intact so the user can retry.
    if (ctrl == nullptr) {
        // Belt-and-braces — the sendDisabled predicate above already
        // gates this, but a future caller of `DispatchAiSend` might skip
        // the gate. Keep the early-out so the function is safe in
        // isolation.
        d.assistantLastError = "AI assistant unavailable — try again after restarting Smatchet.";
        return;
    }
    // First-send outbound-context consent gate (security.md 2026-05-17 P2). On a
    // fresh profile, before any provider POST, disclose exactly which workspace
    // blocks (and how many bytes) are about to leave the machine. Measure the
    // real context now, stash it for the modal, raise the modal, and return
    // WITHOUT dispatching — the input buffer is left intact. The modal's accept
    // button records consent (persisted) and re-enters DispatchAiSend, where the
    // gate now passes and the turn sends with the user's text still in place.
    if (smatchet::ai::ShouldRequireOutboundConsent(d.cfg.AssistantOutboundConsentShown)) {
        d.assistantConsentRows =
            smatchet::ai::BuildOutboundConsentRows(MeasureOutboundConsentInputs(app, d, activeView));
        ImGui::OpenPopup(kOutboundConsentPopupId);
        return;
    }
    const uint64_t turnGen = ++d.assistantTurnGen;
    const std::string snapshotInput = d.assistantInputBuf;
    std::vector<AiContextBlock> context = BuildSendContext(app, d, activeView);
    // Phase C: build the 5-block auto-context snapshot on the UI thread (where
    // all the source state lives) and pass it through Submit. The controller
    // then concatenates these + the agents.md prefix on the worker before
    // calling IAiClient::SendStreaming. Disabled blocks contribute empty bodies
    // — the controller skips empty-body entries when emitting tag wrappers.
    const bool accepted =
        ctrl->Submit(turnGen, snapshotInput, std::move(context), d.assistantPerTurnModel, d.assistantPerTurnEffort);
    if (!accepted) {
        // Submit rejected the turn (no live client or controller shutting
        // down). Roll back the turn-gen bump so a subsequent successful
        // Submit lands on a monotonically-fresh value, and surface a
        // user-facing recovery hint via assistantLastError. The input
        // buffer is left intact (the input mirror at the top of this
        // function keeps the typed text alive) so the user can retry
        // after fixing their provider configuration in Preferences.
        --d.assistantTurnGen;
        d.assistantLastError =
            "No active AI provider — open Preferences and pick a configured provider, then press Send again.";
        return;
    }
    d.assistantInFlight = true;
    d.assistantStreamBuf.clear();
    d.assistantLastError.clear();
    AiMessage userMsg;
    userMsg.Role = "user";
    userMsg.Content = d.assistantInputBuf;
    userMsg.CreatedAtUnixMs = smatchet::ai::NowUnixMs();
    // Phase 3 of ai-chat-claude-desktop-parity. Take an Append snapshot BEFORE
    // moving the message into the history vector — the worker copy survives
    // independently of the UI state. Grow `assistantHistoryRowIds` in lock-step
    // with `assistantHistory`; the worker's on-append callback backfills the
    // sentinel `-1` slot with the SQLite row id when the INSERT completes.
    AiMessage persistCopy = userMsg;
    const std::size_t newIdx = d.assistantHistory.size();
    d.assistantHistory.push_back(std::move(userMsg));
    d.assistantHistoryRowIds.push_back(-1);
    if (d.assistantHistoryHydrated) {
        // Coalescing Trim — successive Appends collapse into a single Trim in the worker queue, so
        // the cost is one O(N) erase on the worker side, not a SQLite DELETE per send.
        smatchet::ai::chat_persist::EnqueueAppendAndTrim(std::move(persistCopy), newIdx,
                                                         d.cfg.AssistantHistoryMaxRows);
    }
    d.assistantInputBuf.clear();
    std::memset(s_inputCharBuf.data(), 0, s_inputCharBuf.size());
    // NOTE — the `ctrl->Submit(...)` call was hoisted to the top of
    // `DispatchAiSend` (above) so the success/failure ack gates the
    // `assistantInFlight = true` flip + the history Append. Reaching this
    // point implies Submit returned true; nothing left to do other than
    // arm autoscroll.
    d.assistantAutoScrollAtTail = true;
}

// First-send outbound-context consent modal (security.md 2026-05-17 P2). Opened
// by DispatchAiSend's gate on the first turn of a fresh profile; the per-block
// rows were measured at open time and stashed in `d.assistantConsentRows`. The
// popup is OpenPopup-modal so it can't race the Send/input row behind it. On
// accept it records consent (persisted) and re-enters DispatchAiSend so the same
// turn dispatches; on cancel it leaves consent unset (modal re-shows next
// attempt) with the typed text intact.
void DrawOutboundConsentModal(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                              AiAssistantController* ctrl) {
    if (!ImGui::BeginPopupModal(kOutboundConsentPopupId, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextUnformatted("Smatchet is about to send context from your workspace to the configured AI provider.");
    ImGui::TextDisabled("This happens on every message. Toggle individual blocks with the context checkboxes.");
    ImGui::Spacing();
    ImGui::Separator();
    for (std::vector<smatchet::ai::OutboundConsentBlockRow>::const_iterator it = d.assistantConsentRows.begin();
         it != d.assistantConsentRows.end(); ++it) {
        if (!it->Enabled) {
            ImGui::TextDisabled("  [ ] %s  (off)", it->Name.c_str());
        } else if (it->DeferredFetch) {
            ImGui::Text("  [x] %s  (fetched at send)", it->Name.c_str());
        } else {
            ImGui::Text("  [x] %s  (~%zu bytes)", it->Name.c_str(), it->ApproxBytes);
        }
    }
    ImGui::Separator();
    const std::size_t total = smatchet::ai::SumOutboundConsentBytes(d.assistantConsentRows);
    ImGui::Text("Approx %zu bytes of context this turn", total);
    ImGui::SameLine();
    ImGui::TextDisabled("(audit-trail excluded; fetched on send if enabled)");
    if (ImGui::TreeNode("What gets sent")) {
        ImGui::TextWrapped("selected_tickets, visible_rows, active_ticket and active_view are pulled from the focused "
                           "grid pane — issue ids, summaries, statuses and field values already visible on screen. "
                           "audit_trail (off by default) additionally includes assignee emails and freeform comments. "
                           "Your global/project agents.md is prepended as the system prompt, and your typed message is "
                           "sent verbatim. Nothing else leaves this machine.");
        ImGui::TreePop();
    }
    ImGui::Spacing();
    if (ImGui::Button("Send and don't ask again", ImVec2(220, 0))) {
        d.cfg.AssistantOutboundConsentShown = true;
        ScheduleConfigSaveDetached(d.cfg);
        ImGui::CloseCurrentPopup();
        DispatchAiSend(app, d, activeView, ctrl);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Returns true when the user submitted (Enter pressed without Ctrl). Sends are dispatched here so
// the keyboard path matches the Send button click; the input buffer is cleared + focus restored.
bool DrawInputAndButtons(AppController& app, UiDrawSession& d, const ViewDefinition* activeView) {
    SeedInputBufferFromModel(d);

    const float inputH = ImGui::GetTextLineHeight() * kInputRowsTall;

    ProcessPendingDictationReload();

    // Pillar 4 (aspirational keyboard-nav): Enter sends, Ctrl+Enter inserts newline.
    // ImGuiInputTextFlags_CtrlEnterForNewLine inverts the default multiline Enter
    // semantics so a bare Enter submits and Ctrl+Enter inserts a line break — see
    // imgui.h flag docs. EnterReturnsTrue makes the call return true on submit.
    // CallbackResize lets us DETECT (not honour) an over-cap paste: a paste
    // bigger than the fixed buffer was previously clamped silently, losing the
    // dropped suffix with no feedback. The callback records the dropped byte
    // count; we toast it below. (aiassistant-silent-8kib-paste-truncation)
    const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                           ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_CallbackResize;
    const bool enterSubmitted =
        ImGui::InputTextMultiline("##AiAssistantInput", s_inputCharBuf.data(), s_inputCharBuf.size(),
                                  ImVec2(-1.0f, inputH), inputFlags, &InputBufferResizeCallback);
    // Mirror char-buf back into the string field every frame so the Send-button click
    // below + the Lua glue see the latest value with no separate poke.
    d.assistantInputBuf.assign(s_inputCharBuf.data());

    // Surface any paste/splice truncation the resize callback flagged this frame.
    // Naming the dropped length restores visibility (UX Pillar) — the input has a
    // hard 8 KB cap and the over-cap suffix is gone. Rounded to whole KB for the
    // message; the title is a plain translated string.
    if (s_inputTruncatedDroppedBytes > 0) {
        const int droppedKb = static_cast<int>((s_inputTruncatedDroppedBytes + 1023u) / 1024u); // round up, min 1 KB
        const int limitKb = kInputBufCap / 1024;
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("ai.paste_truncated.title", "Paste truncated"),
                                              SmatchetLocalization::Format("ai.paste_truncated.body",
                                                                           "%d KB dropped (input limit %d KB)",
                                                                           droppedKb, limitKb),
                                              ToastType::Warning, 5000);
        LOG_WARN("AiAssistantUi: paste truncated — %zu bytes dropped (input cap %d bytes).",
                 s_inputTruncatedDroppedBytes, kInputBufCap);
        s_inputTruncatedDroppedBytes = 0;
    }

    AiAssistantController* ctrl = app.HasAiAssistantController() ? &app.GetAiAssistantController() : nullptr;

    // Defense-in-depth recovery: if a prior turn left `assistantInFlight = true`
    // but the controller has since gone away (mid-shutdown, controller torn
    // down by a panel toggle, or the historical pre-fix bug where Submit
    // dropped a call without clearing the flag), force-clear here so the
    // user gets the Send button back on the next frame. The companion fix
    // in `DispatchAiSend` only flips the flag on a successful Submit ack —
    // this branch handles legacy stuck state + future-proofs against any
    // unrelated path that leaves the flag dangling.
    if (d.assistantInFlight && ctrl == nullptr) {
        LOG_WARN("AiAssistantUi: clearing stuck assistantInFlight (controller is gone).");
        d.assistantInFlight = false;
    }

    const bool sendDisabled =
        smatchet::ai::DecideSendDisabled(d.assistantInFlight, d.assistantInputBuf.empty(), ctrl != nullptr);

    auto dispatchSend = [&]() { DispatchAiSend(app, d, activeView, ctrl); };

    bool submittedByKey = false;
    if (enterSubmitted && !sendDisabled) {
        dispatchSend();
        // Re-focus the same multiline input so the user can keep typing without
        // clicking back in. -1 targets the previously-submitted item.
        ImGui::SetKeyboardFocusHere(-1);
        submittedByKey = true;
    }

    // Phase F — auto-send-on-punctuation hand-off. `s_pendingAutoSend` is set
    // by the dictation router callback (registered in
    // SmatchetDrawAiAssistantPanel) after a Whisper transcription lands on the
    // AI Assistant input AND ends with sentence-final punctuation. Defer the
    // dispatch by one frame (we observe the flag here, AFTER the InputText
    // call this frame, so the buffer mirror in `d.assistantInputBuf` is fresh).
    const bool pendingObserved = s_pendingAutoSend.exchange(false, std::memory_order_acq_rel);
    if (pendingObserved && !sendDisabled) {
        dispatchSend();
        ImGui::SetKeyboardFocusHere(-1);
        submittedByKey = true;
    }

    if (sendDisabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send")) {
        dispatchSend();
    }
    if (sendDisabled) {
        ImGui::EndDisabled();
    }

    if (d.assistantInFlight) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (ctrl) {
                ctrl->Cancel();
            }
        }
    }

    // Compact help so the keybinding is discoverable on first view.
    ImGui::SameLine();
    ImGui::TextDisabled("Enter sends, Ctrl+Enter = newline");

    // Render the first-send consent modal here so it shares this window's id
    // stack with the OpenPopup raised inside DispatchAiSend's gate. No-op until
    // the gate opens it.
    DrawOutboundConsentModal(app, d, activeView, ctrl);

    return submittedByKey;
}

// Header row 1: provider label + swap-side toggle + clear-chat button (with its
// confirm modal). Extracted verbatim from SmatchetDrawAiAssistantPanel.
void DrawAssistantHeaderRow(AppController& app, UiDrawSession& d) {
    // Header strip: provider + swap-side toggle.
    {
        AiAssistantController* ctrl = app.HasAiAssistantController() ? &app.GetAiAssistantController() : nullptr;
        const std::string provider = ctrl ? ctrl->GetActiveProviderName() : std::string();
        if (!provider.empty()) {
            ImGui::TextDisabled("(%s)", provider.c_str());
        } else {
            ImGui::TextDisabled("(no provider)");
        }
    }
    ImGui::SameLine();
    {
        // The swap button label inverts to telegraph the destination (where the panel
        // WILL move). When currently on the left primary side, the label reads "Right"
        // because clicking moves it to the right. Tooltip clarifies the action.
        const bool onRight = d.cfg.AssistantPanelOnSecondarySide;
        const char* swapLabel = onRight ? "<- Left" : "Right ->";
        if (ImGui::SmallButton(swapLabel)) {
            d.cfg.AssistantPanelOnSecondarySide = !onRight;
            d.assistantPendingSideSwap = true;
            // Moving to the right side bar slot requires the slot to actually be
            // visible; otherwise the dock node may be empty and the new tab has
            // nowhere to land. Mirror the existing View-menu toggle pattern so
            // users don't get a vanishing panel.
            if (d.cfg.AssistantPanelOnSecondarySide && !d.cfg.ShowSecondarySideBar) {
                d.cfg.ShowSecondarySideBar = true;
            }
            ScheduleConfigSaveDetached(d.cfg);
        }
        ImGui::SetItemTooltip(onRight ? "Move panel to the left primary side bar."
                                      : "Move panel to the right secondary side bar.");
    }
    ImGui::SameLine();
    {
        // Phase 7 of ai-chat-claude-desktop-parity. Clear-chat header button —
        // FA trash glyph when the icon font is loaded, short text fallback
        // otherwise. Opens a confirm popup (2-button) so a stray click can't
        // wipe the conversation. Confirm wipes both the in-memory vectors AND
        // the persisted SQLite rows via chat_persist::Op{ClearAll}; cancel is
        // a no-op. The confirm popup is OpenPopup-modal, so it can't race a
        // concurrent Send (the input row is behind it).
        const bool faLoaded = SmatchetAreFaIconsLoaded();
        const char* clearLabel = faLoaded ? ICON_FA_TRASH : "Clear";
        if (ImGui::SmallButton(clearLabel)) {
            ImGui::OpenPopup("##ConfirmClearChat");
        }
        ImGui::SetItemTooltip("Clear conversation history (asks for confirmation).");
        if (ImGui::BeginPopupModal("##ConfirmClearChat", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("Clear the entire conversation?");
            ImGui::TextDisabled("In-memory history + persisted SQLite rows are deleted. This cannot be undone.");
            ImGui::Spacing();
            if (ImGui::Button("Clear", ImVec2(120, 0))) {
                d.assistantHistory.clear();
                d.assistantHistoryRowIds.clear();
                d.assistantScrollToMessageIndex = -1;
                d.assistantStreamBuf.clear();
                d.assistantLastError.clear();
                // Drop the per-conversation render caches (plan / height / hover) so their
                // now-orphaned entries don't linger for the rest of the session (Phase 4).
                ClearAiRenderCaches();
                // Worker no-ops if chat_persist isn't running; safe to call.
                smatchet::ai::chat_persist::Op clearOp;
                clearOp.kind = smatchet::ai::chat_persist::OpKind::ClearAll;
                smatchet::ai::chat_persist::Enqueue(std::move(clearOp));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

// Header row 2: per-turn model + reasoning-effort overrides. Empty
// `assistantPerTurnModel` / `assistantPerTurnEffort` mean "use the Preferences
// default for the active provider". Extracted verbatim from
// SmatchetDrawAiAssistantPanel; provider/model/effort resolution now flows
// through the pure helpers in SmatchetAiAssistantUi_detail.h.
void DrawPerTurnModelEffortRow(UiDrawSession& d) {
    const AiProvider activeProvider = smatchet::ai::AiResolveProvider(d.cfg.AiProviderKind);
    const std::vector<smatchet::ai::ModelOption> catalog = smatchet::ai::KnownModels(activeProvider);
    // Display list is the default sentinel followed by each catalog model.
    // Picking the sentinel clears the per-turn override.
    std::vector<std::string> displayStrings;
    displayStrings.reserve(catalog.size() + 1);
    displayStrings.push_back(std::string("<default model>"));
    std::transform(catalog.begin(), catalog.end(), std::back_inserter(displayStrings),
                   [](const smatchet::ai::ModelOption& m) { return m.DisplayName; });
    std::vector<const char*> displayPtrs;
    displayPtrs.reserve(displayStrings.size());
    std::transform(displayStrings.begin(), displayStrings.end(), std::back_inserter(displayPtrs),
                   [](const std::string& s) { return s.c_str(); });

    // When the saved per-turn override doesn't match any entry in the active
    // provider's catalog (e.g. user switched providers leaving a stale id, or
    // typed a custom name from another build), fall back to free-form input
    // so the UI accurately reflects what the next Send will actually use —
    // showing `<default model>` while a hidden non-catalog override is still
    // active is a major UX trap (CodeRabbit comment 3255682299).
    std::vector<std::string> catalogIds;
    catalogIds.reserve(catalog.size());
    std::transform(catalog.begin(), catalog.end(), std::back_inserter(catalogIds),
                   [](const smatchet::ai::ModelOption& m) { return m.Id; });
    const smatchet::ai::AiModelComboResolution modelRes =
        smatchet::ai::AiResolveModelCombo(catalogIds, d.assistantPerTurnModel);
    int comboIdx = modelRes.comboIndex;

    ImGui::SetNextItemWidth(ImGui::GetTextLineHeight() * 12.0f);
    if (modelRes.useFreeformInput) {
        // Provider has no published catalog (Ollama variants). Free-form
        // InputText sized to look like the Combo above.
        char modelBuf[256] = {};
        std::snprintf(modelBuf, sizeof(modelBuf), "%s", d.assistantPerTurnModel.c_str());
        if (ImGui::InputTextWithHint("##AiTurnModel", "<default model>", modelBuf, sizeof(modelBuf))) {
            d.assistantPerTurnModel = modelBuf;
        }
        ImGui::SetItemTooltip("Per-turn model override. Leave blank to use the Preferences-saved value for this "
                              "provider.");
    } else {
        if (ImGui::Combo("##AiTurnModel", &comboIdx, displayPtrs.data(), static_cast<int>(displayPtrs.size()))) {
            if (comboIdx == 0) {
                d.assistantPerTurnModel.clear();
            } else {
                d.assistantPerTurnModel = catalog.at(static_cast<std::size_t>(comboIdx - 1)).Id;
            }
        }
        ImGui::SetItemTooltip("Per-turn model override. Pick <default model> to inherit the Preferences value.");
    }
    ImGui::SameLine();
    // Reasoning-effort Combo. Same 4-value enum as cfg.AiReasoningEffort.
    const char* kEffortLabels[] = {"<default effort>", "Low", "Medium", "High"};
    const char* kEffortIds[] = {"", "low", "medium", "high"};
    int effortIdx = smatchet::ai::AiEffortComboIndex(d.assistantPerTurnEffort);
    ImGui::SetNextItemWidth(ImGui::GetTextLineHeight() * 10.0f);
    if (ImGui::Combo("##AiTurnEffort", &effortIdx, kEffortLabels, 4)) {
        d.assistantPerTurnEffort = kEffortIds[effortIdx];
    }
    ImGui::SetItemTooltip("Per-turn OpenAI `reasoning_effort` override.");
    ImGui::SameLine();
    SmatchetHelpMarker::Render("ai.turn_effort.help",
                               "Per-turn reasoning effort. Applied as the OpenAI `reasoning_effort` parameter; "
                               "providers that don't understand the param ignore it.");
}

// Phase 7 of ai-chat-claude-desktop-parity. Copy-toast strip — ghosted 1-line
// confirmation set by the per-message Copy button, auto-dismissing on time-out.
// When not visible the slot is a plain Dummy of `toastRowH` so the input row
// above doesn't shift. Extracted verbatim from SmatchetDrawAiAssistantPanel.
void DrawCopyToastStrip(UiDrawSession& d, float toastRowH) {
    const std::int64_t nowMs = smatchet::ai::NowUnixMs();
    if (nowMs < d.assistantCopyToastUntilMs && !d.assistantCopyToastLabel.empty()) {
        // Fade out over the last 250 ms so the dismissal is less abrupt.
        const std::int64_t remainingMs = d.assistantCopyToastUntilMs - nowMs;
        constexpr std::int64_t kFadeOutMs = 250;
        const float alpha =
            smatchet::ai::AiToastAlpha(static_cast<long long>(remainingMs), static_cast<long long>(kFadeOutMs));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.75f, 0.65f, alpha));
        ImGui::TextUnformatted(d.assistantCopyToastLabel.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::Dummy(ImVec2(0.0f, toastRowH));
    }
}

// Register the chat-input buffer with the dictation router for the
// duration of the panel being open. Re-registering each frame is cheap
// (router treats the buf pointer as an idempotent key). Phase F — uses
// the AI-Assistant flavour so the post-insertion auto-send-on-punctuation
// check can identify this splice target. The callback is registered once
// (idempotent flag); it simply flips a static atomic that the next panel
// draw observes — keeps the work on the UI thread without re-entering
// ImGui state from the dispatcher drain context.
void RegisterAssistantDictationInput() {
    g_dictationRouter.RegisterAiAssistantInputText(s_inputCharBuf.data(), s_inputCharBuf.size(), nullptr);
    if (!s_autoSendCallbackRegistered) {
        s_autoSendCallbackRegistered = true;
        g_dictationRouter.SetAiAssistantSendCallback(
            []() { s_pendingAutoSend.store(true, std::memory_order_release); });
    }
}

// Pillar 2 (UI never freezes): dock-integrated window — ImGui's dock manager
// owns layout, sizing, and the resize/swap chrome. The panel attaches to the
// primary side bar (left) by default and migrates to the secondary side bar
// (right) when the user toggles the swap button. A pending-side request fires
// SetNextWindowDockID with ImGuiCond_Always so the move actually takes effect,
// otherwise FirstUseEver lets the user's saved imgui.ini state win.
void ApplyAssistantDocking(UiDrawSession& d, bool& needsReDock) {
    const ImGuiID primaryDockId = SmatchetDockNodeIds::kPrimarySideBar;
    const ImGuiID secondaryDockId = SmatchetDockNodeIds::kSecondarySideBar;
    const ImGuiID targetDockId = d.cfg.AssistantPanelOnSecondarySide ? secondaryDockId : primaryDockId;
    if (d.assistantPendingSideSwap || needsReDock) {
        ImGui::SetNextWindowDockID(targetDockId, ImGuiCond_Always);
        d.assistantPendingSideSwap = false;
        needsReDock = false;
    } else if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        ImGui::SetNextWindowDockID(targetDockId, ImGuiCond_FirstUseEver);
    }
}

} // namespace

void SmatchetDrawAiAssistantPanel(AppController& app, UiDrawSession& d, const ViewDefinition* activeView,
                                  bool embedded) {
    HydrateFromConfigOnce(app, d);
    // In embedded mode (dual-ui slice 4) the mobile AI page draws this body straight into the
    // mobile page child, so the open-state gate and the surrounding dock-window chrome are both
    // bypassed. The desktop path below stays unchanged from the pre-slice-4 flow.
    if (!embedded) {
        if (!d.assistantPanelOpen) {
            // Drop the chat-input registration with the dictation router so the
            // panel-closed state never receives transcribed text. The wrapper-level
            // hook would unregister on blur anyway, but the panel-level explicit
            // call is the belt to the wrapper's suspenders.
            g_dictationRouter.UnregisterInputText(s_inputCharBuf.data());
            // Persist a closed state at most once per close event (idempotent if already false).
            PersistOpenStateImmediate(d);
            return;
        }
    }
    RegisterAssistantDictationInput();

    if (!embedded) {
        static bool s_assistantNeedsReDock = false;
        ApplyAssistantDocking(d, s_assistantNeedsReDock);

        if (d.requestAssistantFocus) {
            ImGui::SetNextWindowFocus();
        }

        // The panel is now a dockable, resizable window — drop the floating-only flags
        // (NoDocking / NoSavedSettings / NoTitleBar / NoMove / NoResize). NoCollapse is
        // kept because dock-tab collapse fights the open/close persistence contract.
        const ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse;

        if (!ImGui::Begin("Smatchet Assistant", &d.assistantPanelOpen, kFlags)) {
            ImGui::End();
            if (d.requestAssistantFocus) {
                d.requestAssistantFocus = false;
            }
            // Panel is hidden — collapsed, or its docked tab is inactive. Intentionally do
            // NOT unregister the dictation buffer here — local-model
            // transcription can take multiple seconds, during which the user
            // may have flipped to another dock tab. Unregistering here would
            // leave the post-back with no target and silently drop the
            // transcribed text. The buffer stays alive (static storage); the
            // registration is dropped only when the panel actually closes
            // (`!d.assistantPanelOpen`) at the top of this function.
            PersistOpenStateImmediate(d);
            return;
        }
        if (d.requestAssistantFocus) {
            ImGui::SetWindowFocus();
            d.requestAssistantFocus = false;
        }
        if (!ImGui::IsWindowDocked() && !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
            s_assistantNeedsReDock = true;
        }
    }

    DrawAssistantHeaderRow(app, d);
    DrawPerTurnModelEffortRow(d);
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // Reserve a row for the per-block context checkboxes drawn just above the
    // input strip + a 1-line slot at the bottom of the panel for the Phase 7
    // copy-toast strip. Slot reserved unconditionally so the toast appearing /
    // disappearing never shifts the input row's Y position — the design plan's
    // explicit anti-jitter contract.
    const float checkboxRowH = ImGui::GetFrameHeightWithSpacing();
    const float toastRowH = ImGui::GetTextLineHeightWithSpacing();
    DrawHistoryArea(app, d, avail.y - checkboxRowH - toastRowH);
    DrawErrorStrip(d);
    DrawContextBlockCheckboxes(d);
    DrawInputAndButtons(app, d, activeView);

    DrawCopyToastStrip(d, toastRowH);

    if (!embedded) {
        ImGui::End();

        // Persist toggle changes (close X click, View-menu toggle externally) on the same
        // tick so the open/closed state survives an app crash mid-frame.
        PersistOpenStateImmediate(d);
    }
}

#endif // SMATCHET_WITH_AI
