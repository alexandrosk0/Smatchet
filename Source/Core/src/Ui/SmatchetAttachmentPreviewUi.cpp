#include "SmatchetUI.h"

#include "AppController.h"
#include "Logger.h"
#include "MemoryTelemetry.h"
#include "AttachmentPreviewLayoutPure.h"
#include "ImageDimensionsPure.h"
#include "SmatchetAttachmentPreviewUi.h"
#include "SmatchetLocalization.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"

// SMATCHET_DEVIATION(rule=duplication; reason=idiomatic per-TU ImGui-localization preamble (imgui
// includes + `#define ImGui SmatchetLocalizedImGui` wrapper + the Win32 lean-and-mean guard) shared
// verbatim across localized-ImGui TUs; the `#define ImGui` must follow the imgui includes per-TU, so
// it is not extractable into a shared header; owner=ui-host; revisit=2026-12-31)
#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#endif

// Bitmap attachment thumbnails are available on every platform with renderer texture support:
// Windows decodes via WIC (decode-scaled by IWICBitmapScaler), other platforms via stb_image plus a
// CPU area-average downscale (RgbaDownscalePure). The renderer-agnostic upload path (ImTextureData /
// RegisterUserTexture) already works on GL / GLES3 / DX12, so only the DECODE was ever Win32-only.
#define SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS 1

#if !defined(_WIN32)
// Declarations only — the single STB_IMAGE_IMPLEMENTATION lives in SmatchetImageTextureCache.cpp
// (same Core lib, so the stbi_* symbols link there). STBI_NO_STDIO matches that TU; we decode from
// a memory buffer, so the stdio entry points are unused regardless.
#define STBI_NO_STDIO
#include <stb/stb_image.h>

#include "RgbaDownscalePure.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct ParsedImageInfo {
    bool Ok = false;
    int Width = 0;
    int Height = 0;
    std::string Error;
};

static ParsedImageInfo ParseImageDimensions(const std::string& path, const std::string& mimeType) {
    ParsedImageInfo result;
    if (path.empty()) {
        result.Error = "Attachment preview path is empty.";
        return result;
    }
    // Bounded header read (S4 of docs/plans/shipped/memory-budget-and-lifetime-hardening.md):
    // read at most kMaxHeaderBytes instead of slurping the whole file (was up to 50 MB) on the
    // UI thread. PNG/GIF/WEBP dimensions live in the first <30 bytes; the JPEG SOF marker walk
    // is naturally capped to this window (a SOF buried past the cap by huge EXIF/thumbnail
    // segments falls through to the existing "could not parse" degradation — rare, and the S5
    // decoder still reads the file in full off the UI thread).
    constexpr std::size_t kMaxHeaderBytes = 64u * 1024u;
    // Still synchronous on the UI thread (dimensions feed layout this frame) but now bounded to
    // <=64 KB (sub-ms), down from a whole-file slurp.
    // TODO(pillar2): memory-budget-and-lifetime-hardening — source dimensions from the S5 async
    // decode so even this bounded header read leaves the UI thread.
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open()) {
        result.Error = "Failed to open downloaded attachment file.";
        return result;
    }
    std::vector<unsigned char> bytes(kMaxHeaderBytes);
    ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(kMaxHeaderBytes));
    bytes.resize(static_cast<std::size_t>(ifs.gcount()));

    // Byte-level format decode lives in the pure ImageDimensionsPure seam (unit-tested
    // against untrusted-input cases). Behaviour is byte-for-byte identical to the
    // pre-extraction monolith.
    const smatchet::image_dim::ParsedImageInfo parsed =
        smatchet::image_dim::ParseImageDimensionsFromBytes(bytes, mimeType);
    result.Ok = parsed.Ok;
    result.Width = parsed.Width;
    result.Height = parsed.Height;
    result.Error = parsed.Error;
    return result;
}

static AttachmentThumbnailSupport GetAttachmentThumbnailSupport() {
    AttachmentThumbnailSupport support;
    const ImGuiIO& io = ImGui::GetIO();
    const char* backendName = io.BackendRendererName;
    const std::string rendererName = backendName != nullptr ? ToLowerAsciiCopy(backendName) : std::string();

    if ((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0) {
        support.Reason = rendererName.empty()
                             ? std::string("Renderer texture uploads are unavailable on the current backend.")
                             : std::string("Renderer texture uploads are unavailable on backend: ") + rendererName;
        return support;
    }

#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    support.CanRenderBitmapThumbnails = true;
    support.Reason = rendererName.empty() ? std::string("Bitmap thumbnails available.")
                                          : std::string("Bitmap thumbnails available on backend: ") + rendererName;
    return support;
#else
    support.Reason = rendererName.empty()
                         ? std::string("Bitmap thumbnail decoding is not compiled in for this platform.")
                         : std::string("Bitmap thumbnail decoding is not compiled in for this platform (backend: ") +
                               rendererName + ").";
    return support;
#endif
}

#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
// S5 worker-side budget: cap a decoded thumbnail's longest side. Precedent: the icon cache's
// kMaxIconDimension. A preview tooltip is drawn at a capped size anyway; the WIC backend passes this
// to IWICBitmapScaler (decode-scale, #2) and the stb backend feeds it to the CPU area-average
// downscale, so on neither platform does a multi-megapixel attachment keep a full-res RGBA texture.
constexpr int kMaxThumbnailDimension = 2048;
// Bound concurrent decode→upload tasks so many simultaneous completions can't spike the
// single-frame dispatcher Drain() (producer-side rate limit, plan § S5 spike mitigation).
constexpr std::size_t kMaxConcurrentThumbnailDecodes = 4;

#if defined(_WIN32)
// WIC backend (Windows): decode-scaled via IWICBitmapScaler, so the full-resolution image is never
// materialised on the worker.
static std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// Release the WIC COM objects acquired during a decode (any subset may be null) and balance the
// per-call CoInitializeEx. Extracted from DecodeImageFileToRgba32 under the function-size cap.
static void ReleaseWicDecodeObjects(IWICBitmapScaler* scaler, IWICFormatConverter* converter,
                                    IWICBitmapFrameDecode* frame, IWICBitmapDecoder* decoder,
                                    IWICImagingFactory* factory, bool shouldUninit) {
    if (scaler)
        scaler->Release();
    if (converter)
        converter->Release();
    if (frame)
        frame->Release();
    if (decoder)
        decoder->Release();
    if (factory)
        factory->Release();
    if (shouldUninit) {
        CoUninitialize();
    }
}

// Decode an image file to RGBA32. When maxDimension > 0, the image is decode-SCALED so its
// longest side is at most maxDimension — IWICBitmapScaler scales during the pixel pull, so a
// multi-megapixel attachment never materialises a full-res RGBA buffer on the worker (#2).
static bool DecodeImageFileToRgba32(const std::string& path, std::vector<unsigned char>& outPixels, int& outWidth,
                                    int& outHeight, std::string& outError, int maxDimension) {
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;
    outError.clear();

    const std::wstring widePath = Utf8ToWideLocal(path);
    if (widePath.empty()) {
        outError = "Failed to convert image path to wide string.";
        return false;
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED(initHr);

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    bool ok = false;

    do {
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                      reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            outError = "Failed to create WIC imaging factory.";
            break;
        }
        hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                &decoder);
        if (FAILED(hr) || !decoder) {
            outError = "Failed to decode image file.";
            break;
        }
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) {
            outError = "Failed to access image frame.";
            break;
        }
        UINT srcW = 0;
        UINT srcH = 0;
        hr = frame->GetSize(&srcW, &srcH);
        if (FAILED(hr) || srcW == 0 || srcH == 0) {
            outError = "Failed to read image dimensions.";
            break;
        }
        outWidth = static_cast<int>(srcW);
        outHeight = static_cast<int>(srcH);

        // #2: bound the DECODE resolution when a thumbnail budget is requested. The scaler
        // pulls scaled pixels straight from the frame, so the format converter + CopyPixels
        // (and the resulting buffer) only ever touch the scaled size — not the full image.
        IWICBitmapSource* pixelSource = frame;
        const UINT longest = (srcW > srcH) ? srcW : srcH;
        if (maxDimension > 0 && longest > static_cast<UINT>(maxDimension)) {
            const double scale = static_cast<double>(maxDimension) / static_cast<double>(longest);
            const UINT dstW = (std::max)(1u, static_cast<UINT>(static_cast<double>(srcW) * scale));
            const UINT dstH = (std::max)(1u, static_cast<UINT>(static_cast<double>(srcH) * scale));
            hr = factory->CreateBitmapScaler(&scaler);
            if (FAILED(hr) || !scaler) {
                outError = "Failed to create WIC bitmap scaler.";
                break;
            }
            hr = scaler->Initialize(frame, dstW, dstH, WICBitmapInterpolationModeFant);
            if (FAILED(hr)) {
                outError = "Failed to initialise WIC bitmap scaler.";
                break;
            }
            pixelSource = scaler;
            outWidth = static_cast<int>(dstW);
            outHeight = static_cast<int>(dstH);
        }

        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter) {
            outError = "Failed to create WIC format converter.";
            break;
        }
        hr = converter->Initialize(pixelSource, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            outError = "Failed to convert image to RGBA32.";
            break;
        }
        outPixels.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u);
        hr = converter->CopyPixels(nullptr, static_cast<UINT>(outWidth * 4), static_cast<UINT>(outPixels.size()),
                                   outPixels.data());
        if (FAILED(hr)) {
            outError = "Failed to copy image pixels.";
            outPixels.clear();
            break;
        }
        ok = true;
    } while (false);

    ReleaseWicDecodeObjects(scaler, converter, frame, decoder, factory, shouldUninit);
    return ok;
}

#else  // !_WIN32
// stb_image backend (Android / Linux / macOS): stb has no decode-scale, so decode the full image
// from a bounded file read, reject pathological dimensions before the alloc (memory-pressure DoS
// guard, mirroring the icon cache), then CPU area-average downscale to kMaxThumbnailDimension. Runs
// on the S5 worker pool (off the UI thread), so the transient full-res buffer never blocks a frame
// and the downscaled result lands within the same budget the WIC scaler enforces on Windows.

// Bounded read of the compressed source — not a whole-file slurp of an arbitrarily large attachment
// — before stb sees a byte.
constexpr unsigned long long kMaxThumbnailFileBytes = 32ull * 1024ull * 1024ull; // 32 MiB
// Cap the decoded resolution. stb materialises the FULL-res RGBA buffer before the downscale (unlike
// WIC, which decode-scales), so bound it: 4096*4096*4 = 64 MiB per in-flight decode. A larger source
// degrades to "too large" rather than risking OOM on a memory-constrained phone (×kMaxConcurrent…).
constexpr unsigned long long kMaxThumbnailDecodePixels = 4096ull * 4096ull; // 16 MP

static bool DecodeImageFileToRgba32(const std::string& path, std::vector<unsigned char>& outPixels, int& outWidth,
                                    int& outHeight, std::string& outError, int maxDimension) {
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;
    outError.clear();
    if (path.empty()) {
        outError = "Attachment thumbnail path is empty.";
        return false;
    }

    // Sole caller is the LaunchBackgroundTask lambda in MaybeKickThumbnailDecode (S5 worker pool),
    // never the UI thread; read is bounded to kMaxThumbnailFileBytes (32 MiB).
    /* PILLAR2_WORKER_ONLY */ // est-latency: 50ms
    std::ifstream ifs(path.c_str(), std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        outError = "Failed to open attachment file for decode.";
        return false;
    }
    const std::streamoff size = ifs.tellg();
    if (size <= 0 || static_cast<unsigned long long>(size) > kMaxThumbnailFileBytes) {
        outError = "Attachment file is empty or exceeds the thumbnail decode budget.";
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    std::vector<unsigned char> fileBytes(static_cast<std::size_t>(size));
    ifs.read(reinterpret_cast<char*>(fileBytes.data()), static_cast<std::streamsize>(size));
    if (!ifs) {
        outError = "Failed to read attachment file for decode.";
        return false;
    }

    // Pre-validate dimensions from the header only (stbi_info allocates nothing) so a malicious
    // oversized image is rejected before the full stbi_load decode/alloc (memory-pressure DoS,
    // mirrors SmatchetImageTextureCache's pre-check).
    int infoW = 0;
    int infoH = 0;
    int infoChannels = 0;
    // Fail closed: if the header can't be parsed we cannot enforce the decode budget before stbi_load
    // allocates, so reject rather than decode an unbounded image. stbi_info and stbi_load take different
    // code paths and can disagree, so a header that trips info must NOT silently skip the size gate.
    if (stbi_info_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &infoW, &infoH, &infoChannels) ==
        0) {
        outError = "Attachment image header could not be read for the thumbnail decode budget check.";
        return false;
    }
    const unsigned long long infoPixels =
        static_cast<unsigned long long>(infoW) * static_cast<unsigned long long>(infoH);
    if (infoW <= 0 || infoH <= 0 || infoPixels > kMaxThumbnailDecodePixels) {
        outError = "Attachment image dimensions exceed the thumbnail decode budget.";
        return false;
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pix =
        stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (pix == nullptr || w <= 0 || h <= 0) {
        if (pix != nullptr) {
            stbi_image_free(pix);
        }
        outError = "stb_image: attachment decode failed.";
        return false;
    }

    // Defense in depth: the decoded dimensions can diverge from the header info path, so re-check the
    // budget against the actual decode before the RGBA copy keeps the DoS guard intact.
    const unsigned long long decodedPixels = static_cast<unsigned long long>(w) * static_cast<unsigned long long>(h);
    if (decodedPixels > kMaxThumbnailDecodePixels) {
        stbi_image_free(pix);
        outError = "Attachment image dimensions exceed the thumbnail decode budget.";
        return false;
    }

    std::vector<unsigned char> fullRgba(pix, pix + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(pix);

    // Area-average downscale so the longest side is at most maxDimension. When it already fits, skip
    // the helper's internal passthrough copy and move the full-res buffer straight out.
    int dstW = 0;
    int dstH = 0;
    smatchet::image_scale::FitWithinLongestSide(w, h, maxDimension, dstW, dstH);
    if (dstW == w && dstH == h) {
        outPixels = std::move(fullRgba);
        outWidth = w;
        outHeight = h;
        return true;
    }
    if (!smatchet::image_scale::DownscaleRgba32(fullRgba, w, h, maxDimension, outPixels, outWidth, outHeight)) {
        outError = "Failed to downscale decoded attachment image.";
        return false;
    }
    return true;
}
#endif // _WIN32
#endif // SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS

#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
// Pending-destroy bookkeeping: once we flip Status to ImTextureStatus_WantDestroy the renderer backend
// will free the GPU-side resource on a later frame and set Status to Destroyed; only then is it safe to
// ImGui::UnregisterUserTexture + IM_DELETE. User textures must use RegisterUserTexture — ImGui rebuilds
// PlatformIO.Textures each frame from font atlases + UserTextures only (manual push_back is dropped).
static std::vector<ImTextureData*>& AttachmentPendingDestroyTextures() {
    static std::vector<ImTextureData*> list;
    return list;
}

static void TickAttachmentPendingTextureDestroys() {
    auto& pending = AttachmentPendingDestroyTextures();
    if (pending.empty()) {
        return;
    }
    for (auto it = pending.begin(); it != pending.end();) {
        ImTextureData* tex = *it;
        if (tex && tex->Status == ImTextureStatus_Destroyed) {
            ImGui::UnregisterUserTexture(tex);
            IM_DELETE(tex);
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
}

static bool CreateAttachmentTextureFromRgba(const std::vector<unsigned char>& pixels, int width, int height,
                                            ImTextureData*& outTextureData, std::string& outError) {
    outTextureData = nullptr;
    outError.clear();
    if (pixels.empty() || width <= 0 || height <= 0) {
        outError = "Image pixels are empty.";
        return false;
    }
    const size_t expected = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (pixels.size() < expected) {
        outError = "Image pixel buffer smaller than width*height*4.";
        return false;
    }
    ImTextureData* tex = IM_NEW(ImTextureData)();
    if (!tex) {
        outError = "Failed to allocate ImTextureData.";
        return false;
    }
    tex->Create(ImTextureFormat_RGBA32, width, height);
    if (tex->Pixels == nullptr) {
        IM_DELETE(tex);
        outError = "ImTextureData::Create failed to allocate pixel storage.";
        return false;
    }
    memcpy(tex->Pixels, pixels.data(), expected);
    tex->UseColors = true;
    tex->Status = ImTextureStatus_WantCreate;
    ImGui::RegisterUserTexture(tex);
    outTextureData = tex;
    return true;
}

static void DestroyAttachmentTexture(ImTextureData*& textureData) {
    if (!textureData) {
        return;
    }
    textureData->Status = ImTextureStatus_WantDestroy;
    textureData->WantDestroyNextFrame = true;
    AttachmentPendingDestroyTextures().push_back(textureData);
    textureData = nullptr;
}
#endif

static void ReleaseAttachmentWindowEntry(AttachmentWindowEntry& entry) {
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    DestroyAttachmentTexture(entry.ThumbnailTextureData);
#endif
    entry.LocalPath.clear();
    entry.ImageWidth = 0;
    entry.ImageHeight = 0;
    entry.PreviewError.clear();
    entry.PreviewRequestIssued = false;
}

static void ReleaseAttachmentWindowEntries(std::vector<AttachmentWindowEntry>& entries) {
    for (auto& entry : entries) {
        ReleaseAttachmentWindowEntry(entry);
    }
    entries.clear();
}

#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
static bool AttachmentHasBitmapThumbnail(const AttachmentWindowEntry& entry) {
    return entry.ThumbnailTextureData != nullptr;
}
#endif

static bool QueueAttachmentPreviewRequest(AppController& app, AttachmentWindowEntry& entry, const char* reason) {
    if (!IsSupportedImageMime(entry.MimeType) || entry.Url.empty() || entry.PreviewRequestIssued) {
        return false;
    }

    // Capture the request fields by value before dispatching — `entry` may be invalidated
    // if the attachment window entries are released/regenerated mid-download. The download
    // completion path runs through the existing AttachmentPreviewHandlerCallback (mutex-
    // protected attachmentPreviewUpdateQueue) so no UI-thread state is touched from the
    // worker thread (Pillar 2 — finding #2: was blocking the UI for the 120s-timeout cpr
    // download of up to 50MB per attachment).
    entry.PreviewRequestIssued = true;
    const std::string capturedUrl = entry.Url;
    const std::string capturedFilename = entry.Filename;
    const std::string capturedMime = entry.MimeType;
    const std::string capturedReason = reason ? std::string(reason) : std::string();
    app.LaunchBackgroundTask([&app, capturedUrl, capturedFilename, capturedMime, capturedReason]() {
        std::string previewError;
        if (!app.DownloadAttachmentForPreview(capturedUrl, capturedFilename, capturedMime, &previewError)) {
            LOG_WARN("SmatchetUI: preview request failed reason=%s file=%s err=%s", capturedReason.c_str(),
                     capturedFilename.c_str(),
                     previewError.empty() ? "Failed to start preview download." : previewError.c_str());
            // P2-H7: surface the failure to the card instead of leaving it on
            // "Loading..." forever. Same mutex-protected queue as the success path.
            std::lock_guard<std::mutex> lock(g_ui.attachmentPreviewMutex);
            AttachmentPreviewUpdate failedUpdate;
            failedUpdate.Filename = capturedFilename;
            failedUpdate.Url = capturedUrl;
            failedUpdate.Error = previewError.empty() ? std::string("Preview download failed.") : previewError;
            g_ui.attachmentPreviewUpdateQueue.push_back(std::move(failedUpdate));
            return;
        }
        LOG_DEBUG("SmatchetUI: preview request completed reason=%s file=%s", capturedReason.c_str(),
                  capturedFilename.c_str());
    });
    LOG_DEBUG("SmatchetUI: preview request queued reason=%s file=%s", capturedReason.c_str(), capturedFilename.c_str());
    return true;
}

static int QueuePriorityAttachmentPreviewRequests(AppController& app, std::vector<AttachmentWindowEntry>& entries,
                                                  int selectedIndex, int maxRequests) {
    if (maxRequests <= 0 || entries.empty()) {
        return 0;
    }

    int requestsStarted = 0;
    std::unordered_set<int> scheduledIndices;
    const int entryCount = static_cast<int>(entries.size());

    auto scheduleIndex = [&](int index, const char* reason) {
        if (index < 0 || index >= entryCount || requestsStarted >= maxRequests) {
            return;
        }
        if (scheduledIndices.find(index) != scheduledIndices.end()) {
            return;
        }
        scheduledIndices.insert(index);
        if (QueueAttachmentPreviewRequest(app, entries[static_cast<size_t>(index)], reason)) {
            ++requestsStarted;
        }
    };

    if (selectedIndex >= 0 && selectedIndex < entryCount) {
        scheduleIndex(selectedIndex, "selected");
        for (int radius = 1; radius < entryCount && requestsStarted < maxRequests; ++radius) {
            scheduleIndex(selectedIndex - radius, "nearby-left");
            scheduleIndex(selectedIndex + radius, "nearby-right");
        }
    }

    for (int i = 0; i < entryCount && requestsStarted < maxRequests; ++i) {
        scheduleIndex(i, "backfill");
    }
    return requestsStarted;
}

// Scales image dimensions to fit inside a square of edge `maxEdge`, preserving aspect ratio.
static ImVec2 FitImageInsideSquare(int imageWidth, int imageHeight, float maxEdge) {
    float w = static_cast<float>(imageWidth > 0 ? imageWidth : 1);
    float h = static_cast<float>(imageHeight > 0 ? imageHeight : 1);
    if (w <= 0.0f) {
        w = 1.0f;
    }
    if (h <= 0.0f) {
        h = 1.0f;
    }
    const float scale = (std::min)(maxEdge / w, maxEdge / h);
    if (scale > 0.0f) {
        w *= scale;
        h *= scale;
    }
    return ImVec2(w, h);
}

#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
static void DrawAttachmentThumbnailTooltip(const AttachmentWindowEntry& entry) {
    if (!entry.ThumbnailTextureData) {
        return;
    }

    const float maxEdge = 320.0f;
    const smatchet::attach::ImageDrawSize drawSize =
        smatchet::attach::ComputeImageDrawSize(entry.ImageWidth, entry.ImageHeight, maxEdge, maxEdge, true);

    ImGui::BeginTooltip();
    ImGui::TextUnformatted(entry.Filename.c_str());
    if (entry.ImageWidth > 0 && entry.ImageHeight > 0) {
        ImGui::TextDisabled("%dx%d", entry.ImageWidth, entry.ImageHeight);
    }
    ImGui::Separator();
    ImGui::Image(entry.ThumbnailTextureData->GetTexRef(), ImVec2(drawSize.Width, drawSize.Height));
    ImGui::EndTooltip();
}

// S5: launch or retry an off-thread thumbnail decode when an entry needs one and the rate
// limit allows it. Decode + WIC scale run on the joined pool; only the GPU upload is posted
// back via the dispatcher (it must run on the UI thread). The worker captures path + Url by
// value; the upload callback re-finds the entry by Url in the process-global g_ui, so a vector
// resize/reorder between decode and upload can never dangle a pointer (Pillar 3). The
// ThumbnailDecodeInFlight flag dedupes per-entry; a rate-limited skip just retries next frame
// (no silent drop). Called per-frame from the card-draw loop.
static void MaybeKickThumbnailDecode(AppController& app, AttachmentWindowEntry& entry) {
    if (entry.ThumbnailDecodeInFlight || entry.ThumbnailTextureData != nullptr || !entry.PreviewError.empty() ||
        entry.Url.empty() || entry.LocalPath.empty() || !IsSupportedImageMime(entry.MimeType)) {
        return;
    }
    std::atomic<std::size_t>& pending = smatchet::memtel::PendingThumbnailUploads();
    if (pending.load(std::memory_order_relaxed) >= kMaxConcurrentThumbnailDecodes) {
        return; // saturated — leave the entry eligible; a later frame retries.
    }
    const std::string localPath = entry.LocalPath;
    const std::string urlKey = entry.Url;
    entry.ThumbnailDecodeInFlight = true;
    pending.fetch_add(1, std::memory_order_relaxed);
    app.LaunchBackgroundTask([&app, localPath, urlKey]() {
        std::vector<unsigned char> rgba;
        std::string err;
        int w = 0;
        int h = 0;
        const bool decoded = DecodeImageFileToRgba32(localPath, rgba, w, h, err, kMaxThumbnailDimension);
        app.PostToMainThread([urlKey, rgba = std::move(rgba), w, h, err, decoded]() mutable {
            smatchet::memtel::PendingThumbnailUploads().fetch_sub(1, std::memory_order_relaxed);
            AttachmentWindowEntry* target = nullptr;
            for (AttachmentWindowEntry& e : g_ui.attachmentWindowEntries) {
                if (e.Url == urlKey) {
                    target = &e;
                    break;
                }
            }
            if (target == nullptr) {
                return; // entry gone (window reloaded) — nothing to upload
            }
            target->ThumbnailDecodeInFlight = false;
            if (!decoded) {
                target->PreviewError = err;
                LOG_WARN("SmatchetUI: thumbnail decode failed url=%s err=%s", urlKey.c_str(), err.c_str());
                return;
            }
            if (w > 0 && h > 0) {
                target->ImageWidth = w;
                target->ImageHeight = h;
            }
            DestroyAttachmentTexture(target->ThumbnailTextureData);
            std::string upErr;
            if (!CreateAttachmentTextureFromRgba(rgba, target->ImageWidth, target->ImageHeight,
                                                 target->ThumbnailTextureData, upErr)) {
                target->PreviewError = upErr;
                LOG_WARN("SmatchetUI: thumbnail upload failed url=%s err=%s", urlKey.c_str(), upErr.c_str());
            } else {
                LOG_DEBUG("SmatchetUI: thumbnail uploaded url=%s size=%dx%d", urlKey.c_str(), target->ImageWidth,
                          target->ImageHeight);
            }
        });
    });
}
#endif

// Rebuild the window entry list from a freshly-collected attachment request and kick the
// eager priority preview fetches. Non-ImGui state mutation only — extracted verbatim from
// drawAttachmentPreviewWindow's collection-drain phase.
static void IngestAttachmentCollection(AppController& app, UiDrawSession& d,
                                       const AttachmentCollectionRequest& nextCollection,
                                       const AttachmentThumbnailSupport& thumbnailSupport) {
    ReleaseAttachmentWindowEntries(d.attachmentWindowEntries);
    d.attachmentWindowEntries.reserve(nextCollection.Attachments.size());
    int imageCount = 0;
    for (const auto& attachment : nextCollection.Attachments) {
        AttachmentWindowEntry entry;
        entry.Filename = attachment.Filename.empty() ? std::string("Attachment") : attachment.Filename;
        entry.Url = attachment.Url;
        entry.MimeType = attachment.MimeType;
        if (IsSupportedImageMime(entry.MimeType)) {
            ++imageCount;
        }
        d.attachmentWindowEntries.push_back(std::move(entry));
    }
    d.attachmentWindowSelectedIndex = 0;
    d.attachmentPreviewWindowOpen = true;
    if (imageCount > 0) {
        const int eagerRequests =
            QueuePriorityAttachmentPreviewRequests(app, d.attachmentWindowEntries, d.attachmentWindowSelectedIndex, 6);
        LOG_INFO("SmatchetUI: opened attachment gallery total=%d images=%d eager=%d bitmap=%s detail=%s",
                 static_cast<int>(d.attachmentWindowEntries.size()), imageCount, eagerRequests,
                 thumbnailSupport.CanRenderBitmapThumbnails ? "enabled" : "disabled", thumbnailSupport.Reason.c_str());
    }
}

// Apply a drained batch of download-completion updates to the matching window entries
// (parsing image dimensions on the UI thread for the matched entry). Non-ImGui state
// mutation only — extracted verbatim from drawAttachmentPreviewWindow's update-drain phase.
static void ApplyAttachmentPreviewUpdates(UiDrawSession& d, const std::deque<AttachmentPreviewUpdate>& previewUpdates) {
    for (const AttachmentPreviewUpdate& nextPreviewUpdate : previewUpdates) {
        int targetIndex = -1;
        for (int i = 0; i < static_cast<int>(d.attachmentWindowEntries.size()); ++i) {
            const AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(i)];
            if ((!nextPreviewUpdate.Url.empty() && entry.Url == nextPreviewUpdate.Url) ||
                (nextPreviewUpdate.Url.empty() && entry.Filename == nextPreviewUpdate.Filename)) {
                targetIndex = i;
                break;
            }
        }
        if (targetIndex < 0 && d.attachmentWindowSelectedIndex >= 0 &&
            d.attachmentWindowSelectedIndex < static_cast<int>(d.attachmentWindowEntries.size())) {
            targetIndex = d.attachmentWindowSelectedIndex;
        }
        if (targetIndex >= 0 && !nextPreviewUpdate.Error.empty()) {
            // Failed download (P2-H7): show the error on the card and release the request
            // latch so selecting the attachment again retries the download.
            AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(targetIndex)];
            entry.PreviewError = nextPreviewUpdate.Error;
            entry.PreviewRequestIssued = false;
            continue;
        }
        if (targetIndex >= 0) {
            AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(targetIndex)];
            entry.LocalPath = nextPreviewUpdate.LocalPath;
            entry.MimeType = nextPreviewUpdate.MimeType;
            entry.PreviewError.clear();
            entry.ImageWidth = 0;
            entry.ImageHeight = 0;
            entry.PreviewRequestIssued = true;
            if (IsSupportedImageMime(entry.MimeType)) {
                const ParsedImageInfo imageInfo = ParseImageDimensions(entry.LocalPath, entry.MimeType);
                if (imageInfo.Ok) {
                    entry.ImageWidth = imageInfo.Width;
                    entry.ImageHeight = imageInfo.Height;
                } else {
                    entry.PreviewError = imageInfo.Error;
                }
            }
            // S5: the off-thread thumbnail decode is kicked per-frame by MaybeKickThumbnailDecode()
            // in the card-draw loop below — not here — so a rate-limited (saturated) skip retries
            // on a later frame instead of this one-shot update dropping it silently.
            d.attachmentPreviewWindowOpen = true;
        }
    }
}

} // namespace

void SmatchetUI::drawAttachmentPreviewWindow(AppController& app, UiDrawSession& d) {
    AttachmentCollectionRequest nextCollection;
    bool hasCollection = false;
    std::deque<AttachmentPreviewUpdate> previewUpdates;
    const AttachmentThumbnailSupport thumbnailSupport = GetAttachmentThumbnailSupport();
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    // Reclaim textures whose renderer backend has finished the WantDestroy round-trip.
    TickAttachmentPendingTextureDestroys();
#endif
    {
        std::lock_guard<std::mutex> lock(d.attachmentPreviewMutex);
        if (!d.attachmentCollectionQueue.empty()) {
            nextCollection = d.attachmentCollectionQueue.back();
            d.attachmentCollectionQueue.clear();
            hasCollection = true;
        }
        if (!d.attachmentPreviewUpdateQueue.empty()) {
            previewUpdates.swap(d.attachmentPreviewUpdateQueue);
        }
    }

    if (hasCollection) {
        IngestAttachmentCollection(app, d, nextCollection, thumbnailSupport);
    }

    ApplyAttachmentPreviewUpdates(d, previewUpdates);

    if (!d.attachmentPreviewWindowOpen) {
        return;
    }
    if (d.attachmentWindowEntries.empty()) {
        d.attachmentPreviewWindowOpen = false;
        return;
    }
    if (d.attachmentWindowSelectedIndex < 0 ||
        d.attachmentWindowSelectedIndex >= static_cast<int>(d.attachmentWindowEntries.size())) {
        d.attachmentWindowSelectedIndex = 0;
    }

    QueuePriorityAttachmentPreviewRequests(app, d.attachmentWindowEntries, d.attachmentWindowSelectedIndex, 2);

    prepareTopLevelWindow(d, "attachment", 1040.0f, 560.0f);
    if (ImGui::Begin("Attachment Preview", &d.attachmentPreviewWindowOpen,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        repairTopLevelWindow(d, "attachment", 520.0f, 320.0f);
        ImGui::Text("Attachments: %d", static_cast<int>(d.attachmentWindowEntries.size()));
        if (!thumbnailSupport.CanRenderBitmapThumbnails) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", thumbnailSupport.Reason.c_str());
        }
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
        // S5 visible cue: thumbnail decode now runs off the UI thread, so surface in-flight
        // work (Pillar 2 — a worker-deferred operation must show a cue) driven off the same
        // pendingThumbnailUploads gauge the perf.memory snapshot reads.
        {
            const std::size_t loadingThumbs =
                smatchet::memtel::PendingThumbnailUploads().load(std::memory_order_relaxed);
            if (loadingThumbs > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s",
                                    SmatchetLocalization::T("attachment.thumbnails.loading", "loading thumbnails..."));
            }
        }
#endif
        ImGui::Separator();
        AttachmentPreviewDrawCtx ctx{app, d, thumbnailSupport};
        drawAttachmentListPane(ctx);
        ImGui::SameLine();
        drawAttachmentDetailsPane(ctx);
    }
    ImGui::End();
    if (!d.attachmentPreviewWindowOpen) {
        ReleaseAttachmentWindowEntries(d.attachmentWindowEntries);
        d.attachmentWindowSelectedIndex = 0;
    }
}

namespace {

// Map a classified attachment card label to its display string. Extracted from the per-card switch in
// drawAttachmentListPane under the function-size cap; behaviour-identical.
const char* AttachmentCardLabelText(smatchet::attach::CardLabel label) {
    switch (label) {
    case smatchet::attach::CardLabel::PreviewError:
        return "Preview error";
    case smatchet::attach::CardLabel::Loading:
        return "Loading...";
    case smatchet::attach::CardLabel::Metadata:
        return "Metadata";
    case smatchet::attach::CardLabel::Image:
        return "Image";
    case smatchet::attach::CardLabel::File:
        return "File";
    }
    return "File";
}

// Render one attachment grid card (thumbnail-or-label tile, filename, status line, selection outline)
// into the current table column. Extracted from drawAttachmentListPane under the function-size cap,
// behaviour-identical. Free helper — needs no SmatchetUI state.
void DrawAttachmentCard(AppController& app, UiDrawSession& d, AttachmentWindowEntry& entry, int i,
                        const AttachmentThumbnailSupport& thumbnailSupport, float cardWidth, float cardHeight,
                        float tileSize) {
    (void)app; // only read on the SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS decode-kick path below.
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    // S5: per-frame decode kick — retries a rate-limited skip, dedup'd via the
    // entry's ThumbnailDecodeInFlight flag.
    if (thumbnailSupport.CanRenderBitmapThumbnails) {
        MaybeKickThumbnailDecode(app, entry);
    }
#endif
    const bool selected = (i == d.attachmentWindowSelectedIndex);
    ImGui::TableNextColumn();
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          selected ? ImVec4(0.16f, 0.24f, 0.33f, 0.95f) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::BeginChild("AttachmentCard", ImVec2(cardWidth, cardHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    bool clicked = false;
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    if (AttachmentHasBitmapThumbnail(entry)) {
        const ImVec2 thumbDraw = FitImageInsideSquare(entry.ImageWidth, entry.ImageHeight, tileSize);
        const float padX = (std::max)(0.0f, (cardWidth - thumbDraw.x) * 0.5f);
        const float padTop = (std::max)(0.0f, (tileSize - thumbDraw.y) * 0.5f);
        const float padBottom = (std::max)(0.0f, tileSize - padTop - thumbDraw.y);
        ImGui::SetCursorPosX(padX);
        ImGui::Dummy(ImVec2(0.0f, padTop));
        clicked = ImGui::ImageButton("##attachment_thumb", entry.ThumbnailTextureData->GetTexRef(), thumbDraw);
        ImGui::Dummy(ImVec2(0.0f, padBottom));
        if (ImGui::IsItemHovered()) {
            DrawAttachmentThumbnailTooltip(entry);
        }
    } else
#endif
    {
        const float centeredX = (std::max)(0.0f, (cardWidth - tileSize) * 0.5f);
        ImGui::SetCursorPosX(centeredX);
        const smatchet::attach::CardLabel label = smatchet::attach::ClassifyAttachmentCardLabel(
            !entry.PreviewError.empty(), IsSupportedImageMime(entry.MimeType), entry.PreviewRequestIssued,
            entry.LocalPath.empty(), thumbnailSupport.CanRenderBitmapThumbnails);
        clicked = ImGui::Button(AttachmentCardLabelText(label), ImVec2(tileSize, tileSize));
    }
    if (clicked) {
        d.attachmentWindowSelectedIndex = i;
    }

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardWidth - 10.0f);
    ImGui::TextUnformatted(entry.Filename.c_str());
    ImGui::PopTextWrapPos();
    const smatchet::attach::CardStatus status = smatchet::attach::ClassifyAttachmentCardStatus(
        !entry.PreviewError.empty(), IsSupportedImageMime(entry.MimeType),
        entry.ImageWidth > 0 && entry.ImageHeight > 0, entry.PreviewRequestIssued,
        thumbnailSupport.CanRenderBitmapThumbnails);
    switch (status) {
    case smatchet::attach::CardStatus::PreviewFailed:
        ImGui::TextDisabled("preview failed");
        break;
    case smatchet::attach::CardStatus::Dimensions:
        ImGui::TextDisabled("%dx%d", entry.ImageWidth, entry.ImageHeight);
        break;
    case smatchet::attach::CardStatus::Loading:
        ImGui::TextDisabled("loading");
        break;
    case smatchet::attach::CardStatus::Metadata:
        ImGui::TextDisabled("metadata");
        break;
    case smatchet::attach::CardStatus::Image:
        ImGui::TextDisabled("image");
        break;
    case smatchet::attach::CardStatus::File:
        ImGui::TextDisabled("file");
        break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (selected) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 170, 255, 255), 6.0f, 0, 2.0f);
    }
    ImGui::PopID();
}

} // namespace

void SmatchetUI::drawAttachmentListPane(AttachmentPreviewDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const AttachmentThumbnailSupport& thumbnailSupport = ctx.thumbnailSupport;

    ImGui::BeginChild("AttachmentListPane", ImVec2(390, 0), true);
    const float cardWidth = 120.0f;
    const float cardHeight = 160.0f;
    const float tileSize = 96.0f;
    const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const int columnCount = (std::max)(1, static_cast<int>((availableWidth + itemSpacing) / (cardWidth + itemSpacing)));
    if (ImGui::BeginTable("AttachmentGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < static_cast<int>(d.attachmentWindowEntries.size()); ++i) {
            AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(i)];
            DrawAttachmentCard(app, d, entry, i, thumbnailSupport, cardWidth, cardHeight, tileSize);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void SmatchetUI::drawAttachmentDetailsPane(AttachmentPreviewDrawCtx& ctx) {
    AppController& app = ctx.app;
    UiDrawSession& d = ctx.d;
    const AttachmentThumbnailSupport& thumbnailSupport = ctx.thumbnailSupport;

    AttachmentWindowEntry& selectedEntry =
        d.attachmentWindowEntries[static_cast<size_t>(d.attachmentWindowSelectedIndex)];
    ImGui::BeginChild("AttachmentDetailsPane", ImVec2(0, 0), false);
    ImGui::TextUnformatted(selectedEntry.Filename.c_str());
    ImGui::Separator();
    ImGui::Text("Mime: %s", selectedEntry.MimeType.empty() ? "(unknown)" : selectedEntry.MimeType.c_str());
    if (!selectedEntry.LocalPath.empty()) {
        ImGui::TextWrapped("Local file: %s", selectedEntry.LocalPath.c_str());
    } else {
        ImGui::TextDisabled("Local file: not downloaded yet");
    }
    if (!selectedEntry.PreviewError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", selectedEntry.PreviewError.c_str());
        ImGui::PopStyleColor();
    } else if (IsSupportedImageMime(selectedEntry.MimeType) && selectedEntry.ImageWidth > 0 &&
               selectedEntry.ImageHeight > 0) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Image preview metadata: %dx%d", selectedEntry.ImageWidth,
                           selectedEntry.ImageHeight);
    } else if (IsSupportedImageMime(selectedEntry.MimeType) && selectedEntry.PreviewRequestIssued) {
        ImGui::TextDisabled("Loading preview...");
    }
    if (IsSupportedImageMime(selectedEntry.MimeType) && !thumbnailSupport.CanRenderBitmapThumbnails) {
        ImGui::TextDisabled("%s", thumbnailSupport.Reason.c_str());
    }
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    if (AttachmentHasBitmapThumbnail(selectedEntry)) {
        const float maxWidth = ImGui::GetContentRegionAvail().x;
        const float maxHeight = 220.0f;
        const smatchet::attach::ImageDrawSize drawSize = smatchet::attach::ComputeImageDrawSize(
            selectedEntry.ImageWidth, selectedEntry.ImageHeight, maxWidth, maxHeight, true);
        ImGui::Image(selectedEntry.ThumbnailTextureData->GetTexRef(), ImVec2(drawSize.Width, drawSize.Height));
    }
#endif
    // P2-H8: the download behind "Open selected" can take up to ~120 s — run it on a
    // worker instead of freezing the UI thread, disable the button while in flight, and
    // toast when the result silently changed shape (browser fallback / launch failure).
    const bool openInFlight = !d.attachmentOpenInFlightUrl.empty();
    if (openInFlight) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Open selected")) {
        d.attachmentOpenInFlightUrl = selectedEntry.Url;
        const std::string openUrl = selectedEntry.Url;
        const std::string openFilename = selectedEntry.Filename;
        const std::string openMime = selectedEntry.MimeType;
        app.LaunchBackgroundTask([&app, openUrl, openFilename, openMime]() {
            std::string openError;
            bool fellBackToUrl = false;
            const bool openedLocally =
                app.OpenAttachmentInSystemViewer(openUrl, openFilename, openMime, &openError, &fellBackToUrl);
            app.PostToMainThread([openUrl, openFilename, openError, openedLocally, fellBackToUrl]() {
                if (g_ui.attachmentOpenInFlightUrl == openUrl) {
                    g_ui.attachmentOpenInFlightUrl.clear();
                }
                if (openedLocally) {
                    return;
                }
                if (fellBackToUrl) {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("toast.attachment.open", "Open attachment"),
                        SmatchetLocalization::Format("attachment.open.url_fallback",
                                                     "Download failed (%s) - opened \"%s\" in your browser instead.",
                                                     openError.c_str(), openFilename.c_str()),
                        ToastType::Warning);
                } else {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("toast.attachment.open", "Open attachment"),
                        SmatchetLocalization::Format("attachment.open.launch_failed",
                                                     "Downloaded \"%s\" but couldn't open it: %s", openFilename.c_str(),
                                                     openError.c_str()),
                        ToastType::Error);
                }
            });
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Open URL externally")) {
        app.OpenUrl(selectedEntry.Url);
    }
    if (openInFlight) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::Format("attachment.open.downloading", "Downloading \"%s\"...",
                                                               selectedEntry.Filename.c_str()));
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        d.attachmentPreviewWindowOpen = false;
    }
    ImGui::EndChild();
}
