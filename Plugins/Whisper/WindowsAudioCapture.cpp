// WindowsAudioCapture — WASAPI implementation. See header for the threading
// contract.
//
// WASAPI shared-mode capture loop:
//   1. CoInitializeEx(MULTITHREADED) on the worker thread.
//   2. Create IMMDeviceEnumerator -> GetDefaultAudioEndpoint(eCapture, eConsole).
//   3. Activate IAudioClient.
//   4. Negotiate format: try the 16 kHz mono int16 PCM "ideal" format first;
//      fall back to the device-suggested closest format + a downmix/resample
//      path. Phase B keeps it simple — if IsFormatSupported returns NULL
//      (exact match) we use the ideal format; if it returns a closest format
//      we accept that and resample by integer drop (16 kHz target rate factors
//      cleanly out of 48 kHz / 32 kHz; for 44.1 kHz we accept the small drift
//      Whisper is robust to).
//   5. Initialize() in event-driven mode with a 200 ms shared buffer.
//   6. GetService(IAudioCaptureClient), CreateEvent, SetEventHandle, Start.
//   7. Loop on WaitForSingleObject(event, 200 ms):
//        - GetBuffer -> copy frames -> ReleaseBuffer.
//        - Convert to mono int16 if the negotiated format isn't already that.
//        - Append to pcmBuffer_ under pcmMutex_.
//      Exit when stopRequested_ flips.
//   8. Stop, release everything (RAII handles this), CoUninitialize.

#include "WindowsAudioCapture.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "WhisperConsentGate.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// WASAPI headers
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <combaseapi.h>
#include <propsys.h>
#endif

namespace smatchet {
namespace whisper {

#if defined(_WIN32)

namespace {

// Custom-deleter pointers for the four COM interfaces we touch. Release() is
// the canonical COM cleanup; std::unique_ptr with a stateless deleter struct
// gives us scope-bound lifetime without dragging in ATL / CComPtr (which the
// rest of the codebase avoids).
struct ComReleaseDeleter {
    template <typename T>
    void operator()(T* p) const noexcept {
        if (p != nullptr) {
            p->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaseDeleter>;

// CoTaskMemFree wrapper for WAVEFORMATEX* returned by IAudioClient::GetMixFormat
// / IsFormatSupported.
struct CoTaskMemFreeDeleter {
    void operator()(WAVEFORMATEX* p) const noexcept {
        if (p != nullptr) {
            CoTaskMemFree(p);
        }
    }
};

using WaveFormatExPtr = std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter>;

// Win32 HANDLE wrapper — closes on scope exit. Used for the WASAPI capture
// event the audio engine signals when frames are available.
struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

using WinHandle = std::unique_ptr<void, HandleCloser>;

// COM RAII guard for the capture thread. CoInitializeEx in the ctor;
// CoUninitialize in the dtor. Single instance per thread.
class ComScope {
  public:
    ComScope() noexcept : initialised_(false), hr_(S_OK) {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialised_ = SUCCEEDED(hr_);
    }
    ~ComScope() noexcept {
        if (initialised_) {
            CoUninitialize();
        }
    }
    bool Ok() const noexcept { return initialised_; }
    HRESULT Hr() const noexcept { return hr_; }

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

  private:
    bool initialised_;
    HRESULT hr_;
};

// CLSID / IID locals — the SDK macros expand to extern references; reproducing
// them here avoids dragging in <ksmedia.h>.
const CLSID kClsidMMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIidIMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIidIAudioClient = __uuidof(IAudioClient);
const IID kIidIAudioCaptureClient = __uuidof(IAudioCaptureClient);

// 100-ns units per ms, per WASAPI's REFERENCE_TIME contract.
constexpr REFERENCE_TIME kRefTimesPerMs = 10000;
// Target buffer duration on the WASAPI side. 200 ms is comfortably above the
// worst-case scheduling latency for a desktop worker thread while staying
// small enough that Stop() responds within the buffer drain window.
constexpr REFERENCE_TIME kBufferDuration = 200 * kRefTimesPerMs;

// KSDATAFORMAT_SUBTYPE GUIDs for WAVEFORMATEXTENSIBLE. Reproduced locally so
// we don't have to drag <ksmedia.h> in (same rationale as the IID locals
// above). Both GUIDs share the trailing 14 bytes `0000-0010-8000-00aa00389b71`
// — the first 4 bytes encode the format tag (PCM=1, IEEE_FLOAT=3).
const GUID kSubFormatPcm = {0x00000001, 0x0000, 0x0010,
                            {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubFormatIeeeFloat = {0x00000003, 0x0000, 0x0010,
                                  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool GuidEqual(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// Resolve `fmt` to a pair (effectiveTag, effectiveBits) handling
// WAVE_FORMAT_EXTENSIBLE by unwrapping its SubFormat GUID. Returns true on
// success. Falls through to the original tag/bits when fmt is not the
// extensible wrapper.
bool ResolveFormat(const WAVEFORMATEX& fmt, WORD& outTag, WORD& outBits) noexcept {
    if (fmt.wFormatTag != WAVE_FORMAT_EXTENSIBLE) {
        outTag = fmt.wFormatTag;
        outBits = fmt.wBitsPerSample;
        return true;
    }
    // WAVEFORMATEXTENSIBLE layout: WAVEFORMATEX followed by Samples (WORD),
    // dwChannelMask (DWORD), SubFormat (GUID). cbSize must be at least
    // sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX) (22 bytes).
    if (fmt.cbSize < 22) {
        return false;
    }
    const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&fmt);
    if (GuidEqual(ext->SubFormat, kSubFormatPcm)) {
        outTag = WAVE_FORMAT_PCM;
        outBits = fmt.wBitsPerSample;
        return true;
    }
    if (GuidEqual(ext->SubFormat, kSubFormatIeeeFloat)) {
        outTag = WAVE_FORMAT_IEEE_FLOAT;
        outBits = fmt.wBitsPerSample;
        return true;
    }
    return false;
}

// Convert a single captured frame (interleaved float / int16) into an
// int16 mono sample. Handles WAVE_FORMAT_PCM int16, WAVE_FORMAT_IEEE_FLOAT
// float32, and WAVE_FORMAT_EXTENSIBLE wrapping either (the WASAPI mix
// engine ships float32 inside EXTENSIBLE on most Windows 10/11 mics —
// tag=65534=0xFFFE in the logs). Anything else falls back to mid-channel
// silence (logged once per Start by the caller).
std::int16_t MixToMonoInt16(const BYTE* framePtr, const WAVEFORMATEX& fmt) {
    const int channels = static_cast<int>(fmt.nChannels);
    if (channels < 1) {
        return 0;
    }

    WORD effTag = 0;
    WORD effBits = 0;
    if (!ResolveFormat(fmt, effTag, effBits)) {
        return 0;
    }

    if (effTag == WAVE_FORMAT_PCM && effBits == 16) {
        const std::int16_t* samples = reinterpret_cast<const std::int16_t*>(framePtr);
        std::int32_t accum = 0;
        for (int c = 0; c < channels; ++c) {
            accum += samples[c];
        }
        accum /= channels;
        if (accum > 32767) accum = 32767;
        if (accum < -32768) accum = -32768;
        return static_cast<std::int16_t>(accum);
    }

    // Float32 is the WASAPI mix-engine default. Convert by averaging channels
    // then scaling to int16 with a hard clamp.
    if (effTag == WAVE_FORMAT_IEEE_FLOAT && effBits == 32) {
        const float* samples = reinterpret_cast<const float*>(framePtr);
        double accum = 0.0;
        for (int c = 0; c < channels; ++c) {
            accum += static_cast<double>(samples[c]);
        }
        accum /= channels;
        double scaled = accum * 32767.0;
        if (scaled > 32767.0) scaled = 32767.0;
        if (scaled < -32768.0) scaled = -32768.0;
        return static_cast<std::int16_t>(scaled);
    }

    return 0;
}

// Decimate `sourceRate` -> kCaptureSampleRate via integer step. Crude but
// adequate for cloud Whisper (the model is robust to small sample-rate
// mismatches) and avoids dragging in libsamplerate / soxr for Phase B. A
// future phase that adds local whisper.cpp will likely upgrade to a real
// resampler.
struct DecimationState {
    double sourceToTargetRatio = 1.0;
    double phase = 0.0;
};

void EmitDecimated(std::int16_t s, DecimationState& st, std::vector<std::int16_t>& out) {
    st.phase += 1.0;
    if (st.phase >= st.sourceToTargetRatio) {
        out.push_back(s);
        st.phase -= st.sourceToTargetRatio;
    }
}

} // namespace

#endif // _WIN32

WindowsAudioCapture::WindowsAudioCapture() = default;

WindowsAudioCapture::~WindowsAudioCapture() {
    Stop();
}

bool WindowsAudioCapture::Start(std::string& outError) {
#if defined(_WIN32)
    outError.clear();
    // Consent invariant #1 — no mic access before user opt-in. Load the live
    // config under the lock the cache uses (cheap); WhisperConsentGate gates
    // on WhisperEnabled + WhisperSetupCompleted. Phase B builds (before this
    // PR) bypassed the gate because the only call site was the CLI smoke
    // command; Phase E adds the hotkey path which is the primary consumer.
    {
        const TrackerConfig cfg = ConfigManager::Load();
        if (!smatchet::whisper::consent::CanCaptureMic(cfg)) {
            outError = "consent required: enable Whisper dictation in Preferences first";
            return false;
        }
    }
    if (running_.load(std::memory_order_acquire)) {
        outError = "capture already running";
        return false;
    }
    stopRequested_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(pcmMutex_);
        pcmBuffer_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        deferredError_.clear();
    }
    lastPeakAmplitude_.store(0.0f, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    captureThread_ = std::thread([this]() { CaptureThreadMain(); });
    return true;
#else
    outError = "WASAPI capture is Windows-only";
    return false;
#endif
}

void WindowsAudioCapture::Stop() {
#if defined(_WIN32)
    if (!running_.load(std::memory_order_acquire) && !captureThread_.joinable()) {
        return;
    }
    stopRequested_.store(true, std::memory_order_release);
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    running_.store(false, std::memory_order_release);
    lastPeakAmplitude_.store(0.0f, std::memory_order_release);
#endif
}

bool WindowsAudioCapture::DrainCapturedPcm(std::vector<std::int16_t>& out) {
    out.clear();
#if defined(_WIN32)
    if (!running_.load(std::memory_order_acquire) && pcmBuffer_.empty()) {
        // Surface any worker-thread init error so the caller can report it.
        std::lock_guard<std::mutex> lk(errorMutex_);
        if (!deferredError_.empty()) {
            return false;
        }
        return false;
    }
    std::lock_guard<std::mutex> lk(pcmMutex_);
    out.swap(pcmBuffer_);
    return true;
#else
    return false;
#endif
}

bool WindowsAudioCapture::IsCapturing() const noexcept {
    return running_.load(std::memory_order_acquire);
}

float WindowsAudioCapture::GetLastPeakAmplitude() const noexcept {
    return lastPeakAmplitude_.load(std::memory_order_acquire);
}

#if defined(_WIN32)

void WindowsAudioCapture::CaptureThreadMain() {
    ComScope com;
    if (!com.Ok()) {
        std::lock_guard<std::mutex> lk(errorMutex_);
        deferredError_ = "CoInitializeEx failed (HRESULT=" + std::to_string(com.Hr()) + ")";
        LOG_ERROR("WindowsAudioCapture: %s", deferredError_.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    auto recordError = [this](const std::string& msg) {
        std::lock_guard<std::mutex> lk(errorMutex_);
        deferredError_ = msg;
        LOG_ERROR("WindowsAudioCapture: %s", msg.c_str());
    };

    IMMDeviceEnumerator* rawEnum = nullptr;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIidIMMDeviceEnumerator, reinterpret_cast<void**>(&rawEnum));
    if (FAILED(hr) || rawEnum == nullptr) {
        recordError("CoCreateInstance(MMDeviceEnumerator) failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    ComPtr<IMMDeviceEnumerator> deviceEnum(rawEnum);

    IMMDevice* rawDevice = nullptr;
    hr = deviceEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &rawDevice);
    if (FAILED(hr) || rawDevice == nullptr) {
        recordError("GetDefaultAudioEndpoint(eCapture) failed - no default mic, or permission denied");
        running_.store(false, std::memory_order_release);
        return;
    }
    ComPtr<IMMDevice> device(rawDevice);

    IAudioClient* rawClient = nullptr;
    hr = device->Activate(kIidIAudioClient, CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&rawClient));
    if (FAILED(hr) || rawClient == nullptr) {
        recordError("IMMDevice::Activate(IAudioClient) failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    ComPtr<IAudioClient> audioClient(rawClient);

    WAVEFORMATEX* rawMixFormat = nullptr;
    hr = audioClient->GetMixFormat(&rawMixFormat);
    if (FAILED(hr) || rawMixFormat == nullptr) {
        recordError("IAudioClient::GetMixFormat failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    WaveFormatExPtr mixFormat(rawMixFormat);
    const WAVEFORMATEX negotiated = *mixFormat;

    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 kBufferDuration, 0, mixFormat.get(), nullptr);
    if (FAILED(hr)) {
        recordError("IAudioClient::Initialize failed (HRESULT=" + std::to_string(hr) + ")");
        running_.store(false, std::memory_order_release);
        return;
    }

    WinHandle wakeEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!wakeEvent) {
        recordError("CreateEvent for WASAPI wake handle failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    hr = audioClient->SetEventHandle(wakeEvent.get());
    if (FAILED(hr)) {
        recordError("IAudioClient::SetEventHandle failed");
        running_.store(false, std::memory_order_release);
        return;
    }

    IAudioCaptureClient* rawCapture = nullptr;
    hr = audioClient->GetService(kIidIAudioCaptureClient, reinterpret_cast<void**>(&rawCapture));
    if (FAILED(hr) || rawCapture == nullptr) {
        recordError("IAudioClient::GetService(IAudioCaptureClient) failed");
        running_.store(false, std::memory_order_release);
        return;
    }
    ComPtr<IAudioCaptureClient> captureClient(rawCapture);

    hr = audioClient->Start();
    if (FAILED(hr)) {
        recordError("IAudioClient::Start failed");
        running_.store(false, std::memory_order_release);
        return;
    }

    LOG_INFO("WindowsAudioCapture: capture started (sourceRate=%lu ch=%u bps=%u tag=%u)",
             static_cast<unsigned long>(negotiated.nSamplesPerSec),
             static_cast<unsigned>(negotiated.nChannels), static_cast<unsigned>(negotiated.wBitsPerSample),
             static_cast<unsigned>(negotiated.wFormatTag));

    DecimationState decim;
    if (negotiated.nSamplesPerSec > 0) {
        decim.sourceToTargetRatio =
            static_cast<double>(negotiated.nSamplesPerSec) / static_cast<double>(kCaptureSampleRate);
    }

    const DWORD waitTimeoutMs = 200; // matches kBufferDuration so we wake reliably
    const UINT32 frameSize = negotiated.nBlockAlign; // bytes per frame across all channels

    while (!stopRequested_.load(std::memory_order_acquire)) {
        DWORD waitResult = WaitForSingleObject(wakeEvent.get(), waitTimeoutMs);
        if (waitResult == WAIT_FAILED) {
            recordError("WaitForSingleObject on wake event failed");
            break;
        }

        UINT32 packetSize = 0;
        hr = captureClient->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) {
            recordError("IAudioCaptureClient::GetNextPacketSize failed");
            break;
        }
        while (packetSize > 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                recordError("IAudioCaptureClient::GetBuffer failed");
                break;
            }
            std::vector<std::int16_t> downmixed;
            downmixed.reserve(numFrames / 2 + 1);
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            // Track |sample| across this chunk for the amplitude-meter
            // overlay (Phase E). Cheap: one branch per frame, no extra
            // allocation.
            std::int32_t peakAbs = 0;
            for (UINT32 i = 0; i < numFrames; ++i) {
                const BYTE* framePtr = data + (i * frameSize);
                std::int16_t mono = silent ? 0 : MixToMonoInt16(framePtr, negotiated);
                const std::int32_t magnitude =
                    mono >= 0 ? static_cast<std::int32_t>(mono) : -static_cast<std::int32_t>(mono);
                if (magnitude > peakAbs) {
                    peakAbs = magnitude;
                }
                EmitDecimated(mono, decim, downmixed);
            }
            captureClient->ReleaseBuffer(numFrames);
            if (!downmixed.empty()) {
                std::lock_guard<std::mutex> lk(pcmMutex_);
                pcmBuffer_.insert(pcmBuffer_.end(), downmixed.begin(), downmixed.end());
            }
            // 32768.0 puts a full-scale int16 at exactly 1.0 — the meter UI
            // expects [0, 1].
            const float peakNorm = static_cast<float>(peakAbs) / 32768.0f;
            lastPeakAmplitude_.store(peakNorm > 1.0f ? 1.0f : peakNorm, std::memory_order_release);

            hr = captureClient->GetNextPacketSize(&packetSize);
            if (FAILED(hr)) {
                recordError("IAudioCaptureClient::GetNextPacketSize (loop) failed");
                break;
            }
        }
    }

    audioClient->Stop();
    LOG_INFO("WindowsAudioCapture: capture stopped");
}

#else // !_WIN32

void WindowsAudioCapture::CaptureThreadMain() {
    // Never invoked on non-Windows: Start() returns false above and never spawns
    // the thread. Stub kept so the class definition matches across platforms.
    running_.store(false, std::memory_order_release);
}

#endif // _WIN32

} // namespace whisper
} // namespace smatchet
