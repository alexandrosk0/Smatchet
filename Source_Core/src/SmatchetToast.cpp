#include "SmatchetToast.h"
#include "SmatchetTheme.h"
#include <algorithm>

SmatchetToastManager& SmatchetToastManager::Instance() {
    static SmatchetToastManager instance;
    return instance;
}

void SmatchetToastManager::Push(const std::string& title, const std::string& message, ToastType type, int durationMs) {
    ToastNotification toast;
    toast.Title = title;
    toast.Message = message;
    toast.Type = type;
    toast.Expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    toast.FadeIn = 0.0f;
    m_toasts.push_back(std::move(toast));
}

void SmatchetToastManager::Render() {
    if (m_toasts.empty()) return;

    auto now = std::chrono::steady_clock::now();
    
    // Remove expired toasts
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(), [&](const ToastNotification& t) {
        return now >= t.Expiry;
    }), m_toasts.end());

    if (m_toasts.empty()) return;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 workPos = viewport->WorkPos;
    ImVec2 workSize = viewport->WorkSize;
    
    float padding = 20.0f;
    float toastWidth = 320.0f;
    float currentY = workPos.y + workSize.y - padding;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (int i = static_cast<int>(m_toasts.size()) - 1; i >= 0; --i) {
        auto& t = m_toasts[i];
        
        // Update fade
        t.FadeIn = (std::min)(1.0f, t.FadeIn + ImGui::GetIO().DeltaTime * 4.0f);
        
        // Calculate size
        ImVec2 titleSize = ImGui::CalcTextSize(t.Title.c_str());
        float wrapWidth = toastWidth - 30.0f;
        ImVec2 msgSize = ImGui::CalcTextSize(t.Message.c_str(), nullptr, false, wrapWidth);
        float toastHeight = titleSize.y + msgSize.y + 25.0f;
        if (t.Message.empty()) toastHeight = titleSize.y + 20.0f;

        ImVec2 pos(workPos.x + workSize.x - toastWidth - padding, currentY - toastHeight);
        
        // Animation offset
        float offsetX = (1.0f - t.FadeIn) * 50.0f;
        pos.x += offsetX;

        float alpha = t.FadeIn;
        auto timeRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(t.Expiry - now).count();
        if (timeRemaining < 500) alpha *= (timeRemaining / 500.0f);

        ImU32 bgCol = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.14f, 0.95f * alpha));
        ImU32 borderCol = ImGui::GetColorU32(ImVec4(0.3f, 0.3f, 0.35f, 0.5f * alpha));
        
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
        ImU32 textCol = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
        dl->AddText(ImVec2(pos.x + 15.0f, pos.y + 10.0f), textCol, t.Title.c_str());
        
        if (!t.Message.empty()) {
            ImU32 msgCol = ImGui::GetColorU32(ImVec4(0.8f, 0.8f, 0.85f, 0.8f * alpha));
            dl->AddText(nullptr, 0.0f, ImVec2(pos.x + 15.0f, pos.y + 10.0f + titleSize.y + 5.0f), msgCol, t.Message.c_str(), nullptr, wrapWidth);
        }

        currentY -= (toastHeight + 10.0f);
    }
}






