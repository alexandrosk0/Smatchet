#include "SmatchetInlineError.h"

#include "SmatchetTheme.h"
#include "imgui.h"

#include <algorithm>
#include <string>

namespace smatchet {
namespace ui {

namespace {

#if defined(SMATCHET_BUILD_UI_TESTS)
struct DrawnRecord {
    std::string Text;
    const char* Kind = "";
    int Frame = -1000;
};
DrawnRecord g_drawn;

void Record(const std::string& text, const char* kind) {
    g_drawn.Text = text;
    g_drawn.Kind = kind;
    g_drawn.Frame = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0;
}
#else
void Record(const std::string&, const char*) {}
#endif

/// Room the overlay keeps clear of the window edge, and the gap under the field.
constexpr float kOverlayGapPx = 2.0f;
constexpr float kOverlayPadPx = 3.0f;
constexpr float kOverlayMinWrapPx = 240.0f;

// One boxed text under a field, painted on the foreground list so it never moves the layout: the error overlay (red
// border) and the hint (dim, no border) share it.
void OverlayBelowRect(const std::string& text, const ImVec2& rectMin, const ImVec2& rectMax, ImU32 textColor,
                      bool border) {
    const float wrap = (std::max)(rectMax.x - rectMin.x, kOverlayMinWrapPx);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
    const ImVec2 pos(rectMin.x, rectMax.y + kOverlayGapPx);
    ImDrawList* const list = ImGui::GetForegroundDrawList();
    const ImVec2 boxMin(pos.x - kOverlayPadPx, pos.y - 1.0f);
    const ImVec2 boxMax(pos.x + size.x + kOverlayPadPx, pos.y + size.y + 1.0f);
    list->AddRectFilled(boxMin, boxMax, ImGui::GetColorU32(ImGuiCol_PopupBg), 3.0f);
    if (border) {
        list->AddRect(boxMin, boxMax, textColor, 3.0f);
    }
    list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, textColor, text.c_str(), nullptr, wrap);
}

} // namespace

void DrawInlineParseError(const TrackerParseExplain::ParseMessage& msg) {
    if (!msg.IsError()) {
        return;
    }
    const std::string text = TrackerParseExplain::DescribeParseMessage(msg);
    if (text.empty()) {
        return;
    }
    Record(text, "error");
    ImGui::PushStyleColor(ImGuiCol_Text, SmatchetTheme::GetActiveSemanticColors().ErrorText);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

void DrawInlineParseHint(const std::string& text) {
    if (text.empty()) {
        return;
    }
    Record(text, "hint");
    ImGui::TextDisabled("%s", text.c_str());
}

void DrawInlineParseErrorBelowItem(const TrackerParseExplain::ParseMessage& msg) {
    DrawInlineParseErrorBelowRect(msg, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

void DrawInlineParseErrorBelowRect(const TrackerParseExplain::ParseMessage& msg, const ImVec2& rectMin,
                                   const ImVec2& rectMax) {
    if (!msg.IsError()) {
        return;
    }
    const std::string text = TrackerParseExplain::DescribeParseMessage(msg);
    if (text.empty()) {
        return;
    }
    Record(text, "overlay");
    OverlayBelowRect(text, rectMin, rectMax, ImGui::GetColorU32(SmatchetTheme::GetActiveSemanticColors().ErrorText),
                     true);
}

void DrawInlineTextErrorBelowRect(const std::string& text, const ImVec2& rectMin, const ImVec2& rectMax) {
    if (text.empty()) {
        return;
    }
    Record(text, "overlay");
    OverlayBelowRect(text, rectMin, rectMax, ImGui::GetColorU32(SmatchetTheme::GetActiveSemanticColors().ErrorText),
                     true);
}

void DrawInlineParseHintBelowRect(const std::string& text, const ImVec2& rectMin, const ImVec2& rectMax) {
    if (text.empty()) {
        return;
    }
    Record(text, "hint");
    OverlayBelowRect(text, rectMin, rectMax, ImGui::GetColorU32(ImGuiCol_TextDisabled), false);
}

#if defined(SMATCHET_BUILD_UI_TESTS)
std::string InlineErrorTextForTests() {
    const int now = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0;
    return (now - g_drawn.Frame) <= 1 ? g_drawn.Text : std::string();
}

std::string InlineErrorKindForTests() {
    const int now = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0;
    return (now - g_drawn.Frame) <= 1 ? std::string(g_drawn.Kind) : std::string();
}

void ResetInlineErrorForTests() { g_drawn = DrawnRecord(); }
#endif

} // namespace ui
} // namespace smatchet
