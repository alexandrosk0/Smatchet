// SmatchetWhisperOverlayUi — see header for design pointers.
// Renders a small floating window pinned to the top-right of the main
// viewport while `g_dictationRouter.IsRecording()` is true. Reads the live
// peak amplitude from `g_dictationRouter.GetLastPeakAmplitude()` (set by the
// Phase E capture worker) and draws an ImGui ProgressBar. Cancel button
// flips the router's recording state to false; the WhisperPlugin's release
// callback observes the next-frame flag drop and stops capture.
// All strings flow through SmatchetLocalization::T so future locale work
// only touches SmatchetLocalization.cpp. The TU is added to CORE_SOURCES
// only when SMATCHET_WITH_WHISPER=ON (root CMakeLists.txt source-list
// conditional); the consumer side (SmatchetUI.cpp) wraps its include +
// call in a SMATCHET_WITH_WHISPER guard so the OFF build never references
// this symbol.

#include "SmatchetWhisperOverlayUi.h"

class AppController; // fan-in Phase 6: this TU only names AppController& in signatures / pass-throughs — fwd-decl
                     // suffices
#include "DictationInsertionRouter.h"
#include "SmatchetLocalization.h"

#include "imgui.h"

namespace smatchet {
namespace whisper {
namespace overlay {

void Render(AppController& /*app*/) {
    if (!g_dictationRouter.IsRecording()) {
        return;
    }

    // Pin to the top-right corner of the current viewport with a small
    // padding inset so the overlay clears the menu bar.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr) {
        return;
    }
    const float kPadding = 12.0f;
    const float kEstimatedWidth = 240.0f; // matches the AlwaysAutoResize floor
    const ImVec2 anchor(viewport->WorkPos.x + viewport->WorkSize.x - kEstimatedWidth - kPadding,
                        viewport->WorkPos.y + kPadding);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##SmatchetWhisperOverlay", nullptr, flags)) {
        // Red dot + REC label — same colour shade as the menu-bar mic
        // indicator so the two readouts are recognisable as a pair.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextUnformatted(SmatchetLocalization::T("whisper.overlay.recordingLabel", "\xE2\x97\x8F REC"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("whisper.overlay.holdHint", "hold hotkey"));

        // Live amplitude bar. 0..1 range; ProgressBar clamps internally.
        const float amp = g_dictationRouter.GetLastPeakAmplitude();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1.0f, 0.35f, 0.35f, 0.9f));
        ImGui::ProgressBar(amp, ImVec2(220.0f, 8.0f), "");
        ImGui::PopStyleColor();

        // Cancel — flips the router flag; the next hook-thread WM_KEYUP
        // observes the user-press still down + the flag false and exits
        // cleanly. Pressing Esc through the cancel button also serves as a
        // mouse-only cancel path for users without an Esc key (rare laptop
        // accessibility scenario).
        const char* cancelText = SmatchetLocalization::T("whisper.overlay.cancelButton", "Cancel (Esc)");
        if (ImGui::SmallButton(cancelText)) {
            g_dictationRouter.SetRecording(false);
        }
    }
    ImGui::End();
}

} // namespace overlay
} // namespace whisper
} // namespace smatchet
