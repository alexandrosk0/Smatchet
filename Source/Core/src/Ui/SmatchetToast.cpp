#include "SmatchetToast.h"
#include "TunableLimits.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "imgui_internal.h" // BringWindowToDisplayFront — toast hit-window stays above all windows
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

std::size_t SmatchetToastManager::MaxLiveToasts() {
    return static_cast<std::size_t>(smatchet::limits::Get().ToastMaxLive);
}

std::size_t SmatchetToastManager::HistoryCap() {
    return static_cast<std::size_t>(smatchet::limits::Get().ToastHistoryMax);
}

SmatchetToastManager& SmatchetToastManager::Instance() {
    static SmatchetToastManager instance;
    return instance;
}

void SmatchetToastManager::Push(const std::string& title, const std::string& message, ToastType type, int durationMs) {
    PushImpl(title, message, type, durationMs, ToastRowAction(), false, 0);
}

void SmatchetToastManager::PushClickable(const std::string& title, const std::string& message, ToastType type,
                                         int durationMs, ToastRowAction rowAction) {
    PushImpl(title, message, type, durationMs, std::move(rowAction), true, 0);
}

void SmatchetToastManager::Push(const std::string& title, const std::string& message, ToastType type, int durationMs,
                                ToastRowAction rowAction) {
    PushImpl(title, message, type, durationMs, std::move(rowAction), false, 0);
}

std::uint64_t SmatchetToastManager::PushUndoable(const std::string& title, const std::string& message, int durationMs,
                                                 std::function<void()> undo, std::function<void()> commit) {
    const std::uint64_t undoId = m_undo.Add(std::move(undo), std::move(commit));
    PushImpl(title, message, ToastType::Success, durationMs, ToastRowAction(), false, undoId);
    return undoId;
}

bool SmatchetToastManager::CommitUndo(std::uint64_t undoId) {
    if (undoId == 0) {
        return false;
    }
    for (ToastNotification& t : m_toasts) {
        if (t.UndoId == undoId) {
            t.UndoId = 0;
        }
    }
    return m_undo.Commit(undoId);
}

void SmatchetToastManager::PushImpl(const std::string& title, const std::string& message, ToastType type,
                                    int durationMs, ToastRowAction rowAction, bool runRowActionOnClick,
                                    std::uint64_t undoId) {
    ToastNotification toast;
    toast.Title = title;
    toast.Message = message;
    toast.Type = type;
    toast.Expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    toast.FadeIn = 0.0f;
    // Errors never auto-expire: a failure that fades on the "Copied to clipboard" schedule
    // can vanish before it is read. Sticky toasts stay until the user dismisses them.
    toast.Sticky = (type == ToastType::Error);
    toast.RowAction = rowAction;
    toast.RunRowActionOnClick = runRowActionOnClick;
    toast.UndoId = undoId;
    m_toasts.push_back(std::move(toast));
    // Bound the live stack: sticky error toasts never expire, so a burst of failures
    // (e.g. one toast per failed bulk edit) would otherwise pile past the viewport top
    // and demand one dismissal click each. Oldest-first eviction — the evicted toast's
    // history entry and, for errors, the unread-error badge keep the trail visible. An
    // evicted undoable toast commits its action (its Undo button is gone).
    const std::size_t maxLive = MaxLiveToasts();
    while (m_toasts.size() > maxLive) {
        const std::uint64_t evictedUndo = m_toasts.front().UndoId;
        m_toasts.erase(m_toasts.begin());
        m_undo.Commit(evictedUndo);
    }
    if (type == ToastType::Error) {
        ++m_unreadErrorCount;
    }

    // Every toast is retained in the bounded session history (the transient fades; this persists).
    ToastHistoryEntry entry;
    entry.CreatedAt = std::chrono::system_clock::now();
    entry.Title = title;
    entry.Message = message;
    entry.Type = type;
    entry.RowAction = std::move(rowAction);
    PushBoundedHistory(m_history, std::move(entry), HistoryCap());
}

bool SmatchetToastManager::UndoNewest() {
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t i = m_toasts.size(); i > 0; --i) {
        const ToastNotification& t = m_toasts[i - 1];
        if (t.UndoId == 0 || (!t.Sticky && now >= t.Expiry)) {
            continue;
        }
        const std::uint64_t undoId = t.UndoId;
        m_toasts.erase(m_toasts.begin() + static_cast<std::ptrdiff_t>(i - 1));
        return m_undo.Undo(undoId);
    }
    return false;
}

void SmatchetToastManager::CommitPendingUndos() {
    for (ToastNotification& t : m_toasts) {
        t.UndoId = 0;
    }
    m_undo.CommitAll();
}

std::size_t SmatchetToastManager::LiveUndoableCount() const {
    const auto now = std::chrono::steady_clock::now();
    std::size_t n = 0;
    for (const ToastNotification& t : m_toasts) {
        if (t.UndoId != 0 && (t.Sticky || now < t.Expiry)) {
            ++n;
        }
    }
    return n;
}

void SmatchetToastManager::DismissAllLive() {
    std::vector<std::uint64_t> undoIds;
    for (const ToastNotification& t : m_toasts) {
        if (t.UndoId != 0) {
            undoIds.push_back(t.UndoId);
        }
    }
    m_toasts.clear();
    for (const std::uint64_t id : undoIds) {
        m_undo.Commit(id);
    }
}

namespace {

// ImGui RenderPre hook: the frame is complete, so io.WantTextInput reflects every widget that drew.
void ToastUndoShortcutHook(ImGuiContext*, ImGuiContextHook* hook) {
    static_cast<SmatchetToastManager*>(hook->UserData)->ResolveUndoShortcut();
}

// ImGui Shutdown hook: the context is going away, and a context made later may get the same address.
void ToastUndoShutdownHook(ImGuiContext*, ImGuiContextHook* hook) {
    static_cast<SmatchetToastManager*>(hook->UserData)->ForgetUndoHookContext();
}

} // namespace

void SmatchetToastManager::HandleUndoShortcut() {
    ImGuiContext* const ctx = ImGui::GetCurrentContext();
    if (ctx != nullptr && m_undoHookedContext != ctx) {
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_RenderPre;
        hook.Callback = &ToastUndoShortcutHook;
        hook.UserData = this;
        ImGui::AddContextHook(ctx, &hook);
        ImGuiContextHook shutdownHook;
        shutdownHook.Type = ImGuiContextHookType_Shutdown;
        shutdownHook.Callback = &ToastUndoShutdownHook;
        shutdownHook.UserData = this;
        ImGui::AddContextHook(ctx, &shutdownHook);
        m_undoHookedContext = ctx;
    }
    // Only noted here: a text editor draws after this toast pass and sets io.WantTextInput as it does,
    // so the answer to "does a text field have the keyboard" is known at the end of the frame.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
        m_undoKeyPressed = true;
    }
}

void SmatchetToastManager::ForgetUndoHookContext() {
    m_undoHookedContext = nullptr;
    m_undoKeyPressed = false;
}

void SmatchetToastManager::ResolveUndoShortcut() {
    if (!m_undoKeyPressed) {
        return;
    }
    m_undoKeyPressed = false;
    // Ctrl+Z belongs to a text field while one is being typed in, and to a modal while one is up.
    if (ImGui::GetIO().WantTextInput || ImGui::GetTopMostPopupModal() != nullptr) {
        return;
    }
    UndoNewest();
}

bool SmatchetToastManager::HasLive(const std::string& title, const std::string& message) const {
    // A toast past its time is still in the list until the next Render() removes it. It is not on
    // screen any more, so it does not count.
    const auto now = std::chrono::steady_clock::now();
    for (const ToastNotification& t : m_toasts) {
        if (!t.Sticky && now >= t.Expiry) {
            continue;
        }
        if (t.Title == title && t.Message == message) {
            return true;
        }
    }
    return false;
}

bool SmatchetToastManager::ConsumeOpenCenterRequest() {
    const bool requested = m_openCenterRequested;
    m_openCenterRequested = false;
    return requested;
}

namespace {

// Geometry of one toast's Undo button, bottom-right inside the toast.
struct ToastUndoButton {
    bool Present = false;
    ImVec2 Min;
    ImVec2 Max;
    const char* Label = "";
    const char* KeyHint = "";
};

ToastUndoButton LayoutToastUndoButton(bool present, const ImVec2& rectMax) {
    ToastUndoButton b;
    if (!present) {
        return b;
    }
    b.Present = true;
    b.Label = SmatchetLocalization::T("toast.undo", "Undo");
    b.KeyHint = SmatchetLocalization::T("toast.undo_key", "Ctrl+Z");
    const ImVec2 labelSize = ImGui::CalcTextSize(b.Label);
    const ImVec2 size(labelSize.x + 16.0f, labelSize.y + 6.0f);
    b.Max = ImVec2(rectMax.x - 10.0f, rectMax.y - 8.0f);
    b.Min = ImVec2(b.Max.x - size.x, b.Max.y - size.y);
    return b;
}

// Height the Undo row adds under the message.
float ToastUndoRowHeight(bool present) { return present ? ImGui::GetTextLineHeight() + 12.0f : 0.0f; }

// Painted on the foreground list with the rest of the toast (the hit window under it draws nothing).
void PaintToastUndoButton(ImDrawList* dl, const ToastUndoButton& b, bool hovered, float alpha, const ImVec4& text) {
    if (!b.Present) {
        return;
    }
    const ImVec4 base = ImGui::GetStyleColorVec4(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    dl->AddRectFilled(b.Min, b.Max, ImGui::GetColorU32(ImVec4(base.x, base.y, base.z, base.w * alpha)), 4.0f);
    const ImVec2 labelSize = ImGui::CalcTextSize(b.Label);
    const ImU32 textCol = ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, alpha));
    dl->AddText(ImVec2(b.Min.x + (b.Max.x - b.Min.x - labelSize.x) * 0.5f, b.Min.y + 3.0f), textCol, b.Label);
    const ImVec2 hintSize = ImGui::CalcTextSize(b.KeyHint);
    const ImU32 hintCol = ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, 0.6f * alpha));
    dl->AddText(ImVec2(b.Min.x - hintSize.x - 8.0f, b.Min.y + 3.0f), hintCol, b.KeyHint);
}

ImVec4 ToastAccentColor(ToastType type) {
    switch (type) {
    case ToastType::Success:
        return SmatchetTheme::Colors::StatusDone;
    case ToastType::Error:
        return SmatchetTheme::Colors::StatusBlocked;
    case ToastType::Warning:
        return SmatchetTheme::Colors::PriorityMedium;
    default:
        return SmatchetTheme::Colors::StatusInProgress;
    }
}

// What the hit window saw this frame for one toast.
struct ToastHit {
    bool Hovered = false;
    bool CloseHovered = false;
    bool BodyClicked = false;
    bool UndoHovered = false;
    bool UndoClicked = false;
};

// Input CAPTURE (P2-H6): the visuals paint on the foreground draw list, but hit-testing goes through a
// real (invisible) ImGui window pinned over the toast rect and brought to the display front — so a
// dismissal click is consumed by the toast instead of ALSO landing on whatever grid row / button /
// scrollbar sits underneath, and hover no longer highlights widgets through the toast. The Undo button
// is a second invisible button over the body; the body allows the overlap, so the button takes the
// click.
ToastHit HitTestToast(int index, const ImVec2& pos, const ImVec2& size, const ImVec2& closeMin, const ImVec2& closeMax,
                      const ToastUndoButton& undo) {
    ToastHit hit;
    char hitId[40];
    std::snprintf(hitId, sizeof(hitId), "##toast_hit_%d", index);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    const ImGuiWindowFlags hitFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin(hitId, nullptr, hitFlags)) {
        // Keep the hit window above every other window so clicks over the toast
        // always route here (the painted toast is on the foreground list anyway).
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (undo.Present) {
            ImGui::SetNextItemAllowOverlap();
        }
        hit.BodyClicked = ImGui::InvisibleButton("body", size);
        hit.Hovered = ImGui::IsItemHovered();
        if (undo.Present) {
            ImGui::SetCursorScreenPos(undo.Min);
            hit.UndoClicked = ImGui::InvisibleButton("undo", ImVec2(undo.Max.x - undo.Min.x, undo.Max.y - undo.Min.y));
            hit.UndoHovered = ImGui::IsItemHovered();
            hit.Hovered = hit.Hovered || hit.UndoHovered;
        }
        hit.CloseHovered = hit.Hovered && ImGui::IsMouseHoveringRect(closeMin, closeMax, false);
        if (hit.Hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    return hit;
}

} // namespace

void SmatchetToastManager::Render() {
    if (m_toasts.empty())
        return;

    auto now = std::chrono::steady_clock::now();

    // Remove expired toasts. Sticky toasts (errors) never auto-expire — they wait for an
    // explicit dismiss so a failure can't vanish unread. An expired undoable toast commits
    // its action; the commits run after the erase because a commit may push a toast.
    std::vector<std::uint64_t> expiredUndos;
    for (const ToastNotification& t : m_toasts) {
        if (!t.Sticky && now >= t.Expiry && t.UndoId != 0) {
            expiredUndos.push_back(t.UndoId);
        }
    }
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [&](const ToastNotification& t) { return !t.Sticky && now >= t.Expiry; }),
                   m_toasts.end());
    for (const std::uint64_t id : expiredUndos) {
        m_undo.Commit(id);
    }

    HandleUndoShortcut();
    if (m_toasts.empty())
        return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    float padding = 20.0f;
    float toastWidth = 320.0f;
    float currentY = workPos.y + workSize.y - padding;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    int dismissIndex = -1;      // applied after the loop (never mutate m_toasts mid-walk)
    int undoIndex = -1;         // likewise: the Undo button was pressed on this toast
    ToastRowAction clickAction; // run after the loop, so it may push toasts

    for (int i = static_cast<int>(m_toasts.size()) - 1; i >= 0; --i) {
        auto& t = m_toasts[i];
        const char* title = SmatchetLocalization::TranslateSource(t.Title.c_str());
        const char* message = SmatchetLocalization::TranslateSource(t.Message.c_str());

        // Update fade
        t.FadeIn = (std::min)(1.0f, t.FadeIn + ImGui::GetIO().DeltaTime * 4.0f);

        // Calculate size
        ImVec2 titleSize = ImGui::CalcTextSize(title);
        float wrapWidth = toastWidth - 30.0f;
        ImVec2 msgSize = ImGui::CalcTextSize(message, nullptr, false, wrapWidth);
        float toastHeight = titleSize.y + msgSize.y + 25.0f;
        if (!message || message[0] == '\0')
            toastHeight = titleSize.y + 20.0f;
        toastHeight += ToastUndoRowHeight(t.UndoId != 0);

        ImVec2 pos(workPos.x + workSize.x - toastWidth - padding, currentY - toastHeight);

        // Animation offset
        float offsetX = (1.0f - t.FadeIn) * 50.0f;
        pos.x += offsetX;

        // Click semantics (near-universal convention): clicking a toast DISMISSES it. An
        // error toast additionally opens the Notification Center, which keeps the failure
        // trail one click away without teleporting the user for routine info/success
        // toasts. The Undo button undoes instead of dismissing.
        ImVec2 rectMax(pos.x + toastWidth, pos.y + toastHeight);
        const float closeSize = 16.0f;
        ImVec2 closeMin(rectMax.x - closeSize - 6.0f, pos.y + 6.0f);
        ImVec2 closeMax(closeMin.x + closeSize, closeMin.y + closeSize);
        const ToastUndoButton undoButton = LayoutToastUndoButton(t.UndoId != 0, rectMax);
        const ToastHit hit = HitTestToast(i, pos, ImVec2(toastWidth, toastHeight), closeMin, closeMax, undoButton);
        const bool hovered = hit.Hovered;
        const bool closeHovered = hit.CloseHovered;
        if (hit.UndoClicked) {
            undoIndex = i;
        } else if (hit.BodyClicked) {
            if (t.Type == ToastType::Error && !closeHovered) {
                m_openCenterRequested = true;
            }
            if (t.RunRowActionOnClick && !closeHovered) {
                clickAction = t.RowAction;
            }
            dismissIndex = i;
        }

        float alpha = t.FadeIn;
        if (!t.Sticky) {
            auto timeRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(t.Expiry - now).count();
            if (timeRemaining < 500)
                alpha *= (timeRemaining / 500.0f);
        }

        // Surface colors come from the ACTIVE theme (popup bg / border / text), so a light
        // or high-contrast theme yields a matching toast instead of a hardcoded dark box.
        // Only the semantic accent bar keeps its fixed status color.
        const ImVec4 themeBg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        const ImVec4 themeBorder = ImGui::GetStyleColorVec4(ImGuiCol_Border);
        const ImVec4 themeText = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImU32 bgCol = ImGui::GetColorU32(ImVec4(themeBg.x, themeBg.y, themeBg.z, 0.95f * alpha));
        ImU32 borderCol =
            ImGui::GetColorU32(ImVec4(themeBorder.x, themeBorder.y, themeBorder.z, themeBorder.w * alpha));

        const ImVec4 accentColor = ToastAccentColor(t.Type);
        ImU32 accentU32 = ImGui::GetColorU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, alpha));

        // Draw background
        dl->AddRectFilled(pos, ImVec2(pos.x + toastWidth, pos.y + toastHeight), bgCol, 6.0f);
        dl->AddRect(pos, ImVec2(pos.x + toastWidth, pos.y + toastHeight), borderCol, 6.0f);

        // Draw accent bar on the left
        dl->AddRectFilled(pos, ImVec2(pos.x + 4.0f, pos.y + toastHeight), accentU32, 6.0f,
                          ImDrawFlags_RoundCornersLeft);

        // Draw Text
        ImU32 textCol = ImGui::GetColorU32(ImVec4(themeText.x, themeText.y, themeText.z, alpha));
        dl->AddText(ImVec2(pos.x + 15.0f, pos.y + 10.0f), textCol, title);

        if (message && message[0] != '\0') {
            ImU32 msgCol = ImGui::GetColorU32(ImVec4(themeText.x, themeText.y, themeText.z, 0.8f * alpha));
            dl->AddText(nullptr, 0.0f, ImVec2(pos.x + 15.0f, pos.y + 10.0f + titleSize.y + 5.0f), msgCol, message,
                        nullptr, wrapWidth);
        }
        PaintToastUndoButton(dl, undoButton, hit.UndoHovered, alpha, themeText);

        // Close cross, drawn as two strokes (no font dependency), revealed on hover.
        if (hovered) {
            const float inset = 4.0f;
            const float crossAlpha = closeHovered ? alpha : 0.6f * alpha;
            ImU32 crossCol = ImGui::GetColorU32(ImVec4(themeText.x, themeText.y, themeText.z, crossAlpha));
            dl->AddLine(ImVec2(closeMin.x + inset, closeMin.y + inset), ImVec2(closeMax.x - inset, closeMax.y - inset),
                        crossCol, 1.5f);
            dl->AddLine(ImVec2(closeMax.x - inset, closeMin.y + inset), ImVec2(closeMin.x + inset, closeMax.y - inset),
                        crossCol, 1.5f);
        }

        currentY -= (toastHeight + 10.0f);
    }

    if (undoIndex >= 0 && undoIndex < static_cast<int>(m_toasts.size())) {
        const std::uint64_t undoId = m_toasts[static_cast<std::size_t>(undoIndex)].UndoId;
        m_toasts.erase(m_toasts.begin() + undoIndex);
        m_undo.Undo(undoId);
    } else if (dismissIndex >= 0 && dismissIndex < static_cast<int>(m_toasts.size())) {
        const std::uint64_t undoId = m_toasts[static_cast<std::size_t>(dismissIndex)].UndoId;
        m_toasts.erase(m_toasts.begin() + dismissIndex);
        m_undo.Commit(undoId);
    }
    if (clickAction) {
        clickAction();
    }
}
