#pragma once

// WhisperLocal — thin RAII wrapper around the whisper.cpp C API. The class
// owns one `whisper_context*` for its lifetime and exposes a synchronous
// `Transcribe` call that MUST be invoked from a worker thread (see Pillar 2
// in AGENTS.md — no synchronous inference on the UI thread). The hot path in
// the Phase E hotkey wiring will call Transcribe from a thread launched via
// AppController::LaunchBackgroundTask, then post the result back to the UI
// via MainThreadDispatcher::PostToMainThread.
// Two compile modes, gated by the new sub-option SMATCHET_WHISPER_LOCAL_BACKEND
// (see root CMakeLists.txt and Deviations in docs/plans/shipped/whisper-dictation.md):
//   - SMATCHET_WHISPER_LOCAL_BACKEND=ON  → WhisperLocal.cpp pulls
//     `whisper.h` from the FetchContent build and links the real
//     library. Calling LoadModel + Transcribe runs whisper.cpp inference
//     against the downloaded ggml model.
//   - SMATCHET_WHISPER_LOCAL_BACKEND=OFF (default in Phase C) → the body
//     compiles a stub that always returns false from LoadModel with
//     "local backend not built" in `outError`. Banner / mode router code
//     callers fall back to cloud per § Mode router decision tree.
// The split keeps Phase C's binary-size + MinGW-build risk gated: shipping
// the API surface unconditionally lets the ModelDownloader, banner, and
// Preferences tab land without dragging whisper.cpp + ggml's ~50 MB of code
// into every Smatchet build until Phase H decides whether to ship it as a
// monolithic static link or refactor to a runtime-loaded DLL.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace smatchet {
namespace whisper {

class WhisperLocal {
  public:
    WhisperLocal();
    ~WhisperLocal();

    WhisperLocal(const WhisperLocal&) = delete;
    WhisperLocal& operator=(const WhisperLocal&) = delete;
    WhisperLocal(WhisperLocal&&) noexcept;
    WhisperLocal& operator=(WhisperLocal&&) noexcept;

    /// Loads the ggml model file at `modelPath` and prepares the context
    /// for transcription. Returns false on file-not-found, unsupported
    /// format, OOM, or when the local backend was compiled out (see
    /// header note about SMATCHET_WHISPER_LOCAL_BACKEND). On success the
    /// context is owned by this instance and freed by the destructor.
    bool LoadModel(const std::string& modelPath, std::string& outError);

    /// Synchronous transcription of 16 kHz mono float32 PCM. MUST be
    /// called from a worker thread (see header preamble). Returns false
    /// on inference error; `outText` is populated on success. The 16 kHz
    /// rate matches whisper.cpp's internal expectation — WindowsAudioCapture
    /// already produces decimated mono input at that rate.
    bool Transcribe(const std::vector<float>& pcm, std::string& outText, std::string& outError);

    /// Convenience overload that converts WindowsAudioCapture's int16
    /// payload to float32 in-line. Range-bounded by INT16_MAX so silent
    /// frames map to exact 0.0f without rounding noise.
    bool Transcribe(const std::vector<std::int16_t>& pcmInt16, std::string& outText, std::string& outError);

    /// Phase F language-aware overloads. `language` is the ISO code forwarded
    /// to whisper.cpp's `whisper_full_params.language` (e.g. "en", "fr").
    /// "auto" / empty asks whisper.cpp to autodetect. Same threading
    /// contract as the no-language overloads.
    bool Transcribe(const std::vector<float>& pcm,
                    const std::string& language,
                    std::string& outText,
                    std::string& outError);
    bool Transcribe(const std::vector<std::int16_t>& pcmInt16,
                    const std::string& language,
                    std::string& outText,
                    std::string& outError);

    bool IsModelLoaded() const noexcept;

    /// Returns "ON" / "OFF" depending on the SMATCHET_WHISPER_LOCAL_BACKEND
    /// compile flag. Surfaced by `whisper.status` so the CLI consumer can
    /// distinguish "user hasn't downloaded a model" from "this build does
    /// not have whisper.cpp linked at all".
    static const char* BackendBuildState() noexcept;

  private:
    /// Opaque holder for `whisper_context*` when SMATCHET_WHISPER_LOCAL_BACKEND
    /// is ON, std::nullptr_t when OFF. The unique_ptr's custom deleter
    /// calls `whisper_free` so the destructor is RAII-clean either way.
    struct ContextImpl;
    std::unique_ptr<ContextImpl> impl_;
};

} // namespace whisper
} // namespace smatchet
