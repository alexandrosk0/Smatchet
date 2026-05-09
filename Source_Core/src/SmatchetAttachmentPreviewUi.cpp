#include "SmatchetUI.h"

#include "AppController.h"
#include "SmatchetAttachmentPreviewUi.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
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
#define SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS 1
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct ParsedImageInfo {
    bool Ok = false;
    int Width = 0;
    int Height = 0;
    std::string Error;
};

static std::uint32_t ReadU32BE(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

static std::uint16_t ReadU16LE(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(data[1] << 8);
}

static std::uint32_t ReadU24LE(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16);
}

static ParsedImageInfo ParseImageDimensions(const std::string& path, const std::string& mimeType) {
    ParsedImageInfo result;
    if (path.empty()) {
        result.Error = "Attachment preview path is empty.";
        return result;
    }
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs.is_open()) {
        result.Error = "Failed to open downloaded attachment file.";
        return result;
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (bytes.empty()) {
        result.Error = "Downloaded attachment file is empty.";
        return result;
    }
    if (bytes.size() >= 24 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
        bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A) {
        const std::uint32_t width = ReadU32BE(&bytes[16]);
        const std::uint32_t height = ReadU32BE(&bytes[20]);
        if (width == 0 || height == 0) {
            result.Error = "PNG dimensions are invalid.";
            return result;
        }
        result.Ok = true;
        result.Width = static_cast<int>(width);
        result.Height = static_cast<int>(height);
        return result;
    }
    if (bytes.size() >= 10 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && (bytes[3] == '8') &&
        (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a') {
        const std::uint16_t width = ReadU16LE(&bytes[6]);
        const std::uint16_t height = ReadU16LE(&bytes[8]);
        if (width == 0 || height == 0) {
            result.Error = "GIF dimensions are invalid.";
            return result;
        }
        result.Ok = true;
        result.Width = static_cast<int>(width);
        result.Height = static_cast<int>(height);
        return result;
    }
    if (bytes.size() >= 30 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
         bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
        if (bytes[12] == 'V' && bytes[13] == 'P' && bytes[14] == '8' && bytes[15] == 'X') {
            const std::uint32_t widthMinusOne = ReadU24LE(&bytes[24]);
            const std::uint32_t heightMinusOne = ReadU24LE(&bytes[27]);
            result.Ok = true;
            result.Width = static_cast<int>(widthMinusOne + 1);
            result.Height = static_cast<int>(heightMinusOne + 1);
            return result;
        }
    }
    if (bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < bytes.size()) {
            if (bytes[i] != 0xFF) {
                ++i;
                continue;
            }
            while (i < bytes.size() && bytes[i] == 0xFF) {
                ++i;
            }
            if (i >= bytes.size()) {
                break;
            }
            const unsigned char marker = bytes[i++];
            if (marker == 0xD8 || marker == 0xD9) {
                continue;
            }
            if (i + 1 >= bytes.size()) {
                break;
            }
            const std::uint16_t segmentLength =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
            if (segmentLength < 2 || i + segmentLength > bytes.size()) {
                break;
            }
            const bool isSofMarker = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                                     (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
            if (isSofMarker && segmentLength >= 7) {
                const std::uint16_t height =
                    static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i + 3]) << 8) | bytes[i + 4]);
                const std::uint16_t width =
                    static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i + 5]) << 8) | bytes[i + 6]);
                if (width == 0 || height == 0) {
                    result.Error = "JPEG dimensions are invalid.";
                    return result;
                }
                result.Ok = true;
                result.Width = static_cast<int>(width);
                result.Height = static_cast<int>(height);
                return result;
            }
            i += segmentLength;
        }
    }

    result.Error = IsSupportedImageMime(mimeType)
                       ? "Image format detected but dimensions could not be parsed by the in-app preview."
                       : "Attachment is not a supported image format for in-app preview.";
    return result;
}

struct AttachmentThumbnailSupport {
    bool CanRenderBitmapThumbnails = false;
    std::string Reason;
};

static AttachmentThumbnailSupport GetAttachmentThumbnailSupport() {
    AttachmentThumbnailSupport support;
    ImGuiIO& io = ImGui::GetIO();
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

#if defined(_WIN32) && defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
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

static bool DecodeImageFileToRgba32(const std::string& path, std::vector<unsigned char>& outPixels, int& outWidth,
                                    int& outHeight, std::string& outError) {
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
        hr = frame->GetSize(reinterpret_cast<UINT*>(&outWidth), reinterpret_cast<UINT*>(&outHeight));
        if (FAILED(hr) || outWidth <= 0 || outHeight <= 0) {
            outError = "Failed to read image dimensions.";
            break;
        }
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr) || !converter) {
            outError = "Failed to create WIC format converter.";
            break;
        }
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
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
    return ok;
}
#endif

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

    entry.PreviewRequestIssued = true;
    std::string previewError;
    if (!app.DownloadAttachmentForPreview(entry.Url, entry.Filename, entry.MimeType, &previewError)) {
        entry.PreviewError = previewError.empty() ? std::string("Failed to start preview download.") : previewError;
        LOG_WARN("SmatchetUI: preview request failed reason=%s file=%s err=%s", reason, entry.Filename.c_str(),
                 entry.PreviewError.c_str());
        return false;
    }

    LOG_DEBUG("SmatchetUI: preview request queued reason=%s file=%s", reason, entry.Filename.c_str());
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
    float drawWidth = static_cast<float>(entry.ImageWidth > 0 ? entry.ImageWidth : 1);
    float drawHeight = static_cast<float>(entry.ImageHeight > 0 ? entry.ImageHeight : 1);
    const float scale = (std::min)(maxEdge / drawWidth, maxEdge / drawHeight);
    if (scale > 0.0f) {
        drawWidth *= (std::min)(scale, 1.0f);
        drawHeight *= (std::min)(scale, 1.0f);
    }

    ImGui::BeginTooltip();
    ImGui::TextUnformatted(entry.Filename.c_str());
    if (entry.ImageWidth > 0 && entry.ImageHeight > 0) {
        ImGui::TextDisabled("%dx%d", entry.ImageWidth, entry.ImageHeight);
    }
    ImGui::Separator();
    ImGui::Image(entry.ThumbnailTextureData->GetTexRef(), ImVec2(drawWidth, drawHeight));
    ImGui::EndTooltip();
}
#endif

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
            const int eagerRequests = QueuePriorityAttachmentPreviewRequests(app, d.attachmentWindowEntries,
                                                                             d.attachmentWindowSelectedIndex, 6);
            LOG_INFO("SmatchetUI: opened attachment gallery total=%d images=%d eager=%d bitmap=%s detail=%s",
                     static_cast<int>(d.attachmentWindowEntries.size()), imageCount, eagerRequests,
                     thumbnailSupport.CanRenderBitmapThumbnails ? "enabled" : "disabled",
                     thumbnailSupport.Reason.c_str());
        }
    }

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
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
            if (thumbnailSupport.CanRenderBitmapThumbnails && entry.PreviewError.empty()) {
#if defined(_WIN32)
                std::vector<unsigned char> rgbaPixels;
                std::string uploadError;
                int decodedWidth = 0;
                int decodedHeight = 0;
                if (DecodeImageFileToRgba32(entry.LocalPath, rgbaPixels, decodedWidth, decodedHeight, uploadError)) {
                    if (decodedWidth > 0 && decodedHeight > 0) {
                        entry.ImageWidth = decodedWidth;
                        entry.ImageHeight = decodedHeight;
                    }
                    DestroyAttachmentTexture(entry.ThumbnailTextureData);
                    if (!CreateAttachmentTextureFromRgba(rgbaPixels, entry.ImageWidth, entry.ImageHeight,
                                                         entry.ThumbnailTextureData, uploadError)) {
                        entry.PreviewError = uploadError;
                        LOG_WARN("SmatchetUI: thumbnail upload failed file=%s err=%s", entry.Filename.c_str(),
                                 uploadError.c_str());
                    } else {
                        LOG_DEBUG("SmatchetUI: thumbnail uploaded file=%s size=%dx%d", entry.Filename.c_str(),
                                  entry.ImageWidth, entry.ImageHeight);
                    }
                } else {
                    entry.PreviewError = uploadError;
                    LOG_WARN("SmatchetUI: thumbnail decode failed file=%s err=%s", entry.Filename.c_str(),
                             uploadError.c_str());
                }
#endif
            } else if (!thumbnailSupport.CanRenderBitmapThumbnails && entry.PreviewError.empty()) {
                LOG_DEBUG("SmatchetUI: bitmap thumbnails unavailable for file=%s detail=%s", entry.Filename.c_str(),
                          thumbnailSupport.Reason.c_str());
            }
#endif
            d.attachmentPreviewWindowOpen = true;
        }
    }

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
    if (ImGui::Begin("Attachment Preview", &d.attachmentPreviewWindowOpen)) {
        repairTopLevelWindow(d, "attachment", 520.0f, 320.0f);
        ImGui::Text("Attachments: %d", static_cast<int>(d.attachmentWindowEntries.size()));
        if (!thumbnailSupport.CanRenderBitmapThumbnails) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", thumbnailSupport.Reason.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("AttachmentListPane", ImVec2(390, 0), true);
        const float cardWidth = 120.0f;
        const float cardHeight = 160.0f;
        const float tileSize = 96.0f;
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.x;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const int columnCount =
            (std::max)(1, static_cast<int>((availableWidth + itemSpacing) / (cardWidth + itemSpacing)));
        if (ImGui::BeginTable("AttachmentGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
            for (int i = 0; i < static_cast<int>(d.attachmentWindowEntries.size()); ++i) {
                AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(i)];
                const bool selected = (i == d.attachmentWindowSelectedIndex);
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? ImVec4(0.16f, 0.24f, 0.33f, 0.95f)
                                                                 : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
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
                    clicked =
                        ImGui::ImageButton("##attachment_thumb", entry.ThumbnailTextureData->GetTexRef(), thumbDraw);
                    ImGui::Dummy(ImVec2(0.0f, padBottom));
                    if (ImGui::IsItemHovered()) {
                        DrawAttachmentThumbnailTooltip(entry);
                    }
                } else
#endif
                {
                    const float centeredX = (std::max)(0.0f, (cardWidth - tileSize) * 0.5f);
                    ImGui::SetCursorPosX(centeredX);
                    if (!entry.PreviewError.empty()) {
                        clicked = ImGui::Button("Preview error", ImVec2(tileSize, tileSize));
                    } else if (IsSupportedImageMime(entry.MimeType) && entry.PreviewRequestIssued &&
                               entry.LocalPath.empty()) {
                        clicked = ImGui::Button("Loading...", ImVec2(tileSize, tileSize));
                    } else if (IsSupportedImageMime(entry.MimeType) && !thumbnailSupport.CanRenderBitmapThumbnails) {
                        clicked = ImGui::Button("Metadata", ImVec2(tileSize, tileSize));
                    } else if (IsSupportedImageMime(entry.MimeType)) {
                        clicked = ImGui::Button("Image", ImVec2(tileSize, tileSize));
                    } else {
                        clicked = ImGui::Button("File", ImVec2(tileSize, tileSize));
                    }
                }
                if (clicked) {
                    d.attachmentWindowSelectedIndex = i;
                }

                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardWidth - 10.0f);
                ImGui::TextUnformatted(entry.Filename.c_str());
                ImGui::PopTextWrapPos();
                if (!entry.PreviewError.empty()) {
                    ImGui::TextDisabled("preview failed");
                } else if (IsSupportedImageMime(entry.MimeType)) {
                    if (entry.ImageWidth > 0 && entry.ImageHeight > 0) {
                        ImGui::TextDisabled("%dx%d", entry.ImageWidth, entry.ImageHeight);
                    } else if (entry.PreviewRequestIssued) {
                        ImGui::TextDisabled("loading");
                    } else if (!thumbnailSupport.CanRenderBitmapThumbnails) {
                        ImGui::TextDisabled("metadata");
                    } else {
                        ImGui::TextDisabled("image");
                    }
                } else {
                    ImGui::TextDisabled("file");
                }
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                if (selected) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(90, 170, 255, 255),
                                      6.0f, 0, 2.0f);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::SameLine();
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
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Image preview metadata: %dx%d",
                               selectedEntry.ImageWidth, selectedEntry.ImageHeight);
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
            float drawWidth = static_cast<float>(selectedEntry.ImageWidth > 0 ? selectedEntry.ImageWidth : 1);
            float drawHeight = static_cast<float>(selectedEntry.ImageHeight > 0 ? selectedEntry.ImageHeight : 1);
            const float scale = (std::min)(maxWidth / drawWidth, maxHeight / drawHeight);
            if (scale > 0.0f) {
                drawWidth *= (std::min)(scale, 1.0f);
                drawHeight *= (std::min)(scale, 1.0f);
            }
            ImGui::Image(selectedEntry.ThumbnailTextureData->GetTexRef(), ImVec2(drawWidth, drawHeight));
        }
#endif
        if (ImGui::Button("Open selected")) {
            app.OpenAttachmentInSystemViewer(selectedEntry.Url, selectedEntry.Filename, selectedEntry.MimeType);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open URL externally")) {
            app.OpenUrl(selectedEntry.Url);
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            d.attachmentPreviewWindowOpen = false;
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!d.attachmentPreviewWindowOpen) {
        ReleaseAttachmentWindowEntries(d.attachmentWindowEntries);
        d.attachmentWindowSelectedIndex = 0;
    }
}




