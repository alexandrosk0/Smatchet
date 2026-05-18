// WhisperLocal — see header for design pointers. The compile-mode split
// described there is implemented via `#if SMATCHET_WHISPER_LOCAL_BACKEND`
// guards below: the ON branch pulls `whisper.h` from the FetchContent build
// and calls the real `whisper_init_from_file_with_params` + `whisper_full`
// pair; the OFF branch returns "local backend not built" from LoadModel and
// no-ops the rest.
//
// THREADING — Transcribe is synchronous and may block for hundreds of ms on
// a 10 s clip. Callers MUST dispatch it via AppController::LaunchBackgroundTask
// (Pattern A); the Phase E hotkey path will do so, and Phase C's `whisper.
// transcribe-once --mode=local` CLI command also routes through that worker
// path. Calling Transcribe from a function reachable from `ImGui::*`-frame
// is a Pillar 2 violation.

#include "WhisperLocal.h"

#include "Logger.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND
#include <whisper.h>
#endif

namespace smatchet {
namespace whisper {

#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND

struct WhisperLocal::ContextImpl {
    // Custom deleter so std::unique_ptr cleanup runs `whisper_free` instead
    // of `delete`. Pillar 3 RAII: no raw new/delete; lifetime ends exactly
    // when the parent WhisperLocal destructs.
    struct Deleter {
        void operator()(::whisper_context* ctx) const noexcept {
            if (ctx != nullptr) {
                ::whisper_free(ctx);
            }
        }
    };
    std::unique_ptr<::whisper_context, Deleter> ctx;
};

#else // SMATCHET_WHISPER_LOCAL_BACKEND OFF — stub the impl so the TU compiles.

struct WhisperLocal::ContextImpl {
    bool dummy = false;
};

#endif

WhisperLocal::WhisperLocal() : impl_(std::unique_ptr<ContextImpl>(new ContextImpl())) {}

WhisperLocal::~WhisperLocal() = default;

WhisperLocal::WhisperLocal(WhisperLocal&&) noexcept = default;
WhisperLocal& WhisperLocal::operator=(WhisperLocal&&) noexcept = default;

bool WhisperLocal::IsModelLoaded() const noexcept {
#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND
    return impl_ && impl_->ctx != nullptr;
#else
    return false;
#endif
}

const char* WhisperLocal::BackendBuildState() noexcept {
#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND
    return "ON";
#else
    return "OFF";
#endif
}

bool WhisperLocal::LoadModel(const std::string& modelPath, std::string& outError) {
    outError.clear();
    if (modelPath.empty()) {
        outError = "model path is empty";
        return false;
    }
#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND
    // Probe the file existence before whisper.cpp's loader to produce a
    // friendlier error than the deep "failed to open" log line.
    {
        std::ifstream probe(modelPath, std::ios::binary);
        if (!probe.is_open()) {
            outError = "model file not found or unreadable: " + modelPath;
            return false;
        }
    }
    ::whisper_context_params cparams = ::whisper_context_default_params();
    cparams.use_gpu = false; // CPU-only first cut; CUDA / Vulkan optional follow-up.
    ::whisper_context* raw = ::whisper_init_from_file_with_params(modelPath.c_str(), cparams);
    if (raw == nullptr) {
        outError = "whisper.cpp init returned null for model: " + modelPath;
        return false;
    }
    impl_->ctx.reset(raw);
    LOG_INFO("WhisperLocal: model loaded from %s", modelPath.c_str());
    return true;
#else
    outError = "local backend not built (set SMATCHET_WHISPER_LOCAL_BACKEND=ON to link whisper.cpp)";
    LOG_WARN("WhisperLocal::LoadModel called but local backend is OFF (path=%s)", modelPath.c_str());
    return false;
#endif
}

bool WhisperLocal::Transcribe(const std::vector<float>& pcm, std::string& outText, std::string& outError) {
    // Legacy entry point — default to English (matches Phase C ggml-*.en models).
    return Transcribe(pcm, std::string("en"), outText, outError);
}

bool WhisperLocal::Transcribe(const std::vector<float>& pcm,
                              const std::string& language,
                              std::string& outText,
                              std::string& outError) {
    outError.clear();
    outText.clear();
#if defined(SMATCHET_WHISPER_LOCAL_BACKEND) && SMATCHET_WHISPER_LOCAL_BACKEND
    if (!IsModelLoaded()) {
        outError = "model not loaded";
        return false;
    }
    if (pcm.empty()) {
        outError = "empty PCM payload";
        return false;
    }

    ::whisper_full_params wparams = ::whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_special = false;
    wparams.print_timestamps = false;
    wparams.translate = false;
    // Phase F — caller-supplied language code. "auto" / empty asks whisper.cpp
    // to autodetect from its supported set; concrete codes ("en", "fr", ...)
    // bypass the detector and force the target language. The ggml-*.en models
    // remain English-only; passing a non-"en" code with an English model
    // produces transliterated nonsense — UX responsibility lives in
    // Preferences (only show non-"en" options when a multilingual model is
    // installed; that filter is itself a Phase G follow-up).
    const std::string langArg = language.empty() ? std::string("en") : language;
    wparams.language = (langArg == "auto") ? "auto" : langArg.c_str();
    const unsigned int hwThreads = std::thread::hardware_concurrency();
    wparams.n_threads = static_cast<int>(std::max(1u, hwThreads / 2u));
    wparams.no_context = true;
    wparams.single_segment = false;

    const int rc = ::whisper_full(impl_->ctx.get(), wparams, pcm.data(), static_cast<int>(pcm.size()));
    if (rc != 0) {
        outError = "whisper_full failed (rc=" + std::to_string(rc) + ")";
        return false;
    }

    const int nSegments = ::whisper_full_n_segments(impl_->ctx.get());
    std::string acc;
    acc.reserve(64);
    for (int i = 0; i < nSegments; ++i) {
        const char* seg = ::whisper_full_get_segment_text(impl_->ctx.get(), i);
        if (seg != nullptr) {
            acc += seg;
        }
    }
    outText = std::move(acc);
    return true;
#else
    (void)pcm;
    (void)language;
    outError = "local backend not built (set SMATCHET_WHISPER_LOCAL_BACKEND=ON to link whisper.cpp)";
    return false;
#endif
}

bool WhisperLocal::Transcribe(const std::vector<std::int16_t>& pcmInt16, std::string& outText, std::string& outError) {
    return Transcribe(pcmInt16, std::string("en"), outText, outError);
}

bool WhisperLocal::Transcribe(const std::vector<std::int16_t>& pcmInt16,
                              const std::string& language,
                              std::string& outText,
                              std::string& outError) {
    if (pcmInt16.empty()) {
        outError = "empty PCM payload";
        outText.clear();
        return false;
    }
    // Convert int16 → float32 in [-1.0, 1.0]. INT16_MAX (32767) maps to ~1.0;
    // INT16_MIN maps to -32768/32767 ≈ -1.00003 which whisper.cpp clamps
    // internally — no need to special-case.
    std::vector<float> floats;
    floats.resize(pcmInt16.size());
    const float scale = 1.0f / 32767.0f;
    for (std::size_t i = 0; i < pcmInt16.size(); ++i) {
        floats[i] = static_cast<float>(pcmInt16[i]) * scale;
    }
    return Transcribe(floats, language, outText, outError);
}

} // namespace whisper
} // namespace smatchet
