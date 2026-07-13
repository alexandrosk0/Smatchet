#include "SmatchetToast.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include <algorithm>

SmatchetToastManager& SmatchetToastManager::Instance() {
    static SmatchetToastManager instance;
    return instance;
}

void SmatchetToastManager::Push(const std::string& title, const std::string& message, ToastType type, int durationMs) {
    Push(title, message, type, durationMs, ToastRowAction());
}

void SmatchetToastManager::Push(const std::string& title, const std::string& message, ToastType type, int durationMs,
                                ToastRowAction rowAction) {
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
    m_toasts.push_back(std::move(toast));
    // Bound the live stack: sticky error toasts never expire, so a burst of failures
    // (e.g. one toast per failed bulk edit) would otherwise pile past the viewport top
    // and demand one dismissal click each. Oldest-first eviction — the evicted toast's
    // history entry and, for errors, the unread-error badge keep the trail visible.
    while (m_toasts.size() > kMaxLiveToasts) {
        m_toasts.erase(m_toasts.begin());
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
    PushBoundedHistory(m_history, std::move(entry), kHistoryCap);
}

bool SmatchetToastManager::ConsumeOpenCenterRequest() {
    const bool requested = m_openCenterRequested;
    m_openCenterRequested = false;
    return requested;
}

void SmatchetToastManager::Render() {
    if (m_toasts.empty()) return;

    auto now = std::chrono::steady_clock::now();

    // Remove expired toasts. Sticky toasts (errors) never auto-expire — they wait for an
    // explicit dismiss so a failure can't vanish unread.
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [&](const ToastNotification& t) { return !t.Sticky && now >= t.Expiry; }),
                   m_toasts.end());

    if (m_toasts.empty()) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;

    float padding = 20.0f;
    float toastWidth = 320.0f;
    float currentY = workPos.y + workSize.y - padding;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    int dismissIndex = -1; // applied after the loop (never mutate m_toasts mid-walk)

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
        if (!message || message[0] == '\0') toastHeight = titleSize.y + 20.0f;

        ImVec2 pos(workPos.x + workSize.x - toastWidth - padding, currentY - toastHeight);

        // Animation offset
        float offsetX = (1.0f - t.FadeIn) * 50.0f;
        pos.x += offsetX;

        // Click semantics (near-universal convention): clicking a toast DISMISSES it. An
        // error toast additionally opens the Notification Center, which keeps the failure
        // trail one click away without teleporting the user for routine info/success
        // toasts. clip=false: toasts draw on the foreground, outside any window clip rect.
        ImVec2 rectMax(pos.x + toastWidth, pos.y + toastHeight);
        const bool hovered = ImGui::IsMouseHoveringRect(pos, rectMax, false);
        // Hover-revealed close cross in the top-right corner.
        const float closeSize = 16.0f;
        ImVec2 closeMin(rectMax.x - closeSize - 6.0f, pos.y + 6.0f);
        ImVec2 closeMax(closeMin.x + closeSize, closeMin.y + closeSize);
        const bool closeHovered = hovered && ImGui::IsMouseHoveringRect(closeMin, closeMax, false);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (t.Type == ToastType::Error && !closeHovered) {
                m_openCenterRequested = true;
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

        ImVec4 accentColor;
        switch (t.Type) {
            case ToastType::Success: accentColor = SmatchetTheme::Colors::StatusDone; break;
            case ToastType::Error:   accentColor = SmatchetTheme::Colors::StatusBlocked; break;
            case ToastType::Warning: accentColor = SmatchetTheme::Colors::PriorityMedium; break;
            default:                accentColor = SmatchetTheme::Colors::StatusInProgress; break;
        }
        ImU32 accentU32 = ImGui::GetColorU32(ImVec4(accentColor.x, accentColor.y, accentColor.z, alpha));

        // Draw background
        dl->AddRectFilled(pos, ImVec2(pos.x + toastWidth, pos.y + toastHeight), bgCol, 6.0f);
        dl->AddRect(pos, ImVec2(pos.x + toastWidth, pos.y + toastHeight), borderCol, 6.0f);

        // Draw accent bar on the left
        dl->AddRectFilled(pos, ImVec2(pos.x + 4.0f, pos.y + toastHeight), accentU32, 6.0f, ImDrawFlags_RoundCornersLeft);

        // Draw Text
        ImU32 textCol = ImGui::GetColorU32(ImVec4(themeText.x, themeText.y, themeText.z, alpha));
        dl->AddText(ImVec2(pos.x + 15.0f, pos.y + 10.0f), textCol, title);

        if (message && message[0] != '\0') {
            ImU32 msgCol = ImGui::GetColorU32(ImVec4(themeText.x, themeText.y, themeText.z, 0.8f * alpha));
            dl->AddText(nullptr, 0.0f, ImVec2(pos.x + 15.0f, pos.y + 10.0f + titleSize.y + 5.0f), msgCol,
                        message, nullptr, wrapWidth);
        }

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

    if (dismissIndex >= 0 && dismissIndex < static_cast<int>(m_toasts.size())) {
        m_toasts.erase(m_toasts.begin() + dismissIndex);
    }
}





