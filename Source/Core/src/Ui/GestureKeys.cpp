#include "Ui/GestureKeys.h"

#include "imgui.h"

namespace smatchet {
namespace ui {

namespace {

// UI-thread only, like every caller: the commands hop to the UI thread before posting.
GestureKeyLatch& Latch() {
    static GestureKeyLatch latch;
    return latch;
}

int CurrentFrame() { return ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0; }

} // namespace

void PostGestureKey(GestureKey key) { Latch().Post(key, CurrentFrame()); }

bool TakeGestureKey(GestureKey key, bool targetHasFocus) {
    if (!targetHasFocus) {
        return false;
    }
    return Latch().Take(key, CurrentFrame());
}

bool TakeGestureKeyForCurrentWindow(GestureKey key) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    return TakeGestureKey(key, ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
}

} // namespace ui
} // namespace smatchet
