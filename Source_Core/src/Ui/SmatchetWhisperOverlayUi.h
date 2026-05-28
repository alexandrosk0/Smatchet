#pragma once

// SmatchetWhisperOverlayUi — floating amplitude-meter overlay shown while
// push-to-talk dictation is active. Renders only when
// `g_dictationRouter.IsRecording()` returns true; auto-hides otherwise.
//
// Per Pillar 2's visual-cue contract: the overlay must appear within 100 ms
// of the hotkey press. Worker thread flips `recording_` immediately on the
// onPress callback (see WhisperPlugin.cpp); the next UI frame picks it up,
// which on the 144 Hz target is ~6.9 ms after the flip and within budget on
// any sustainable load.
//
// Live amplitude reading comes from the dictation router's
// `GetLastPeakAmplitude()` accessor, written by the Phase E
// WindowsAudioCapture worker per drained WASAPI chunk. The reading is
// lock-free (atomic<float>); the per-frame UI cost is one atomic load + one
// ImGui::ProgressBar.
//
// Gated at the source-list level (CMakeLists.txt only adds this TU to
// CORE_SOURCES when SMATCHET_WITH_WHISPER=ON), so the unconditional
// `#include` from SmatchetUI.cpp is wrapped in a SMATCHET_WITH_WHISPER guard
// on the consumer side.

class AppController;

namespace smatchet {
namespace whisper {
namespace overlay {

// Render the overlay window. No-op when recording is idle. Cheap: a single
// AlwaysAutoResize / NoMove / NoFocus window with one ProgressBar + an Esc
// label, pinned to the top-right of the viewport.
void Render(AppController& app);

} // namespace overlay
} // namespace whisper
} // namespace smatchet
