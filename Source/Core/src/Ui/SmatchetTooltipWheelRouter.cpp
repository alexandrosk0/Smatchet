#include "Ui/SmatchetTooltipWheelRouter.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>

namespace {

bool WindowIsUsableTooltipPart(ImGuiWindow* window, bool includePreviousFrame) {
    return window && !window->Hidden && (window->Active || (includePreviousFrame && window->WasActive));
}

bool HasTooltipAncestor(ImGuiWindow* window, bool includePreviousFrame) {
    for (ImGuiWindow* w = window; w; w = w->ParentWindow) {
        if ((w->Flags & ImGuiWindowFlags_Tooltip) != 0 && WindowIsUsableTooltipPart(w, includePreviousFrame)) {
            return true;
        }
    }
    return false;
}

ImGuiWindow* FindVerticallyScrollableTooltipOwner(ImGuiWindow* window, bool includePreviousFrame) {
    if (!WindowIsUsableTooltipPart(window, includePreviousFrame) || !HasTooltipAncestor(window, includePreviousFrame)) {
        return nullptr;
    }

    for (ImGuiWindow* w = window; w; w = w->ParentWindow) {
        if (WindowIsUsableTooltipPart(w, includePreviousFrame) && w->ScrollMax.y > 0.0f) {
            return w;
        }
    }
    return nullptr;
}

ImGuiWindow* FindScrollableTooltipOwner(ImGuiContext* g, bool includePreviousFrame) {
    if (!g) {
        return nullptr;
    }

    for (int i = g->Windows.Size - 1; i >= 0; --i) {
        ImGuiWindow* owner = FindVerticallyScrollableTooltipOwner(g->Windows[i], includePreviousFrame);
        if (owner) {
            return owner;
        }
    }
    return nullptr;
}

float WindowVerticalWheelStep(ImGuiWindow* window) {
    if (!window) {
        return 0.0f;
    }
    const float maxStep = window->InnerRect.GetHeight() * 0.67f;
    return std::floor((std::min)(5.0f * window->FontRefSize, maxStep));
}

void ApplyVerticalWheelToTooltip(ImGuiWindow* tooltipOwner, float wheelY) {
    if (!tooltipOwner || std::abs(wheelY) <= 0.0f || tooltipOwner->ScrollMax.y <= 0.0f) {
        return;
    }

    const float step = WindowVerticalWheelStep(tooltipOwner);
    if (step > 0.0f) {
        ImGui::SetScrollY(tooltipOwner, tooltipOwner->Scroll.y - wheelY * step);
    }
}

void RestoreWheelFromNonTooltipWindow(ImGuiContext* g, ImGuiWindow* tooltipOwner, float wheelY) {
    if (!g || !g->WithinFrameScope || !tooltipOwner || std::abs(wheelY) <= 0.0f) {
        return;
    }

    ImGuiWindow* wheelOwner = g->WheelingWindow;
    if (!wheelOwner || wheelOwner == tooltipOwner || HasTooltipAncestor(wheelOwner, true) ||
        wheelOwner->ScrollMax.y <= 0.0f) {
        return;
    }

    const float step = WindowVerticalWheelStep(wheelOwner);
    if (step > 0.0f) {
        ImGui::SetScrollY(wheelOwner, wheelOwner->Scroll.y + wheelY * step);
    }
}

} // namespace

namespace smatchet {
namespace ui {

bool RouteWheelToScrollableTooltip() {
    ImGuiContext* g = ImGui::GetCurrentContext();
    ImGuiWindow* tooltipOwner = FindScrollableTooltipOwner(g, true);
    if (!g || !tooltipOwner) {
        return false;
    }

    bool consumed = false;
    ImGuiIO& io = ImGui::GetIO();
    if (std::abs(io.MouseWheel) > 0.0f || std::abs(io.MouseWheelH) > 0.0f) {
        RestoreWheelFromNonTooltipWindow(g, tooltipOwner, io.MouseWheel);
        ApplyVerticalWheelToTooltip(tooltipOwner, io.MouseWheel);
        io.MouseWheel = 0.0f;
        io.MouseWheelH = 0.0f;
        consumed = true;
    }

    for (int i = 0; i < g->InputEventsQueue.Size; ++i) {
        ImGuiInputEvent& e = g->InputEventsQueue[i];
        if (e.Type != ImGuiInputEventType_MouseWheel) {
            continue;
        }
        if (std::abs(e.MouseWheel.WheelX) > 0.0f || std::abs(e.MouseWheel.WheelY) > 0.0f) {
            consumed = true;
        }
        RestoreWheelFromNonTooltipWindow(g, tooltipOwner, e.MouseWheel.WheelY);
        ApplyVerticalWheelToTooltip(tooltipOwner, e.MouseWheel.WheelY);
        e.MouseWheel.WheelX = 0.0f;
        e.MouseWheel.WheelY = 0.0f;
    }
    return consumed;
}

void RouteWheelToScrollableTooltipBeforeNewFrame() {
    (void)RouteWheelToScrollableTooltip();
}

bool HasOpenScrollableTooltip() {
    return FindScrollableTooltipOwner(ImGui::GetCurrentContext(), false) != nullptr;
}

} // namespace ui
} // namespace smatchet
