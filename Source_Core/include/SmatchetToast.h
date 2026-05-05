#pragma once

#include <string>
#include <vector>
#include <chrono>
#include "imgui.h"

enum class ToastType {
    Info,
    Success,
    Warning,
    Error
};

struct ToastNotification {
    std::string Title;
    std::string Message;
    ToastType Type = ToastType::Info;
    std::chrono::steady_clock::time_point Expiry;
    float FadeIn = 0.0f; // 0..1
};

class SmatchetToastManager {
public:
    static SmatchetToastManager& Instance();

    void Push(const std::string& title, const std::string& message, ToastType type = ToastType::Info, int durationMs = 4000);
    void Render();

private:
    std::vector<ToastNotification> m_toasts;
};






