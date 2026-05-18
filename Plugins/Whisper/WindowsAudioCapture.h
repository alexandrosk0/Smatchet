#pragma once

// WindowsAudioCapture — WASAPI mic capture, 16 kHz mono 16-bit PCM, ring-
// buffered for caller drain. Phase B addition; first cut targets the Whisper
// cloud transcription smoke path (whisper.transcribe-once CLI).
//
// Threading model:
//   - Start() spins up an internal capture thread; the call returns immediately.
//   - The capture thread blocks on a WASAPI event handle and copies new audio
//     frames into a mutex-guarded ring buffer. It NEVER touches ImGui state
//     or any UI primitive — strict Pillar 2 compliance.
//   - DrainCapturedPcm() copies and clears the ring buffer; the caller decides
//     when to read (typically post-Stop() for offline-style capture, or every
//     ~50 ms for live-streaming variants in future phases).
//   - Stop() signals the thread to exit + joins it; idempotent.
//
// RAII for every COM object: IMMDeviceEnumerator, IMMDevice, IAudioClient,
// IAudioCaptureClient. No raw `new` / `delete`. Custom-deleter unique_ptrs
// keep the destructor / failure paths exception-safe.
//
// Failure mode for non-Windows builds: the .cpp is compiled only when the
// Plugins/Whisper subdirectory is added (SMATCHET_WITH_WHISPER + Windows
// host). The DX12 / Unreal build keeps SMATCHET_WITH_WHISPER=OFF, so this
// header is never visited there.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace smatchet {
namespace whisper {

class WindowsAudioCapture {
  public:
    static constexpr int kCaptureSampleRate = 16000; // Whisper-preferred
    static constexpr int kCaptureChannels = 1;       // mono

    WindowsAudioCapture();
    ~WindowsAudioCapture();

    WindowsAudioCapture(const WindowsAudioCapture&) = delete;
    WindowsAudioCapture& operator=(const WindowsAudioCapture&) = delete;

    /// Initialise the default capture endpoint, start the worker thread, and
    /// begin filling the ring buffer. Returns immediately. Returns false on
    /// permission denial / no default device / COM init failure; `outError`
    /// carries a human-readable summary in that case.
    bool Start(std::string& outError);

    /// Signal the worker thread to exit and join it. Idempotent: calling Stop
    /// on a non-running capture is a no-op. Destructor calls Stop().
    void Stop();

    /// Copy captured PCM samples into `out` and clear the ring buffer. Safe
    /// to call from any thread (the same mutex the worker uses). Returns
    /// false when no capture is active (Start() was never called or already
    /// stopped); `out` is cleared in that case.
    bool DrainCapturedPcm(std::vector<std::int16_t>& out);

    /// True between a successful Start() and Stop() / destructor.
    bool IsCapturing() const noexcept;

  private:
    // Capture thread loop entry. Holds the COM init for the entire lifetime
    // of the thread (single-thread apartment is fine for IAudioCaptureClient
    // on a worker thread per WASAPI docs).
    void CaptureThreadMain();

    std::mutex pcmMutex_;
    std::vector<std::int16_t> pcmBuffer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};

    std::thread captureThread_;

    // Error message set by CaptureThreadMain on initialisation failure inside
    // the worker (where Start() has already returned). Surfaced via the next
    // DrainCapturedPcm call so the CLI command can report it.
    std::mutex errorMutex_;
    std::string deferredError_;
};

} // namespace whisper
} // namespace smatchet
