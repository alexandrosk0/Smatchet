#include "SmatchetUI.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "JiraGridFieldDisplay.h"
#include "SmatchetFieldRender.h"
#include "CompactDateFormat.h"
#include "SpreadsheetState.h"
#include "AiController.h"
#include "Logger.h"
#include "NavigationHistory.h"
#include "UiPerfMonitor.h"
#include "SmatchetPerfUi.h"
#include "StringUtil.h"
#include "JiraLabelsEditor.h"
#include "JiraDateTimeFieldEditor.h"
#include "IssueDraft.h"
#include "IssueTableSerializer.h"
#include "Win32PickFiles.h"
#include <ghc/filesystem.hpp>
#include "imgui.h"
#include "imgui_internal.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
// WIC decoding works on both OpenGL (GLFW standalone) and DX12 (Unreal plugin) Windows builds, so we
// gate it on Windows rather than on a specific renderer backend. Thumbnail upload uses ImGui's
// backend-agnostic ImTextureData API (requires ImGuiBackendFlags_RendererHasTextures at runtime).
#define SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS 1
#endif
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring> // memcpy, memset
#include <exception>
#include <fstream>
#include <algorithm>
#include <deque>
#include <future>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>
#include <string>
#include <system_error>

namespace {

/** Quiet period after last grid column width / sort metadata change before writing views JSON. */
constexpr std::chrono::milliseconds kViewStateSaveDebounce{300};

std::string JoinCsv(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += values[i];
    }
    return out;
}

/** Bounded, NUL-terminated copy into a char array (avoids strncpy deprecation / truncation warnings). */
template <size_t N>
void CopyStringToBuffer(char (&dst)[N], const std::string& str) {
    if (N == 0) {
        return;
    }
    std::memset(dst, 0, N);
    const size_t cap = N - 1;
    const size_t n = (std::min)(str.size(), cap);
    if (n > 0) {
        std::memcpy(dst, str.data(), n);
    }
}

std::vector<std::string> ParseCsv(const std::string& csv) {
    std::vector<std::string> result;
    std::string current;
    for (char ch : csv) {
        if (ch == ',') {
            size_t start = 0;
            size_t end = current.size();
            while (start < end && (current[start] == ' ' || current[start] == '\t')) ++start;
            while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) --end;
            if (end > start) {
                result.push_back(current.substr(start, end - start));
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    size_t start = 0;
    size_t end = current.size();
    while (start < end && (current[start] == ' ' || current[start] == '\t')) ++start;
    while (end > start && (current[end - 1] == ' ' || current[end - 1] == '\t')) --end;
    if (end > start) {
        result.push_back(current.substr(start, end - start));
    }
    return result;
}

bool ContainsCaseInsensitive(const std::string& text, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }

    auto toLower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string loweredText(text.size(), '\0');
    std::string loweredNeedle(needle.size(), '\0');
    std::transform(text.begin(), text.end(), loweredText.begin(), toLower);
    std::transform(needle.begin(), needle.end(), loweredNeedle.begin(), toLower);
    return loweredText.find(loweredNeedle) != std::string::npos;
}

std::vector<std::string> ToSortedVector(const std::unordered_set<std::string>& values) {
    std::vector<std::string> result(values.begin(), values.end());
    std::sort(result.begin(), result.end());
    return result;
}

// Natural issue key comparison: BLOOP-2 < BLOOP-12 (project lexicographic, then number).
bool CompareIssueKeyNatural(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) -> std::pair<std::string, long long> {
        std::string project;
        long long num = 0;
        const size_t dash = s.rfind('-');
        if (dash != std::string::npos && dash + 1 < s.size()) {
            project = s.substr(0, dash);
            const std::string tail = s.substr(dash + 1);
            if (!tail.empty()) {
                char* end = nullptr;
                const long long v = std::strtoll(tail.c_str(), &end, 10);
                if (end == tail.c_str() + tail.size()) {
                    num = v;
                    return {project, num};
                }
            }
        }
        return {s, 0};
    };
    const auto pa = split(a);
    const auto pb = split(b);
    if (pa.first != pb.first) {
        return pa.first < pb.first;
    }
    return pa.second < pb.second;
}

// Parse "10h", "2d 4h", "36000" to seconds for sort. Same work units as display (8h day, 5d week).
long long ParseDurationToSecondsForSort(const std::string& input) {
    const long long kSecondsPerHour = 3600LL;
    const long long kSecondsPerDay = 8LL * kSecondsPerHour;
    const long long kSecondsPerWeek = 5LL * kSecondsPerDay;
    auto trim = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    };
    std::string s = trim(input);
    if (s.empty()) return 0;
    size_t pos = 0;
    try {
        long long v = std::stoll(s, &pos, 10);
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.size()) return v;
    } catch (...) {}
    pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.size()) break;
        long long num = 1;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t next = 0;
            try {
                num = std::stoll(s.substr(pos), &next, 10);
                pos += next;
            } catch (...) { break; }
        }
        if (pos >= s.size()) { total += num; break; }
        const char u = s[pos];
        if (u == 'w' || u == 'W') { total += num * kSecondsPerWeek; ++pos; }
        else if (u == 'd' || u == 'D') { total += num * kSecondsPerDay; ++pos; }
        else if (u == 'h' || u == 'H') { total += num * kSecondsPerHour; ++pos; }
        else if (u == 'm' || u == 'M') { total += num * 60LL; ++pos; }
        else { total += num; }
    }
    return total;
}

static const std::unordered_set<std::string> kTimeTrackingFieldIds = {
    "timeoriginalestimate", "timeestimate", "timespent",
    "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"
};

static const std::unordered_set<std::string> kDateFieldIds = {
    "created", "updated", "duedate"
};

// Returns -1 if a < b, 0 if equal, 1 if a > b. Empty values sort last when ascending.
int CompareFieldValuesForSort(const std::string& fieldId,
                              const JiraField* fieldMeta,
                              const std::string& aVal,
                              const std::string& bVal,
                              int sortDirection) {
    const bool aEmpty = aVal.empty();
    const bool bEmpty = bVal.empty();
    if (aEmpty && bEmpty) return 0;
    if (aEmpty) return (sortDirection == 1) ? 1 : -1;  // ascending: empty last
    if (bEmpty) return (sortDirection == 1) ? -1 : 1;

    if (kTimeTrackingFieldIds.count(fieldId)) {
        const long long sa = ParseDurationToSecondsForSort(aVal);
        const long long sb = ParseDurationToSecondsForSort(bVal);
        if (sa != sb) return (sa < sb) ? -1 : 1;
        return 0;
    }
    if (kDateFieldIds.count(fieldId) || (fieldMeta && (fieldMeta->Type == "date" || fieldMeta->Type == "datetime"))) {
        const int cmp = aVal.compare(bVal);
        if (cmp != 0) return (cmp < 0) ? -1 : 1;
        return 0;
    }
    if (fieldMeta && fieldMeta->Type == "number") {
        bool aNum = false, bNum = false;
        double da = 0, db = 0;
        try { da = std::stod(aVal); aNum = true; } catch (...) {}
        try { db = std::stod(bVal); bNum = true; } catch (...) {}
        if (aNum && bNum) {
            if (da != db) return (da < db) ? -1 : 1;
            return 0;
        }
    } else {
        try {
            size_t posA = 0, posB = 0;
            const long long na = std::stoll(aVal, &posA, 10);
            const long long nb = std::stoll(bVal, &posB, 10);
            if (posA == aVal.size() && posB == bVal.size()) {
                if (na != nb) return (na < nb) ? -1 : 1;
                return 0;
            }
        } catch (...) {}
    }
    auto ciCompare = [](const std::string& x, const std::string& y) {
        const size_t n = (std::min)(x.size(), y.size());
        for (size_t i = 0; i < n; ++i) {
            const int cxa = std::tolower(static_cast<unsigned char>(x[i]));
            const int cxb = std::tolower(static_cast<unsigned char>(y[i]));
            if (cxa != cxb) return (cxa < cxb) ? -1 : 1;
        }
        if (x.size() != y.size()) return (x.size() < y.size()) ? -1 : 1;
        return 0;
    };
    return ciCompare(aVal, bVal);
}

bool IsJiraDateOrDateTimeField(const std::string& fieldId, const JiraField* field) {
    if (kDateFieldIds.count(fieldId)) {
        return true;
    }
    if (field && (field->Type == "date" || field->Type == "datetime")) {
        return true;
    }
    return false;
}

bool IsAttachmentFieldId(const std::string& fieldId) {
    const std::string lower = ToLowerAsciiCopy(fieldId);
    return lower == "attachment" || lower == "attachments";
}

std::string DisplayValueForJiraDateField(const std::string& fieldId,
                                         const JiraField* field,
                                         const std::string& currentValue) {
    if (!IsJiraDateOrDateTimeField(fieldId, field)) {
        return currentValue;
    }
    const std::string compact = FormatCompactJiraDateForDisplay(currentValue);
    return compact.empty() ? currentValue : compact;
}

struct AttachmentCollectionRequest {
    std::vector<AppController::AttachmentDescriptor> Attachments;
};

struct AttachmentPreviewUpdate {
    std::string LocalPath;
    std::string MimeType;
    std::string Filename;
    std::string Url;
};

struct AttachmentWindowEntry {
    std::string Filename;
    std::string Url;
    std::string MimeType;
    std::string LocalPath;
    int ImageWidth = 0;
    int ImageHeight = 0;
    std::string PreviewError;
    bool PreviewRequestIssued = false;
#if defined(SMATCHET_ENABLE_BITMAP_ATTACHMENT_THUMBNAILS)
    ImTextureData* ThumbnailTextureData = nullptr;
#endif
};

struct ParsedImageInfo {
    bool Ok = false;
    int Width = 0;
    int Height = 0;
    std::string Error;
};

static bool IsSupportedImageMime(const std::string& mimeType) {
    const auto semiPos = mimeType.find(';');
    const std::string normalized =
        ToLowerAsciiCopy(TrimCopyAsciiWhitespace(semiPos == std::string::npos ? mimeType : mimeType.substr(0, semiPos)));
    return normalized == "image/png" ||
           normalized == "image/jpeg" ||
           normalized == "image/jpg" ||
           normalized == "image/gif" ||
           normalized == "image/webp";
}

static std::uint32_t ReadU32BE(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

static std::uint16_t ReadU16LE(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1] << 8);
}

static std::uint32_t ReadU24LE(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
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
    if (bytes.size() >= 24 &&
        bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
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
    if (bytes.size() >= 10 &&
        bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' &&
        (bytes[3] == '8') && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a') {
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
    if (bytes.size() >= 30 &&
        bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
        if (bytes[12] == 'V' && bytes[13] == 'P' && bytes[14] == '8' && bytes[15] == 'X' && bytes.size() >= 30) {
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
            const bool isSofMarker =
                (marker >= 0xC0 && marker <= 0xC3) ||
                (marker >= 0xC5 && marker <= 0xC7) ||
                (marker >= 0xC9 && marker <= 0xCB) ||
                (marker >= 0xCD && marker <= 0xCF);
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
    support.Reason = rendererName.empty()
        ? std::string("Bitmap thumbnails available.")
        : std::string("Bitmap thumbnails available on backend: ") + rendererName;
    return support;
#else
    support.Reason = rendererName.empty()
        ? std::string("Bitmap thumbnail decoding is not compiled in for this platform.")
        : std::string("Bitmap thumbnail decoding is not compiled in for this platform (backend: ") + rendererName + ").";
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

static bool DecodeImageFileToRgba32(const std::string& path,
                                    std::vector<unsigned char>& outPixels,
                                    int& outWidth,
                                    int& outHeight,
                                    std::string& outError) {
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
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory,
                                      reinterpret_cast<void**>(&factory));
        if (FAILED(hr) || !factory) {
            outError = "Failed to create WIC imaging factory.";
            break;
        }
        hr = factory->CreateDecoderFromFilename(
            widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
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
        hr = converter->Initialize(frame,
                                   GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone,
                                   nullptr,
                                   0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) {
            outError = "Failed to convert image to RGBA32.";
            break;
        }
        outPixels.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 4u);
        hr = converter->CopyPixels(nullptr,
                                   static_cast<UINT>(outWidth * 4),
                                   static_cast<UINT>(outPixels.size()),
                                   outPixels.data());
        if (FAILED(hr)) {
            outError = "Failed to copy image pixels.";
            outPixels.clear();
            break;
        }
        ok = true;
    } while (false);

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
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

static bool CreateAttachmentTextureFromRgba(const std::vector<unsigned char>& pixels,
                                            int width,
                                            int height,
                                            ImTextureData*& outTextureData,
                                            std::string& outError) {
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

static bool QueueAttachmentPreviewRequest(AppController& app,
                                          AttachmentWindowEntry& entry,
                                          const char* reason) {
    if (!IsSupportedImageMime(entry.MimeType) || entry.Url.empty() || entry.PreviewRequestIssued) {
        return false;
    }

    entry.PreviewRequestIssued = true;
    std::string previewError;
    if (!app.DownloadAttachmentForPreview(entry.Url, entry.Filename, entry.MimeType, &previewError)) {
        entry.PreviewError = previewError.empty()
            ? std::string("Failed to start preview download.")
            : previewError;
        LOG_WARN("SmatchetUI: preview request failed reason=%s file=%s err=%s",
                 reason,
                 entry.Filename.c_str(),
                 entry.PreviewError.c_str());
        return false;
    }

    LOG_DEBUG("SmatchetUI: preview request queued reason=%s file=%s", reason, entry.Filename.c_str());
    return true;
}

static int QueuePriorityAttachmentPreviewRequests(AppController& app,
                                                  std::vector<AttachmentWindowEntry>& entries,
                                                  int selectedIndex,
                                                  int maxRequests) {
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

struct JiraFieldCatalogIndex {
    explicit JiraFieldCatalogIndex(const std::vector<JiraField>& fields) {
        for (const auto& field : fields) {
            FieldById[field.Id] = &field;
        }
    }

    const JiraField* Find(const std::string& fieldId) const {
        const auto it = FieldById.find(fieldId);
        return it == FieldById.end() ? nullptr : it->second;
    }

    std::string DisplayName(const std::string& fieldId) const {
        if (fieldId == "history") {
            return "History";
        }
        const JiraField* field = Find(fieldId);
        return field ? field->Name : fieldId;
    }

private:
    std::unordered_map<std::string, const JiraField*> FieldById;
};

struct TicketGridColumn {
    enum class Kind {
        Id,
        JiraFieldValue
    };

    enum class RenderPlan {
        PlainText,
        SpecialAttachment,
        SpecialWatchers,
        SpecialVotes,
        SpecialWorklog,
        SpecialProgress,
        SpecialIssueRestriction,
        Labels,
        Cascading,
        MultiSelect,
        SingleSelect,
        DateTimeEditor,
        TextEditor
    };

    Kind ColumnKind = Kind::Id;
    std::string Key;
    std::string Label;
    std::string FieldId;
    RenderPlan Plan = RenderPlan::PlainText;
    bool IsDateLike = false;
    bool CatalogReadOnly = false;
    bool NeedsAllowEditsCheck = false;
};

class TicketGridColumnsBuilder {
public:
    static std::vector<TicketGridColumn> Build(const ViewDefinition& view, const JiraFieldCatalogIndex& catalog) {
        std::vector<TicketGridColumn> columns;
        std::vector<TicketGridColumn> allColumns;
        allColumns.push_back({TicketGridColumn::Kind::Id, "id", "ID", std::string()});

        std::unordered_set<std::string> seenFieldIds;
        for (const auto& rawFieldId : view.Fields) {
            const std::string fieldId = TrimFieldId(rawFieldId);
            if (fieldId.empty() || !seenFieldIds.insert(fieldId).second) {
                continue;
            }

            TicketGridColumn column;
            column.ColumnKind = TicketGridColumn::Kind::JiraFieldValue;
            column.Key = "field:" + fieldId;
            column.FieldId = fieldId;
            column.Label = catalog.DisplayName(fieldId);
            const JiraField* field = catalog.Find(fieldId);
            column.Plan = ResolveRenderPlan(fieldId, field);
            column.IsDateLike = IsJiraDateOrDateTimeField(fieldId, field);
            column.CatalogReadOnly = field != nullptr && field->ReadOnly;
            column.NeedsAllowEditsCheck = RequiresAllowEditsCheck(column.Plan);
            allColumns.push_back(column);
        }

        std::unordered_map<std::string, TicketGridColumn> byKey;
        for (const auto& col : allColumns) {
            byKey[col.Key] = col;
        }

        std::unordered_set<std::string> usedKeys;
        for (const auto& key : view.ColumnOrder) {
            const auto it = byKey.find(key);
            if (it == byKey.end()) {
                continue;
            }
            columns.push_back(it->second);
            usedKeys.insert(key);
        }
        for (const auto& col : allColumns) {
            if (usedKeys.find(col.Key) == usedKeys.end()) {
                columns.push_back(col);
            }
        }

        return columns;
    }

private:
    static TicketGridColumn::RenderPlan ResolveRenderPlan(const std::string& fieldId, const JiraField* field) {
        if (field == nullptr) {
            if (IsAttachmentFieldId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialAttachment;
            }
            if (JiraGridFieldDisplay::IsWatchersColumnId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialWatchers;
            }
            if (JiraGridFieldDisplay::IsVotesColumnId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialVotes;
            }
            if (JiraGridFieldDisplay::IsWorklogColumnId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialWorklog;
            }
            if (JiraGridFieldDisplay::IsProgressStyleColumnId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialProgress;
            }
            if (JiraGridFieldDisplay::IsIssueRestrictionColumnId(fieldId)) {
                return TicketGridColumn::RenderPlan::SpecialIssueRestriction;
            }
            return TicketGridColumn::RenderPlan::PlainText;
        }

        if (IsAttachmentFieldId(field->Id)) {
            return TicketGridColumn::RenderPlan::SpecialAttachment;
        }
        if (JiraGridFieldDisplay::IsWatchersColumnId(field->Id)) {
            return TicketGridColumn::RenderPlan::SpecialWatchers;
        }
        if (JiraGridFieldDisplay::IsVotesColumnId(field->Id)) {
            return TicketGridColumn::RenderPlan::SpecialVotes;
        }
        if (JiraGridFieldDisplay::IsWorklogColumnId(field->Id)) {
            return TicketGridColumn::RenderPlan::SpecialWorklog;
        }
        if (JiraGridFieldDisplay::IsProgressDisplayField(field)) {
            return TicketGridColumn::RenderPlan::SpecialProgress;
        }
        if (JiraGridFieldDisplay::IsIssueRestrictionField(field)) {
            return TicketGridColumn::RenderPlan::SpecialIssueRestriction;
        }
        if (field->ReadOnly) {
            return TicketGridColumn::RenderPlan::PlainText;
        }
        if (JiraLabelsEditor::IsLabelsField(field->Id)) {
            return TicketGridColumn::RenderPlan::Labels;
        }
        if (field->Family == JiraFieldFamily::CascadingSelect) {
            return TicketGridColumn::RenderPlan::Cascading;
        }
        if ((field->Family == JiraFieldFamily::SelectMulti ||
             field->Family == JiraFieldFamily::StructuredMulti ||
             field->Family == JiraFieldFamily::UserMulti) &&
            !field->AllowedValueOptions.empty()) {
            return TicketGridColumn::RenderPlan::MultiSelect;
        }
        if ((field->Family == JiraFieldFamily::SelectSingle ||
             field->Family == JiraFieldFamily::StructuredSingle ||
             field->Family == JiraFieldFamily::UserSingle ||
             field->Family == JiraFieldFamily::Status ||
             field->Family == JiraFieldFamily::IssueType) &&
            !field->AllowedValueOptions.empty()) {
            return TicketGridColumn::RenderPlan::SingleSelect;
        }
        if (field->IsArray && !field->AllowedValueOptions.empty()) {
            return TicketGridColumn::RenderPlan::MultiSelect;
        }
        if (!field->AllowedValueOptions.empty()) {
            return TicketGridColumn::RenderPlan::SingleSelect;
        }
        if (JiraDateTimeFieldEditor::IsJiraDateTimePickerField(*field)) {
            return TicketGridColumn::RenderPlan::DateTimeEditor;
        }
        return TicketGridColumn::RenderPlan::TextEditor;
    }

    static bool RequiresAllowEditsCheck(TicketGridColumn::RenderPlan plan) {
        return plan == TicketGridColumn::RenderPlan::Labels ||
               plan == TicketGridColumn::RenderPlan::Cascading ||
               plan == TicketGridColumn::RenderPlan::MultiSelect ||
               plan == TicketGridColumn::RenderPlan::SingleSelect ||
               plan == TicketGridColumn::RenderPlan::DateTimeEditor ||
               plan == TicketGridColumn::RenderPlan::TextEditor;
    }

    static std::string TrimFieldId(const std::string& value) {
        size_t start = 0;
        size_t end = value.size();
        while (start < end && (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' || value[start] == '\r')) {
            ++start;
        }
        while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\n' || value[end - 1] == '\r')) {
            --end;
        }
        return value.substr(start, end - start);
    }
};

struct PendingFieldEdit {
    std::string IssueId;
    JiraField Field;
    std::vector<std::string> Values;
};

struct FieldEditCommitResult {
    bool Ok = false;
    std::string Error;
    AppController::JiraFieldEditResult ApplyResult;
};

struct FieldCatalogFetchResult {
    bool Ok = false;
    std::vector<JiraField> Fields;
    std::vector<JiraComponent> Components;
    std::vector<JiraUser> Users;
    std::string Error;
    std::string Warning;
};

enum class CellWriteState {
    Saving,
    Success,
    Error
};

struct CellWriteFeedback {
    CellWriteState State = CellWriteState::Saving;
    std::string Message;
    int FramesRemaining = 0;
};

struct UiDrawSession {
    bool cfgInitialized = false;
    JiraConfig cfg;

    bool showPreferences = false;
    bool showViewsDashboard = true;
    /** When true, next draw of the Views window requests ImGui focus (brings docked tab to front). */
    bool requestViewsDashboardFocus = false;
    bool showPerformance = false;
    bool showBlameAnalysis = false;
    bool showBulkImport = false;
    bool showBulkExport = false;

    bool fieldCatalogFetchStarted = false;
    bool fieldCatalogLoading = false;
    std::future<FieldCatalogFetchResult> fieldCatalogFuture;
    bool triggerCatalogRefetch = false;
    std::string fieldCatalogWarning;

    bool appliedInitialView = false;
    NavigationHistory navHistory;

    char domainBuf[128]{};
    char emailBuf[128]{};
    char tokenBuf[512]{};
    char projectKeyBuf[64]{};
    /** Comma-separated Jira field ids copied from last row for + New issue (see JiraConfig::NewIssueInheritFieldIds). */
    char newIssueInheritFieldsBuf[512]{};
    char aiApiKeyBuf[512]{};
    char aiModelBuf[128]{};
    char aiBaseUrlBuf[256]{};
    bool mcpEnabled = false;
    int mcpPort = 8080;
    bool mcpAllowRemote = false;
    char mcpAuthTokenBuf[512]{};
    bool preferencesBuffersLoaded = false;

    char viewNameBuf[128]{};
    char viewJqlBuf[512]{};
    char selectedFieldsBuf[1024]{};
    char fieldSearchBuf[128]{};
    std::vector<std::string> editingColumnOrder;
    int selectedColumnOrderIndex = -1;
    std::string editingViewId;
    std::vector<std::string> lastSyncedColumnOrder;

    SpreadsheetState gridState;
    std::string gridEditError;
    std::string gridEditSuccess;
    std::deque<PendingFieldEdit> queuedFieldEdits;
    bool hasInFlightEdit = false;
    PendingFieldEdit inFlightEdit;
    std::string inFlightOriginalEstimateSnapshot;
    std::string inFlightRemainingEstimateSnapshot;
    bool inFlightCommitStarted = false;
    std::future<FieldEditCommitResult> inFlightCommitFuture;
    /** Issuetype key captured with the row when starting an async Jira edit (for editmeta on worker). */
    std::string inFlightIssueTypeKeySnapshot;
    int inFlightDelayFrames = 0;
    std::unordered_map<std::string, CellWriteFeedback> cellFeedbackByKey;

    std::string lastGridActiveViewId;
    /** Last `BuildGridContextSignature` for the active grid; used to drop new-issue draft on view/JQL/column change. */
    std::string lastGridContextSignature;
    /** When true, next completed `CreateIssueAsync` result is ignored (user left the grid). */
    bool newIssueDiscardAsyncCreateResult = false;
    std::vector<size_t> cachedSortedIndices;
    std::string cachedSortFingerprint;
    std::uint64_t cachedSortTicketsRevision = 0;
    std::uint64_t cachedSortCatalogRevision = 0;
    bool cachedSortValid = false;

    /** At vertical bottom: first N wheel ticks do not map to horizontal scroll (see RouteVerticalWheelToHorizontalAtTableVerticalEnds). */
    int gridBottomHorizontalWheelSwallowsRemaining = 0;
    /** Same at vertical top. */
    int gridTopHorizontalWheelSwallowsRemaining = 0;

    std::string aiResponse;
    bool aiIsThinking = false;

    std::vector<char> logBuffer;
    std::uint64_t lastSeenLogRevision = 0;
    /** When true, `ViewState.Save()` is scheduled for `pendingViewStateSaveAt` (debounced, extended on each change). */
    bool pendingViewStateSave = false;
    std::chrono::steady_clock::time_point pendingViewStateSaveAt{};

    JiraGridFieldAsyncState jiraGridAsync;

    // ---- End-of-grid "new issue" draft ----
    // Holds the in-progress draft plus per-row editor buffers. Reset when
    // the user clicks Cancel, or after a successful create.
    bool newIssueDraftActive = false;
    IssueDraft newIssueDraft;
    std::unordered_map<std::string, std::vector<char>> newIssueDraftEditBufs; // fieldId -> ImGui InputText buffer
    std::future<IssueCreateResult> newIssueCreateFuture;
    bool newIssueCreateInFlight = false;
    std::string newIssueLastError;
    std::string newIssueLastSuccessKey;
    std::vector<std::string> newIssueMissingFieldIds;

    // ---- Bulk import/export ----
    std::vector<char> bulkImportTextBuf;          // textarea backing store
    char bulkImportPathBuf[1024]{};               // source file path (typed in)
    int  bulkImportFormatSel = 0;                 // 0=Auto,1=CSV,2=TSV,3=JSON
    IssueTableSerializer::ImportResult bulkImportPreview;
    std::vector<std::string> bulkImportStatus;    // per-row status text
    std::vector<std::future<IssueCreateResult>> bulkImportFutures;
    size_t bulkImportNextSubmit = 0;
    size_t bulkImportCompleted = 0;
    bool bulkImportRunning = false;
    std::string bulkImportError;
    /** Tracks prior frame's `showBulkImport` so we can reset state on the open→close transition. */
    bool bulkImportWasOpen = false;

    char bulkExportPathBuf[1024]{};
    int  bulkExportFormatSel = 1; // 0=CSV,1=TSV(default),2=JSON
    bool bulkExportOnlySelected = false;
    std::string bulkExportFeedback;

    std::mutex attachmentPreviewMutex;
    std::deque<AttachmentCollectionRequest> attachmentCollectionQueue;
    std::deque<AttachmentPreviewUpdate> attachmentPreviewUpdateQueue;
    bool attachmentPreviewCallbackRegistered = false;
    bool attachmentPreviewWindowOpen = false;
    std::vector<AttachmentWindowEntry> attachmentWindowEntries;
    int attachmentWindowSelectedIndex = 0;
};

/** Fingerprint of what the ticket grid is showing (view, JQL, columns). */
static std::string BuildGridContextSignature(const ViewDefinition* view, const std::string& jqlQuery) {
    std::string s;
    if (!view) {
        s = "@\x1e";
    } else {
        s = view->Id;
        s.push_back('\x1e');
        s += JoinCsv(view->Fields);
        s.push_back('\x1e');
        s += JoinCsv(view->ColumnOrder);
        s.push_back('\x1e');
    }
    s += jqlQuery;
    return s;
}

static void CancelUnfinishedNewIssueForGridChange(UiDrawSession& d) {
    if (!d.newIssueDraftActive && !d.newIssueCreateInFlight) {
        return;
    }
    d.newIssueDraftActive = false;
    d.newIssueDraft = IssueDraft{};
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueLastError.clear();
    d.newIssueLastSuccessKey.clear();
    if (d.newIssueCreateInFlight) {
        d.newIssueDiscardAsyncCreateResult = true;
    }
}

static UiDrawSession g_ui;
static SmatchetPerfUi g_perfUi;
static bool g_openFilePathsHandlerInstalled = false;

static std::future<FieldCatalogFetchResult> StartFieldCatalogFetchAsync(const JiraConfig& fetchCfg) {
    return std::async(std::launch::async, [fetchCfg]() {
        FieldCatalogFetchResult result;
        JiraClient client;
        std::vector<JiraField> fields;
        std::vector<JiraComponent> components;
        std::string error;
        result.Ok = client.FetchFieldCatalog(fetchCfg, fields, components, error);
        if (!result.Ok) {
            result.Error = error;
            return result;
        }

        std::vector<JiraUser> users;
        std::string usersError;
        if (!client.FetchUsers(fetchCfg, users, usersError)) {
            result.Warning = usersError;
        }

        if (!users.empty()) {
            for (auto& field : fields) {
                if (!field.IsUserType) {
                    continue;
                }
                field.AllowedValues.clear();
                field.AllowedValueOptions.clear();
                field.AllowedValues.reserve(users.size());
                field.AllowedValueOptions.reserve(users.size());
                for (const auto& user : users) {
                    field.AllowedValues.push_back(user.DisplayName);
                    JiraFieldOption option;
                    option.Id = user.AccountId;
                    option.Value = user.DisplayName;
                    field.AllowedValueOptions.push_back(std::move(option));
                }
            }
        }

        result.Fields = std::move(fields);
        result.Components = std::move(components);
        result.Users = std::move(users);
        return result;
    });
}

/** When vertically at top/bottom (or no vertical scroll), map mouse wheel to horizontal scroll; first N wheel ticks at each end ignored (see kEndWheelSwallowsBeforeHorizontal). */
static void RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGuiTable* table) {
    constexpr int kEndWheelSwallowsBeforeHorizontal = 6;

    if (!table) {
        return;
    }
    ImGuiWindow* inner = table->InnerWindow;
    ImGuiWindow* outer = table->OuterWindow;
    ImGuiIO& io = ImGui::GetIO();

    if (!inner ||
        std::abs(io.MouseWheel) <= 0.0f ||
        std::abs(io.MouseWheelH) >= 0.0001f ||
        inner->ScrollMax.x <= 0.0f) {
        return;
    }

    const float eps = 1.0f;
    const bool atBottom = inner->Scroll.y >= inner->ScrollMax.y - eps;
    const bool atTop = inner->Scroll.y <= eps;
    const bool noVerticalScroll = inner->ScrollMax.y <= eps;

    if (outer) {
        const ImRect tableRect = outer->Rect();
        if (!ImGui::IsMouseHoveringRect(tableRect.Min, tableRect.Max, false)) {
            return;
        }
    }

    if (!atBottom) {
        g_ui.gridBottomHorizontalWheelSwallowsRemaining = 0;
    }
    if (!atTop) {
        g_ui.gridTopHorizontalWheelSwallowsRemaining = 0;
    }

    bool allowRoute = false;
    if (noVerticalScroll) {
        allowRoute = true;
    } else if (atBottom) {
        if (g_ui.gridBottomHorizontalWheelSwallowsRemaining < kEndWheelSwallowsBeforeHorizontal) {
            ++g_ui.gridBottomHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    } else if (atTop) {
        if (g_ui.gridTopHorizontalWheelSwallowsRemaining < kEndWheelSwallowsBeforeHorizontal) {
            ++g_ui.gridTopHorizontalWheelSwallowsRemaining;
            return;
        }
        allowRoute = true;
    }

    if (!allowRoute) {
        return;
    }

    const float wheelToScrollX = -io.MouseWheel * (ImGui::GetTextLineHeightWithSpacing() * 3.0f);
    float targetX = inner->Scroll.x + wheelToScrollX;
    if (targetX < 0.0f) {
        targetX = 0.0f;
    }
    if (targetX > inner->ScrollMax.x) {
        targetX = inner->ScrollMax.x;
    }
    ImGui::SetScrollX(inner, targetX);
}


std::string BuildCellKey(const std::string& issueId, const std::string& fieldId) {
    return issueId + "|" + fieldId;
}

static std::string BuildJiraBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) {
    if (cfg.Domain.empty() || issueKey.empty()) {
        return std::string();
    }
    std::string base = cfg.Domain;
    if (base.find("http://") != 0 && base.find("https://") != 0) {
        base = "https://" + base;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base + "/browse/" + issueKey;
}

static void SyncWithCurrentView(AppController& app,
                                UiDrawSession& d,
                                const ViewsStore& store,
                                bool pushHistory) {
    ConfigManager::Save(d.cfg);
    if (pushHistory) {
        d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
    }
    app.SyncWithBackend(&d.cfg, &store);
}

/**
 * Ensure d.newIssueDraftEditBufs has a sized InputText buffer for `fieldId`,
 * seeded from `seed`. Buffer is large enough for summary-ish lines; the
 * description column gets a bigger allocation via the caller's override.
 * Description uses ImGui::InputText (single-line); Jira often returns multiline
 * text — normalize CR/LF to spaces in the initial seed so the control stays one line.
 */
static std::vector<char>& EnsureDraftEditBuf(UiDrawSession& d,
                                              const std::string& fieldId,
                                              const std::string& seed,
                                              size_t minSize = 512) {
    auto& buf = d.newIssueDraftEditBufs[fieldId];
    if (buf.empty()) {
        buf.resize(std::max(minSize, seed.size() + 64), '\0');
        std::memcpy(buf.data(), seed.data(), std::min(buf.size() - 1, seed.size()));
        if (fieldId == "description") {
            for (size_t i = 0; i < buf.size() && buf[i] != '\0'; ++i) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    buf[i] = ' ';
                }
            }
        }
    }
    return buf;
}

static std::string FormatBytesUi(std::uint64_t n) {
    if (n < 1024) {
        return std::to_string(n) + " B";
    }
    const char* suffix = "KB";
    double v = static_cast<double>(n) / 1024.0;
    if (v >= 1024.0) {
        v /= 1024.0;
        suffix = "MB";
    }
    if (v >= 1024.0) {
        v /= 1024.0;
        suffix = "GB";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, suffix);
    return std::string(buf);
}

static bool DraftHasStagedPath(const IssueDraft& draft, const ghc::filesystem::path& absPath) {
    std::error_code ec;
    for (const auto& a : draft.StagedAttachments) {
        if (a.AbsPath.empty()) {
            continue;
        }
        if (ghc::filesystem::equivalent(absPath, ghc::filesystem::path(a.AbsPath), ec)) {
            return true;
        }
        ec.clear();
    }
    return false;
}

static bool TryAppendStagedFromAbsPath(IssueDraft& draft, const std::string& absUtf8) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    const fs::path p(absUtf8);
    if (!fs::is_regular_file(p, ec)) {
        return false;
    }
    if (DraftHasStagedPath(draft, p)) {
        return false;
    }
    StagedAttachment sa;
    sa.AbsPath = absUtf8;
    sa.FileName = p.filename().string();
    sa.SizeBytes = fs::file_size(p, ec);
    draft.StagedAttachments.push_back(std::move(sa));
    return true;
}

static void RenderNewIssueDraftRow(AppController& app,
                                    UiDrawSession& d,
                                    const std::vector<TicketGridColumn>& columns,
                                    const JiraConfig& cfg) {
    const auto& catalog = app.GetAvailableJiraFields();

    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid() &&
        d.newIssueCreateFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        IssueCreateResult r = d.newIssueCreateFuture.get();
        d.newIssueCreateInFlight = false;
        if (d.newIssueDiscardAsyncCreateResult) {
            d.newIssueDiscardAsyncCreateResult = false;
        } else if (r.Ok) {
            d.newIssueLastSuccessKey = r.IssueKey;
            d.newIssueLastError.clear();
            d.newIssueMissingFieldIds.clear();
            d.newIssueDraftActive = false;
            d.newIssueDraft = IssueDraft{};
            d.newIssueDraftEditBufs.clear();
            d.gridEditSuccess = "Created " + r.IssueKey + ".";
        } else {
            d.newIssueLastError = r.Error;
            d.newIssueMissingFieldIds = r.MissingFieldIds;
            d.newIssueLastSuccessKey.clear();
        }
    }

    ImGui::TableNextRow();

    if (!d.newIssueDraftActive) {
        ImGui::TableSetColumnIndex(0);
        if (ImGui::SmallButton("+ New issue")) {
            d.newIssueDraft = app.BuildDraftFromLastTicket(cfg);
            if (!d.newIssueDraft.ParentKey.empty()) {
                d.newIssueDraft.FieldValues["parent"] = d.newIssueDraft.ParentKey;
            }
            d.newIssueDraftActive = true;
            d.newIssueDraftEditBufs.clear();
            d.newIssueMissingFieldIds.clear();
            d.newIssueLastError.clear();
            d.newIssueLastSuccessKey.clear();
        }
        if (!d.newIssueLastSuccessKey.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "OK %s", d.newIssueLastSuccessKey.c_str());
        } else if (!d.newIssueLastError.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", d.newIssueLastError.c_str());
        }
        return;
    }

    // Active draft: ID column gets Create/Cancel; other columns get per-field inputs.
    const std::unordered_set<std::string> missing(d.newIssueMissingFieldIds.begin(),
                                                   d.newIssueMissingFieldIds.end());

    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
        const auto& column = columns[static_cast<size_t>(colIndex)];
        ImGui::TableSetColumnIndex(colIndex);

        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
            ImGui::PushID("newissue_draft_id");
            ImGui::BeginGroup();
            ImGui::TextDisabled("[new]");

            const bool disabled = d.newIssueCreateInFlight;
            const auto runAttachmentPicker = [&]() {
                app.RequestOpenFilePaths(true, d.cfg.LastImportDirectory, [&d](std::vector<std::string> paths) {
                    for (const auto& path : paths) {
                        TryAppendStagedFromAbsPath(d.newIssueDraft, path);
                    }
                });
            };

            const ImVec2 draftActionBtn(ImGui::GetContentRegionAvail().x, 0.0f);
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Create", draftActionBtn)) {
                // Flush edit buffers into draft field values just in case.
                for (auto& kv : d.newIssueDraftEditBufs) {
                    d.newIssueDraft.FieldValues[kv.first] = std::string(kv.second.data());
                }
                // Parent key for create comes from the `parent` grid column (FieldValues), not the payload map.
                auto parentIt = d.newIssueDraft.FieldValues.find("parent");
                if (parentIt != d.newIssueDraft.FieldValues.end() && !parentIt->second.empty()) {
                    std::string parentKey = parentIt->second;
                    const size_t sep = parentKey.find(" - ");
                    if (sep != std::string::npos) parentKey = parentKey.substr(0, sep);
                    d.newIssueDraft.ParentKey = parentKey;
                    d.newIssueDraft.FieldValues.erase(parentIt);
                }
                d.newIssueMissingFieldIds = IssueDraftHelpers::MissingRequiredFields(
                    d.newIssueDraft,
                    app.GetRequiredFieldSet(d.newIssueDraft.ProjectKey,
                                             d.newIssueDraft.IssueTypeId,
                                             d.newIssueDraft.IssueTypeName));
                if (!d.newIssueMissingFieldIds.empty()) {
                    d.newIssueLastError = "Missing required fields.";
                } else {
                    d.newIssueLastError.clear();
                    d.newIssueCreateFuture = app.CreateIssueAsync(d.newIssueDraft);
                    d.newIssueCreateInFlight = true;
                }
            }
            if (ImGui::Button("Queue", draftActionBtn)) {
                for (auto& kv : d.newIssueDraftEditBufs) {
                    d.newIssueDraft.FieldValues[kv.first] = std::string(kv.second.data());
                }
                auto parentIt = d.newIssueDraft.FieldValues.find("parent");
                if (parentIt != d.newIssueDraft.FieldValues.end() && !parentIt->second.empty()) {
                    std::string parentKey = parentIt->second;
                    const size_t sep = parentKey.find(" - ");
                    if (sep != std::string::npos) parentKey = parentKey.substr(0, sep);
                    d.newIssueDraft.ParentKey = parentKey;
                    d.newIssueDraft.FieldValues.erase(parentIt);
                }
                const std::int64_t qid = app.QueueCreateOffline(d.newIssueDraft);
                if (qid > 0) {
                    d.gridEditSuccess = "Queued offline.";
                    d.newIssueDraftActive = false;
                    d.newIssueDraft = IssueDraft{};
                    d.newIssueDraftEditBufs.clear();
                    d.newIssueMissingFieldIds.clear();
                    d.newIssueLastError.clear();
                } else {
                    d.newIssueLastError = "Failed to queue offline.";
                }
            }
            if (ImGui::Button("Cancel", draftActionBtn)) {
                d.newIssueDraftActive = false;
                d.newIssueDraft = IssueDraft{};
                d.newIssueDraftEditBufs.clear();
                d.newIssueMissingFieldIds.clear();
                d.newIssueLastError.clear();
            }
            if (ImGui::Button("Add attachment...", draftActionBtn)) {
                runAttachmentPicker();
            }
            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (ImGui::Button("Clear all##clratt", draftActionBtn)) {
                    d.newIssueDraft.StagedAttachments.clear();
                }
            }
            if (disabled) ImGui::EndDisabled();

            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (disabled) ImGui::BeginDisabled();
                constexpr float kChipStripH = 54.0f;
                ImGui::BeginChild("##newissueattstrip",
                                  ImVec2(0.0f, kChipStripH),
                                  true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = 0; i < d.newIssueDraft.StagedAttachments.size();) {
                    const auto& att = d.newIssueDraft.StagedAttachments[i];
                    ImGui::PushID(static_cast<int>(i));
                    std::string label = att.FileName;
                    if (label.size() > 36) {
                        label = label.substr(0, 16) + "..." + label.substr(label.size() - 16);
                    }
                    const std::string chip = label + " (" + FormatBytesUi(att.SizeBytes) + ")";
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(chip.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x##rma")) {
                        d.newIssueDraft.StagedAttachments.erase(d.newIssueDraft.StagedAttachments.begin() +
                                                                static_cast<std::ptrdiff_t>(i));
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::Dummy(ImVec2(8.0f, 1.0f));
                    ImGui::SameLine();
                    ImGui::PopID();
                    ++i;
                }
                ImGui::EndChild();
                if (disabled) ImGui::EndDisabled();
            }

            ImGui::EndGroup();
            if (ImGui::BeginPopupContextItem("newissue_draft_att_ctx")) {
                if (ImGui::MenuItem("Add attachment...")) {
                    if (!d.newIssueCreateInFlight) {
                        runAttachmentPicker();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            continue;
        }

        const std::string& fieldId = column.FieldId;
        if (fieldId == "id" ||
            (IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) && fieldId != "status")) {
            ImGui::TextDisabled("-");
            continue;
        }

        const JiraField* field = nullptr;
        for (const auto& f : catalog) {
            if (f.Id == fieldId) { field = &f; break; }
        }

        const bool isMissing = missing.count(fieldId) > 0 ||
                                (fieldId == "summary" && missing.count("summary") > 0) ||
                                (fieldId == "parent" && missing.count("__parent__") > 0);
        if (isMissing) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 0.45f));
        }

        ImGui::PushID(fieldId.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);

        const std::string& current = d.newIssueDraft.FieldValues[fieldId];

        // Project/IssueType dropdowns come from catalog option sets when available.
        if (fieldId == "issuetype" || fieldId == "project" ||
            (field && !field->IsArray && !field->AllowedValueOptions.empty() &&
             (field->Family == JiraFieldFamily::SelectSingle ||
              field->Family == JiraFieldFamily::StructuredSingle ||
              field->Family == JiraFieldFamily::IssueType ||
              field->Family == JiraFieldFamily::Status ||
              field->Family == JiraFieldFamily::Sprint ||
              field->IsUserType))) {
            std::string preview = current;
            if (fieldId == "project" && preview.empty()) preview = d.newIssueDraft.ProjectKey;
            if (fieldId == "issuetype" && preview.empty()) preview = d.newIssueDraft.IssueTypeName;
            if (field && !field->AllowedValueOptions.empty()) {
                for (const auto& opt : field->AllowedValueOptions) {
                    if (opt.Id == current || opt.Value == current) {
                        preview = opt.Value;
                        break;
                    }
                }
            }
            if (ImGui::BeginCombo("##combo", preview.empty() ? "(choose)" : preview.c_str())) {
                if (field) {
                    for (const auto& opt : field->AllowedValueOptions) {
                        const bool selected = (opt.Id == current || opt.Value == current);
                        if (ImGui::Selectable(opt.Value.c_str(), selected)) {
                            if (fieldId == "issuetype") {
                                d.newIssueDraft.IssueTypeId = opt.Id;
                                d.newIssueDraft.IssueTypeName = opt.Value;
                            } else if (fieldId == "project") {
                                d.newIssueDraft.ProjectKey = opt.Id.empty() ? opt.Value : opt.Id;
                            } else {
                                // Store option id (e.g. accountId for users) so create payload matches grid edits.
                                d.newIssueDraft.FieldValues[fieldId] = opt.Id.empty() ? opt.Value : opt.Id;
                            }
                        }
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            const size_t minBuf = (fieldId == "description") ? size_t(4096) : size_t(512);
            auto& buf = EnsureDraftEditBuf(d, fieldId, current, minBuf);
            if (ImGui::InputText("##input", buf.data(), buf.size())) {
                d.newIssueDraft.FieldValues[fieldId] = std::string(buf.data());
            }
        }
        ImGui::PopID();

        if (isMissing) {
            ImGui::PopStyleColor();
        }
    }
}

static std::string BuildTemplateCommentBody(const std::string& issueKey, const std::string& templateId) {
    if (templateId == "need_repro") {
        return "Need reproduction details for " + issueKey +
               ":\n- Repro steps\n- Expected vs actual result\n- Branch / CL / build\n- Environment details";
    }
    if (templateId == "need_logs") {
        return "Please attach diagnostic data for " + issueKey +
               ":\n- Relevant logs\n- Callstack / crash context\n- Local repro notes";
    }
    return "Triage handoff for " + issueKey +
           ":\n- Current owner: \n- Next action: \n- ETA: \n- Blockers:";
}

class TicketFieldEditor {
public:
    static void RenderFieldCell(AppController& app,
                                const CachedTicket& ticket,
                                const TicketGridColumn& column,
                                const JiraField* field,
                                const std::string& currentValue,
                                float availWidth,
                                bool tooltipsEnabled,
                                bool allowEdits,
                                SpreadsheetState& state,
                                std::vector<PendingFieldEdit>& pendingEdits,
                                JiraGridFieldAsyncState& jiraGridAsync) {
        const bool handledByLua = app.TryLuaFieldDisplay(column.FieldId, ticket, currentValue, availWidth, field);
        if (handledByLua) {
            return;
        }

        auto renderPlainText = [&](bool disabled) {
            const std::string display = DisplayValueForJiraDateField(column.FieldId, field, currentValue);
            const std::string* tip = column.IsDateLike ? &currentValue : nullptr;
            RenderClippedFieldText(display, availWidth, tooltipsEnabled, disabled, tip);
        };

        switch (column.Plan) {
        case TicketGridColumn::RenderPlan::SpecialAttachment:
            if (field == nullptr || IsAttachmentFieldId(field->Id)) {
                JiraGridFieldDisplay::RenderAttachmentsField(app, currentValue, availWidth, tooltipsEnabled);
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::SpecialWatchers:
            if (field == nullptr || JiraGridFieldDisplay::IsWatchersColumnId(field->Id)) {
                JiraGridFieldDisplay::RenderWatchersField(
                    ticket.id, currentValue, availWidth, tooltipsEnabled, jiraGridAsync);
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::SpecialVotes:
            if (field == nullptr || JiraGridFieldDisplay::IsVotesColumnId(field->Id)) {
                JiraGridFieldDisplay::RenderVotesField(
                    ticket.id, currentValue, availWidth, tooltipsEnabled, jiraGridAsync);
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::SpecialWorklog:
            if (field == nullptr || JiraGridFieldDisplay::IsWorklogColumnId(field->Id)) {
                JiraGridFieldDisplay::RenderWorklogField(currentValue, availWidth, tooltipsEnabled);
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::SpecialProgress:
            if (JiraGridFieldDisplay::TryRenderProgressJsonField(currentValue, availWidth)) {
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::SpecialIssueRestriction:
            if (JiraGridFieldDisplay::TryRenderIssueRestrictionField(currentValue, availWidth, tooltipsEnabled)) {
                return;
            }
            break;
        case TicketGridColumn::RenderPlan::PlainText:
            renderPlainText(column.CatalogReadOnly);
            return;
        case TicketGridColumn::RenderPlan::Labels:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            JiraLabelsEditor::RenderLabelsFieldEditor(
                app,
                ticket,
                *field,
                currentValue,
                [&](const std::string& issueId,
                    const JiraField& fld,
                    const std::vector<std::string>& values) { QueueEdit(issueId, fld, values, pendingEdits); });
            return;
        case TicketGridColumn::RenderPlan::Cascading:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            RenderCascadingSelectEditor(ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
            return;
        case TicketGridColumn::RenderPlan::MultiSelect:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            RenderMultiSelectEditor(ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
            return;
        case TicketGridColumn::RenderPlan::SingleSelect:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            RenderSingleSelectEditor(ticket, *field, currentValue, pendingEdits, tooltipsEnabled);
            return;
        case TicketGridColumn::RenderPlan::DateTimeEditor:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            JiraDateTimeFieldEditor::RenderDateTimeFieldEditor(
                ticket,
                *field,
                currentValue,
                state,
                [&](const std::string& issueId,
                    const JiraField& fld,
                    const std::vector<std::string>& values) { QueueEdit(issueId, fld, values, pendingEdits); });
            return;
        case TicketGridColumn::RenderPlan::TextEditor:
            if (!allowEdits) {
                renderPlainText(true);
                return;
            }
            RenderTextEditor(ticket, *field, currentValue, state, pendingEdits, tooltipsEnabled, availWidth);
            return;
        }
    }

private:
    static void QueueEdit(const std::string& issueId,
                          const JiraField& field,
                          const std::vector<std::string>& values,
                          std::vector<PendingFieldEdit>& pendingEdits) {
        PendingFieldEdit edit;
        edit.IssueId = issueId;
        edit.Field = field;
        edit.Values = values;
        pendingEdits.push_back(std::move(edit));
    }

    static std::string EncodeCascadingSelection(const std::string& parentId, const std::string& childId) {
        return parentId + "\x1f" + childId;
    }

    static const JiraFieldOption* FindOptionRecursive(const std::vector<JiraFieldOption>& options,
                                                      const std::string& value) {
        for (const auto& option : options) {
            if (option.Id == value || option.Value == value) {
                return &option;
            }
            if (!option.Children.empty()) {
                if (const JiraFieldOption* nested = FindOptionRecursive(option.Children, value)) {
                    return nested;
                }
            }
        }
        return nullptr;
    }

    static std::string ResolveOptionId(const JiraField& field, const std::string& value) {
        if (const JiraFieldOption* option = FindOptionRecursive(field.AllowedValueOptions, value)) {
            return option->Id;
        }
        return value;
    }

    static std::string ResolveOptionLabel(const JiraField& field, const std::string& value) {
        if (const JiraFieldOption* option = FindOptionRecursive(field.AllowedValueOptions, value)) {
            return option->Value;
        }
        return value;
    }

    static std::vector<std::string> ResolveCurrentSelectionIds(const JiraField& field, const std::string& currentValue) {
        std::vector<std::string> ids;
        for (const auto& part : ParseCsv(currentValue)) {
            const std::string id = ResolveOptionId(field, part);
            if (!id.empty()) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    static const char* EmptySelectPreviewLabel(const JiraField& field) {
        return field.IsUserType ? "Unassigned" : "<none>";
    }

    static std::string BuildSelectionPreview(const JiraField& field, const std::vector<std::string>& selectedIds) {
        if (selectedIds.empty()) {
            return field.IsUserType ? std::string("Unassigned") : std::string("<none>");
        }
        std::vector<std::string> labels;
        labels.reserve(selectedIds.size());
        for (const auto& id : selectedIds) {
            labels.push_back(ResolveOptionLabel(field, id));
        }
        return JoinCsv(labels);
    }

    static std::string BuildCascadingPreview(const JiraFieldOption& parent, const JiraFieldOption* child) {
        if (child == nullptr) {
            return parent.Value;
        }
        return parent.Value + " > " + child->Value;
    }

    static bool TryResolveCascadingSelection(const JiraField& field,
                                             const std::string& currentValue,
                                             std::string& outParentId,
                                             std::string& outChildId) {
        for (const auto& parent : field.AllowedValueOptions) {
            if (currentValue == parent.Id || currentValue == parent.Value) {
                outParentId = parent.Id;
                outChildId.clear();
                return true;
            }
            for (const auto& child : parent.Children) {
                const std::string display = BuildCascadingPreview(parent, &child);
                if (currentValue == child.Id || currentValue == child.Value || currentValue == display) {
                    outParentId = parent.Id;
                    outChildId = child.Id;
                    return true;
                }
            }
        }
        return false;
    }

    static void RenderTextEditor(const CachedTicket& ticket,
                                 const JiraField& field,
                                 const std::string& currentValue,
                                 SpreadsheetState& state,
                                 std::vector<PendingFieldEdit>& pendingEdits,
                                 bool tooltipsEnabled,
                                 float availWidth) {
        const std::string itemId = "##TextCell_" + ticket.id + "_" + field.Id;
        if (state.IsEditingField(ticket.id, field.Id)) {
            const bool editJustStarted = state.EditJustStarted;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (editJustStarted) {
                ImGui::SetKeyboardFocusHere();
            }
            const bool submitted = ImGui::InputText(
                itemId.c_str(),
                state.EditBuffer,
                sizeof(state.EditBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.ClearEditing();
            } else if (submitted || (!editJustStarted && ImGui::IsItemDeactivatedAfterEdit())) {
                QueueEdit(ticket.id, field, {state.EditBuffer}, pendingEdits);
                state.ClearEditing();
            } else if (editJustStarted) {
                state.EditJustStarted = false;
            }
            return;
        }

        const std::string valueForDisplay = DisplayValueForJiraDateField(field.Id, &field, currentValue);
        bool hasNewlineInValue = false;
        for (size_t i = 0; i < valueForDisplay.size(); ++i) {
            if (valueForDisplay[i] == '\n' || valueForDisplay[i] == '\r') {
                hasNewlineInValue = true;
                break;
            }
        }
        std::string singleLine = valueForDisplay;
        for (size_t i = 0; i < singleLine.size(); ++i) {
            if (singleLine[i] == '\n' || singleLine[i] == '\r') {
                singleLine.erase(i);
                break;
            }
        }
        const std::string& display = singleLine;
        const float regionAvail = (availWidth > 0.0f) ? availWidth : ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable((display + itemId).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(0)) {
                state.StartEditingField(ticket.id, field.Id, currentValue);
            }
        }
        const ImVec2 textSize = ImGui::CalcTextSize(display.c_str());
        const bool horizontallyClipped = (regionAvail > 0.0f && textSize.x > regionAvail + 1.0f);
        if (tooltipsEnabled && (hasNewlineInValue || horizontallyClipped) && ImGui::IsItemHovered()) {
            const std::string* rawTip = IsJiraDateOrDateTimeField(field.Id, &field) ? &currentValue : nullptr;
            const std::string& tipSource =
                (rawTip && !rawTip->empty()) ? *rawTip : valueForDisplay;
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
            ImGui::TextUnformatted(tipSource.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    static void RenderSingleSelectEditor(const CachedTicket& ticket,
                                         const JiraField& field,
                                         const std::string& currentValue,
                                         std::vector<PendingFieldEdit>& pendingEdits,
                                         bool tooltipsEnabled) {
        const std::string currentId = ResolveOptionId(field, currentValue);
        const std::string preview = ResolveOptionLabel(field, currentId);
        const std::string comboId = "##SingleSelect_" + ticket.id + "_" + field.Id;
        const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
        const char* previewCStr = preview.empty() ? EmptySelectPreviewLabel(field) : preview.c_str();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(comboId.c_str(), previewCStr, ImGuiComboFlags_NoArrowButton)) {
            const bool selectedNone = currentId.empty();
            if (ImGui::Selectable("<clear>", selectedNone)) {
                QueueEdit(ticket.id, field, {}, pendingEdits);
            }

            for (const auto& option : field.AllowedValueOptions) {
                const bool isSelected = (option.Id == currentId);
                if (ImGui::Selectable(option.Value.c_str(), isSelected)) {
                    QueueEdit(ticket.id, field, {option.Id}, pendingEdits);
                }
            }
            ImGui::EndCombo();
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const ImVec2 psz = ImGui::CalcTextSize(previewCStr);
            const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(previewCStr);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }

    static void RenderMultiSelectEditor(const CachedTicket& ticket,
                                        const JiraField& field,
                                        const std::string& currentValue,
                                        std::vector<PendingFieldEdit>& pendingEdits,
                                        bool tooltipsEnabled) {
        std::vector<std::string> selectedIds = ResolveCurrentSelectionIds(field, currentValue);
        std::unordered_set<std::string> selectedSet(selectedIds.begin(), selectedIds.end());
        const std::string preview = BuildSelectionPreview(field, selectedIds);
        const std::string comboId = "##MultiSelect_" + ticket.id + "_" + field.Id;
        const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
            if (ImGui::Selectable("<clear all>", selectedSet.empty())) {
                QueueEdit(ticket.id, field, {}, pendingEdits);
            }
            ImGui::Separator();

            for (const auto& option : field.AllowedValueOptions) {
                bool checked = (selectedSet.find(option.Id) != selectedSet.end());
                const std::string optionWidget = option.Value + "##Opt_" + ticket.id + "_" + field.Id + "_" + option.Id;
                if (ImGui::Checkbox(optionWidget.c_str(), &checked)) {
                    if (checked) {
                        selectedSet.insert(option.Id);
                    } else {
                        selectedSet.erase(option.Id);
                    }
                    std::vector<std::string> updated(selectedSet.begin(), selectedSet.end());
                    std::sort(updated.begin(), updated.end());
                    QueueEdit(ticket.id, field, updated, pendingEdits);
                }
            }
            ImGui::EndCombo();
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
            const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(preview.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }

    static void RenderCascadingSelectEditor(const CachedTicket& ticket,
                                            const JiraField& field,
                                            const std::string& currentValue,
                                            std::vector<PendingFieldEdit>& pendingEdits,
                                            bool tooltipsEnabled) {
        std::string parentId;
        std::string childId;
        TryResolveCascadingSelection(field, currentValue, parentId, childId);
        std::string preview = currentValue.empty() ? std::string("<none>") : currentValue;
        for (const auto& parent : field.AllowedValueOptions) {
            if (parent.Id != parentId) {
                continue;
            }
            preview = BuildCascadingPreview(parent, childId.empty() ? nullptr : FindOptionRecursive(parent.Children, childId));
            break;
        }

        const std::string comboId = "##CascadeSelect_" + ticket.id + "_" + field.Id;
        const float comboAvailBefore = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(comboId.c_str(), preview.c_str(), ImGuiComboFlags_NoArrowButton)) {
            if (ImGui::Selectable("<clear>", parentId.empty() && childId.empty())) {
                QueueEdit(ticket.id, field, {}, pendingEdits);
            }
            ImGui::Separator();
            for (const auto& parent : field.AllowedValueOptions) {
                if (parent.Children.empty()) {
                    const bool selected = (parent.Id == parentId && childId.empty());
                    if (ImGui::Selectable(parent.Value.c_str(), selected)) {
                        QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                    }
                    continue;
                }

                if (ImGui::BeginMenu(parent.Value.c_str())) {
                    const bool parentOnlySelected = (parent.Id == parentId && childId.empty());
                    if (ImGui::Selectable("<parent only>", parentOnlySelected)) {
                        QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, std::string())}, pendingEdits);
                    }
                    ImGui::Separator();
                    for (const auto& child : parent.Children) {
                        const bool selected = (parent.Id == parentId && child.Id == childId);
                        if (ImGui::Selectable(child.Value.c_str(), selected)) {
                            QueueEdit(ticket.id, field, {EncodeCascadingSelection(parent.Id, child.Id)}, pendingEdits);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndCombo();
        }
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            const ImVec2 psz = ImGui::CalcTextSize(preview.c_str());
            const bool previewClipped = (comboAvailBefore > 0.0f && psz.x > comboAvailBefore + 1.0f);
            if (previewClipped) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
                ImGui::TextUnformatted(preview.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
};

/**
 * Right-click on the cell group: Copy (plain RMB). Shift+RMB: full raw cached value panel.
 * Uses OpenPopup — not BeginPopupContextItem — so Shift+RMB is not swallowed by the Copy menu.
 */
static void DrawGridCellRightClickPopups(const std::string& imguiStackId,
                                         const std::string& issueKey,
                                         const std::string& fieldId,
                                         const std::string& fieldLabel,
                                         const std::string& rawValue,
                                         AppController* app,
                                         UiDrawSession* ui,
                                         bool readOnlyMode) {
    ImGui::PushID(imguiStackId.c_str());
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        if (ImGui::GetIO().KeyShift) {
            ImGui::SetNextWindowSize(ImVec2(520.0f, 300.0f), ImGuiCond_FirstUseEver);
            ImGui::OpenPopup("cell_raw_cached");
        } else {
            ImGui::OpenPopup("cell_copy_quick");
        }
    }
    if (ImGui::BeginPopup("cell_raw_cached")) {
        ImGui::TextUnformatted("Raw cached value");
        ImGui::Separator();
        ImGui::Text("Issue: %s", issueKey.c_str());
        if (!fieldId.empty()) {
            ImGui::Text("Field: %s (%s)", fieldLabel.c_str(), fieldId.c_str());
        } else {
            ImGui::Text("Field: %s", fieldLabel.c_str());
        }
        ImGui::TextUnformatted("Value:");
        if (rawValue.empty()) {
            ImGui::TextDisabled("(empty)");
        } else {
            ImGui::BeginChild("cell_raw_cached_body", ImVec2(0, 140.0f), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            ImGui::TextUnformatted(rawValue.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
        }
        if (ImGui::MenuItem("Copy value")) {
            ImGui::SetClipboardText(rawValue.c_str());
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("cell_copy_quick")) {
        if (ImGui::MenuItem("Copy")) {
            ImGui::SetClipboardText(rawValue.c_str());
        }
        if (app && ui && fieldId.empty() && !issueKey.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Quick comment templates");
            if (readOnlyMode) {
                ImGui::TextDisabled("(disabled while offline/read-only)");
            } else {
                auto postTemplate = [&](const char* title, const char* id) {
                    if (ImGui::MenuItem(title)) {
                        std::string err;
                        if (app->JiraAddIssueCommentPlain(issueKey, BuildTemplateCommentBody(issueKey, id), err)) {
                            ui->gridEditError.clear();
                            ui->gridEditSuccess = std::string("Posted template comment to ") + issueKey + ".";
                        } else {
                            ui->gridEditSuccess.clear();
                            ui->gridEditError = err.empty() ? "Failed to post Jira comment." : err;
                        }
                    }
                };
                postTemplate("Need repro details", "need_repro");
                postTemplate("Need logs / diagnostics", "need_logs");
                postTemplate("Triage handoff summary", "handoff");
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void DrawTicketGridHeaderContextMenu(const TicketGridColumn& col, const JiraField* meta) {
    // Power-user only: Shift + right-click on header (plain RMB keeps default table header behavior).
    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
        ImGui::GetIO().KeyShift) {
        ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(520.0f, 420.0f), ImGuiCond_FirstUseEver);
        ImGui::OpenPopup("grid_hdr_meta");
    }
    if (!ImGui::BeginPopup("grid_hdr_meta")) {
        return;
    }

    if (col.ColumnKind == TicketGridColumn::Kind::Id) {
        ImGui::TextUnformatted("Issue key column (not a Jira field)");
        ImGui::Separator();
        ImGui::Text("Key: %s", col.Key.c_str());
        ImGui::Text("Label: %s", col.Label.c_str());
        if (ImGui::MenuItem("Copy column key")) {
            ImGui::SetClipboardText(col.Key.c_str());
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("Jira field (catalog)");
    ImGui::Separator();

    const std::string& fieldIdForCopy = meta ? meta->Id : col.FieldId;
    if (ImGui::MenuItem("Copy field id")) {
        ImGui::SetClipboardText(fieldIdForCopy.c_str());
    }

    if (meta) {
        ImGui::Text("Id: %s", meta->Id.c_str());
        ImGui::Text("Name: %s", meta->Name.c_str());
        ImGui::Text("Type: %s", meta->Type.c_str());
        ImGui::Text("ReadOnly: %s", meta->ReadOnly ? "true" : "false");
        ImGui::Text("IsCustom: %s", meta->IsCustom ? "true" : "false");
        ImGui::Text("IsArray: %s", meta->IsArray ? "true" : "false");
        ImGui::Text("ItemsType: %s", meta->ItemsType.empty() ? "(none)" : meta->ItemsType.c_str());
        ImGui::Text("IsUserType: %s", meta->IsUserType ? "true" : "false");
        ImGui::Text("AllowedValues count: %d", static_cast<int>(meta->AllowedValues.size()));
        ImGui::Text("AllowedValueOptions count: %d", static_cast<int>(meta->AllowedValueOptions.size()));

        const int optCount = static_cast<int>(meta->AllowedValueOptions.size());
        if (optCount > 0) {
            ImGui::Separator();
            ImGui::TextUnformatted("AllowedValueOptions (scroll)");
            ImGui::BeginChild("hdr_allowed_opts", ImVec2(0, 100.0f), true);
            const int show = (optCount < 200) ? optCount : 200;
            for (int i = 0; i < show; ++i) {
                const JiraFieldOption& o = meta->AllowedValueOptions[static_cast<size_t>(i)];
                ImGui::BulletText("id=%s  value=%s", o.Id.c_str(), o.Value.c_str());
            }
            if (optCount > show) {
                ImGui::TextDisabled("... %d more", optCount - show);
            }
            ImGui::EndChild();
        }

        const int valCount = static_cast<int>(meta->AllowedValues.size());
        if (valCount > 0 && meta->AllowedValueOptions.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted("AllowedValues (scroll)");
            ImGui::BeginChild("hdr_allowed_vals", ImVec2(0, 80.0f), true);
            const int show = (valCount < 200) ? valCount : 200;
            for (int i = 0; i < show; ++i) {
                ImGui::BulletText("%s", meta->AllowedValues[static_cast<size_t>(i)].c_str());
            }
            if (valCount > show) {
                ImGui::TextDisabled("... %d more", valCount - show);
            }
            ImGui::EndChild();
        }
    } else {
        ImGui::TextDisabled("No catalog entry for field id: %s", col.FieldId.c_str());
        ImGui::Text("Column key: %s", col.Key.c_str());
        ImGui::Text("Header label: %s", col.Label.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("GET /rest/api/3/field (raw object)");
    if (meta && !meta->RestFieldDefinitionJson.empty()) {
        if (ImGui::MenuItem("Copy raw JSON")) {
            ImGui::SetClipboardText(meta->RestFieldDefinitionJson.c_str());
        }
        ImGui::BeginChild("hdr_raw_json", ImVec2(0, 140.0f), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(meta->RestFieldDefinitionJson.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("(not available — refresh field catalog or synthetic field)");
    }

    ImGui::EndPopup();
}

}

void RenderClippedFieldText(const std::string& rawValue,
                            float availWidth,
                            bool tooltipsEnabled,
                            bool disabled,
                            const std::string* rawForTooltip) {
    const std::string& displayValue = rawValue;

    bool hasNewline = false;
    std::string singleLine = displayValue;
    for (size_t i = 0; i < singleLine.size(); ++i) {
        if (singleLine[i] == '\n' || singleLine[i] == '\r') {
            singleLine.erase(i);
            hasNewline = true;
            break;
        }
    }

    const ImVec2 textSize = ImGui::CalcTextSize(singleLine.c_str());
    const bool horizontallyClipped = (availWidth > 0.0f && textSize.x > availWidth + 1.0f);

    if (disabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    ImGui::TextUnformatted(singleLine.c_str());
    if (disabled) {
        ImGui::PopStyleColor();
    }

    const std::string& tipSource =
        (rawForTooltip && !rawForTooltip->empty()) ? *rawForTooltip : displayValue;
    if (tooltipsEnabled && (hasNewline || horizontallyClipped) && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 48.0f);
        ImGui::TextUnformatted(tipSource.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void ApplyLoggingSettingsFromConfig(const JiraConfig& cfg) {
    Logger::Instance().SetMinLevel(Logger::ParseLogLevelString(cfg.LogMinLevel, LogLevel::Info));
    Logger::Instance().SetLogJiraHttpBodies(cfg.LogJiraHttpBodies);
    Logger::Instance().SetLogP4Io(cfg.LogP4Io);
}

static void PersistPerformanceWindowPreference(UiDrawSession& d) {
    if (d.cfg.ShowPerformanceWindow == d.showPerformance) {
        return;
    }
    d.cfg.ShowPerformanceWindow = d.showPerformance;
    ConfigManager::Save(d.cfg);
}

void SmatchetUI::Draw(AppController& app) {
    if (!g_ui.cfgInitialized) {
        g_ui.cfg = ConfigManager::Load();
        g_ui.showPerformance = g_ui.cfg.ShowPerformanceWindow;
        g_ui.cfgInitialized = true;
        ApplyLoggingSettingsFromConfig(g_ui.cfg);
    }
    // Drain the offline create queue opportunistically (rate-limited internally).
    app.TickOfflineCreates();
    if (!g_ui.attachmentPreviewCallbackRegistered) {
        app.SetAttachmentPreviewHandler([](const std::string& localPath,
                                           const std::string& mimeType,
                                           const std::string& filename,
                                           const std::string& sourceUrl) -> bool {
            if (!IsSupportedImageMime(mimeType)) {
                return false;
            }
            std::lock_guard<std::mutex> lock(g_ui.attachmentPreviewMutex);
            AttachmentPreviewUpdate update;
            update.LocalPath = localPath;
            update.MimeType = mimeType;
            update.Filename = filename;
            update.Url = sourceUrl;
            g_ui.attachmentPreviewUpdateQueue.push_back(std::move(update));
            return true;
        });
        app.SetAttachmentCollectionHandler(
            [](const std::vector<AppController::AttachmentDescriptor>& attachments) {
                std::lock_guard<std::mutex> lock(g_ui.attachmentPreviewMutex);
                AttachmentCollectionRequest request;
                request.Attachments = attachments;
                g_ui.attachmentCollectionQueue.push_back(std::move(request));
            });
        g_ui.attachmentPreviewCallbackRegistered = true;
    }
    if (!g_openFilePathsHandlerInstalled) {
#if defined(_WIN32)
        app.SetOpenFilePathsHandler([](bool allowMultiple,
                                       const std::string& initialDirectoryUtf8,
                                       std::function<void(std::vector<std::string>)> onComplete) {
            // GLFW: PlatformHandle is GLFWwindow*; PlatformHandleRaw is HWND (see imgui_impl_glfw).
            void* hwnd = nullptr;
            if (ImGui::GetCurrentContext()) {
                if (const ImGuiViewport* vp = ImGui::GetMainViewport()) {
                    hwnd = vp->PlatformHandleRaw;
                }
            }
            std::vector<std::string> paths;
            std::string lastDir;
            if (SmatchetWin32PickOpenFilePaths(
                    hwnd, allowMultiple, initialDirectoryUtf8, &lastDir, paths)) {
                if (!lastDir.empty()) {
                    g_ui.cfg.LastImportDirectory = std::move(lastDir);
                    ConfigManager::Save(g_ui.cfg);
                }
            }
            if (onComplete) {
                onComplete(std::move(paths));
            }
        });
#endif
        g_openFilePathsHandlerInstalled = true;
    }
    UiPerfMonitor::Instance().BeginFrame();
    SMATCHET_UI_PERF_SCOPE("SmatchetUI::Draw");
    {
        SMATCHET_UI_PERF_SCOPE("ViewState::EnsureLoaded");
        ViewState.EnsureLoaded(g_ui.cfg);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawEnsureCatalogAndInitialSync");
        drawEnsureCatalogAndInitialSync(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawMainMenuBar");
        drawMainMenuBar(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("SmatchetPerfUi::DrawWindow");
        g_perfUi.DrawWindow(&g_ui.showPerformance);
        PersistPerformanceWindowPreference(g_ui);
    }
    blameAnalysisUi_.SetBlamePanelOpen(g_ui.showBlameAnalysis);
    blameAnalysisUi_.ServiceBackground();
    if (g_ui.showBlameAnalysis) {
        blameAnalysisUi_.DrawWindow(app, &g_ui.showBlameAnalysis, g_ui.gridState.SelectedId);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawPreferencesWindow");
        drawPreferencesWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawViewsDashboardWindow");
        drawViewsDashboardWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawActiveProjectWindow");
        drawActiveProjectWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawAttachmentPreviewWindow");
        drawAttachmentPreviewWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawBulkImportWindow");
        drawBulkImportWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawBulkExportWindow");
        drawBulkExportWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("JiraGridFieldDisplay::DrawWatchersListWindow");
        JiraGridFieldDisplay::DrawWatchersListWindow(g_ui.jiraGridAsync);
    }
    {
        SMATCHET_UI_PERF_SCOPE("JiraGridFieldDisplay::DrawVotesListWindow");
        JiraGridFieldDisplay::DrawVotesListWindow(g_ui.jiraGridAsync);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawAIAssistantWindow");
        drawAIAssistantWindow(app);
    }
    {
        SMATCHET_UI_PERF_SCOPE("drawLogWindow");
        drawLogWindow();
    }
    if (g_ui.showPerformance) {
        g_perfUi.DrawFpsOverlay();
    }
    if (g_ui.pendingViewStateSave &&
        std::chrono::steady_clock::now() >= g_ui.pendingViewStateSaveAt) {
        SMATCHET_UI_PERF_SCOPE("ViewState::SaveDebounced");
        ViewState.Save();
        g_ui.pendingViewStateSave = false;
    }
}

void SmatchetUI::drawAttachmentPreviewWindow(AppController& app) {
    auto& d = g_ui;
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
            const int eagerRequests = QueuePriorityAttachmentPreviewRequests(
                app, d.attachmentWindowEntries, d.attachmentWindowSelectedIndex, 6);
            LOG_INFO("SmatchetUI: opened attachment gallery total=%d images=%d eager=%d bitmap=%s detail=%s",
                     static_cast<int>(d.attachmentWindowEntries.size()),
                     imageCount,
                     eagerRequests,
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
                    if (!CreateAttachmentTextureFromRgba(
                            rgbaPixels, entry.ImageWidth, entry.ImageHeight, entry.ThumbnailTextureData, uploadError)) {
                        entry.PreviewError = uploadError;
                        LOG_WARN("SmatchetUI: thumbnail upload failed file=%s err=%s",
                                 entry.Filename.c_str(),
                                 uploadError.c_str());
                    } else {
                        LOG_DEBUG("SmatchetUI: thumbnail uploaded file=%s size=%dx%d",
                                  entry.Filename.c_str(),
                                  entry.ImageWidth,
                                  entry.ImageHeight);
                    }
                } else {
                    entry.PreviewError = uploadError;
                    LOG_WARN("SmatchetUI: thumbnail decode failed file=%s err=%s",
                             entry.Filename.c_str(),
                             uploadError.c_str());
                }
#endif
            } else if (!thumbnailSupport.CanRenderBitmapThumbnails && entry.PreviewError.empty()) {
                LOG_DEBUG("SmatchetUI: bitmap thumbnails unavailable for file=%s detail=%s",
                          entry.Filename.c_str(),
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

    ImGui::SetNextWindowSize(ImVec2(1040, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Attachment Preview", &d.attachmentPreviewWindowOpen)) {
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
        const int columnCount = (std::max)(1, static_cast<int>((availableWidth + itemSpacing) / (cardWidth + itemSpacing)));
        if (ImGui::BeginTable("AttachmentGrid", columnCount, ImGuiTableFlags_SizingFixedFit)) {
            for (int i = 0; i < static_cast<int>(d.attachmentWindowEntries.size()); ++i) {
                AttachmentWindowEntry& entry = d.attachmentWindowEntries[static_cast<size_t>(i)];
                const bool selected = (i == d.attachmentWindowSelectedIndex);
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                ImGui::PushStyleColor(
                    ImGuiCol_ChildBg,
                    selected ? ImVec4(0.16f, 0.24f, 0.33f, 0.95f) : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
                ImGui::BeginChild("AttachmentCard",
                                  ImVec2(cardWidth, cardHeight),
                                  true,
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
                    clicked = ImGui::ImageButton(
                        "##attachment_thumb", entry.ThumbnailTextureData->GetTexRef(), thumbDraw);
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
                    } else if (IsSupportedImageMime(entry.MimeType) && entry.PreviewRequestIssued && entry.LocalPath.empty()) {
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
                    drawList->AddRect(ImGui::GetItemRectMin(),
                                      ImGui::GetItemRectMax(),
                                      IM_COL32(90, 170, 255, 255),
                                      6.0f,
                                      0,
                                      2.0f);
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
        } else if (IsSupportedImageMime(selectedEntry.MimeType) &&
                   selectedEntry.ImageWidth > 0 &&
                   selectedEntry.ImageHeight > 0) {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                               "Image preview metadata: %dx%d",
                               selectedEntry.ImageWidth,
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
            app.OpenAttachmentInSystemViewer(selectedEntry.Url,
                                             selectedEntry.Filename,
                                             selectedEntry.MimeType);
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

void SmatchetUI::drawEnsureCatalogAndInitialSync(AppController& app) {
    auto& d = g_ui;
    const auto startCatalogFetch = [&](const JiraConfig& fetchCfg) {
        if (d.fieldCatalogLoading) {
            return;
        }
        d.fieldCatalogLoading = true;
        d.fieldCatalogFetchStarted = true;
        d.fieldCatalogFuture = StartFieldCatalogFetchAsync(fetchCfg);
    };

    if ((!d.fieldCatalogFetchStarted || d.triggerCatalogRefetch) && !d.fieldCatalogLoading) {
        d.triggerCatalogRefetch = false;
        startCatalogFetch(d.cfg);
    }

    if (d.fieldCatalogLoading &&
        d.fieldCatalogFuture.valid() &&
        d.fieldCatalogFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {
            FieldCatalogFetchResult result = d.fieldCatalogFuture.get();
            if (result.Ok) {
                app.SetJiraFieldCatalog(std::move(result.Fields), std::move(result.Components), std::string());
                d.fieldCatalogWarning = result.Warning;
                if (!d.fieldCatalogWarning.empty()) {
                    LOG_WARN("SmatchetUI: users fetch warning: %s", d.fieldCatalogWarning.c_str());
                }
            } else {
                app.SetJiraFieldCatalog({}, {}, result.Error.empty() ? std::string("Failed to fetch Jira field catalog.") : result.Error);
                d.fieldCatalogWarning.clear();
            }
        } catch (const std::exception& ex) {
            app.SetJiraFieldCatalog({}, {}, std::string("Field catalog load failed: ") + ex.what());
            d.fieldCatalogWarning.clear();
            LOG_ERROR("SmatchetUI: field catalog future exception: %s", ex.what());
        } catch (...) {
            app.SetJiraFieldCatalog({}, {}, "Field catalog load failed.");
            d.fieldCatalogWarning.clear();
            LOG_ERROR("SmatchetUI: field catalog future unknown exception");
        }
        d.fieldCatalogLoading = false;
    }

    if (!d.appliedInitialView) {
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            d.cfg.JqlQuery = activeView->Jql;
            d.cfg.SelectedFields = activeView->Fields;
            d.navHistory.Push(NavigationEntry{d.cfg.JqlQuery});
        }
        SyncWithCurrentView(app, d, ViewState.GetStore(), false);
        d.appliedInitialView = true;
    }
}

void SmatchetUI::drawMainMenuBar(AppController& app) {
#ifndef SMATCHET_EMBEDDED_IN_UNREAL
    (void)app;
#endif
    auto& d = g_ui;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Preferences...")) {
                d.showPreferences = true;
            }
            if (ImGui::MenuItem("Performance...")) {
                d.showPerformance = true;
            }
            if (ImGui::MenuItem("Views Dashboard...")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            if (ImGui::MenuItem("Blame Analysis...")) {
                d.showBlameAnalysis = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tickets")) {
            if (ImGui::MenuItem("Bulk import...")) {
                d.showBulkImport = true;
            }
            if (ImGui::MenuItem("Bulk export...")) {
                d.showBulkExport = true;
            }
            ImGui::EndMenu();
        }
#ifdef SMATCHET_EMBEDDED_IN_UNREAL
        {
            const char* closeLabel = "Close";
            const float btnW = ImGui::CalcTextSize(closeLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            constexpr float kRightMargin = 10.0f;
            const float xPos =
                (std::max)(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - btnW - kRightMargin);
            ImGui::SetCursorPosX(xPos);
            if (ImGui::SmallButton(closeLabel)) {
                app.CloseEmbeddedUi();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Hide Smatchet overlay (same as Ctrl+Shift+J)");
            }
        }
#endif
        ImGui::EndMainMenuBar();
    }
}

void SmatchetUI::drawPreferencesWindow(AppController& app) {
    auto& d = g_ui;
    if (!d.showPreferences) {
        d.preferencesBuffersLoaded = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &d.showPreferences)) {
        ImGui::End();
        return;
    }

    if (!d.preferencesBuffersLoaded) {
        CopyStringToBuffer(d.domainBuf, d.cfg.Domain);
        CopyStringToBuffer(d.emailBuf, d.cfg.Email);
        CopyStringToBuffer(d.tokenBuf, d.cfg.ApiToken);
        CopyStringToBuffer(d.projectKeyBuf, d.cfg.ProjectKey);
        CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
        CopyStringToBuffer(d.aiApiKeyBuf, d.cfg.AiApiKey);
        CopyStringToBuffer(d.aiModelBuf, d.cfg.AiModel);
        CopyStringToBuffer(d.aiBaseUrlBuf, d.cfg.AiBaseUrl);
        d.mcpEnabled = d.cfg.McpEnabled;
        d.mcpPort = d.cfg.McpPort;
        d.mcpAllowRemote = d.cfg.McpAllowRemote;
        CopyStringToBuffer(d.mcpAuthTokenBuf, d.cfg.McpAuthToken);
        d.preferencesBuffersLoaded = true;
    }

    if (ImGui::BeginTabBar("PreferencesTabs")) {
        if (ImGui::BeginTabItem("Jira")) {
            ImGui::TextUnformatted("Atlassian Cloud");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::InputText("Domain", d.domainBuf, sizeof(d.domainBuf), ImGuiInputTextFlags_CharsNoBlank);
            ImGui::SetItemTooltip("e.g. companyname.atlassian.net");
            ImGui::InputText("Email", d.emailBuf, sizeof(d.emailBuf));
            ImGui::InputText("API Token", d.tokenBuf, sizeof(d.tokenBuf), ImGuiInputTextFlags_Password);
            ImGui::InputText("Project Key", d.projectKeyBuf, sizeof(d.projectKeyBuf), ImGuiInputTextFlags_CharsUppercase);
            ImGui::SetItemTooltip("Used for create meta enrichment, e.g. PROJ");
            ImGui::Spacing();
            ImGui::InputText("New issue: inherit fields from last row",
                             d.newIssueInheritFieldsBuf,
                             sizeof(d.newIssueInheritFieldsBuf));
            ImGui::SetItemTooltip(
                "Comma-separated Jira field ids copied from the last grid row when you click + New issue "
                "(e.g. description, priority, assignee, labels, components). "
                "Summary is never copied from the last row. "
                "Clear and Save & Sync to restore the built-in default list.");
            ImGui::Spacing();
            ImGui::TextWrapped("JQL and column fields are configured in the Views dashboard.");
            if (ImGui::Button("Open Views Dashboard")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assistant")) {
            ImGui::TextUnformatted("OpenAI-compatible API used by the AI assistant panel.");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::InputText("API Key", d.aiApiKeyBuf, sizeof(d.aiApiKeyBuf), ImGuiInputTextFlags_Password);
            ImGui::InputText("Model", d.aiModelBuf, sizeof(d.aiModelBuf));
            ImGui::SetItemTooltip("Example: gpt-4o-mini");
            ImGui::InputText("Base URL", d.aiBaseUrlBuf, sizeof(d.aiBaseUrlBuf));
            ImGui::SetItemTooltip("Example: https://api.openai.com");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Integrations")) {
            ImGui::TextUnformatted("MCP (Model Context Protocol)");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Checkbox("Enable MCP server", &d.mcpEnabled);
            ImGui::InputInt("MCP Port", &d.mcpPort);
            if (d.mcpPort < 1) {
                d.mcpPort = 1;
            }
            if (d.mcpPort > 65535) {
                d.mcpPort = 65535;
            }
            ImGui::Checkbox("Bind on all interfaces (LAN)", &d.mcpAllowRemote);
            ImGui::SetItemTooltip(
                "When off, MCP listens on localhost only (127.0.0.1). When on, it binds 0.0.0.0 — reachable on your "
                "network. Set an auth token below if you enable this.");
            ImGui::InputText("MCP auth token (optional)", d.mcpAuthTokenBuf, sizeof(d.mcpAuthTokenBuf), ImGuiInputTextFlags_Password);
            ImGui::SetItemTooltip(
                "If set, clients must send header X-Smatchet-Token with this value. If empty and bind is localhost-only, "
                "only loopback clients may connect.");
            ImGui::TextDisabled("Changes apply on next host initialization or restart.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Appearance")) {
            ImGui::TextUnformatted("Grid and field text");
            ImGui::Separator();
            if (ImGui::Checkbox("Show tooltips when text overflows", &d.cfg.EnableFieldOverflowTooltips)) {
                ConfigManager::Save(d.cfg);
            }
            ImGui::SetItemTooltip(
                "When a value is truncated to fit the cell, or spans multiple lines, hover to read the full text in a "
                "tooltip.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            static const LogLevel kLogLevels[] = {
                LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};
            LogLevel parsedLevel = Logger::ParseLogLevelString(d.cfg.LogMinLevel, LogLevel::Info);
            int levelComboIndex = 2;
            for (int i = 0; i < 5; ++i) {
                if (kLogLevels[i] == parsedLevel) {
                    levelComboIndex = i;
                    break;
                }
            }
            ImGui::TextUnformatted("Logging");
            ImGui::Separator();
            ImGui::TextUnformatted("Min log level");
            ImGui::SameLine();
            if (ImGui::Combo(
                    "##LogMinLevel",
                    &levelComboIndex,
                    "Trace\0"
                    "Debug\0"
                    "Info\0"
                    "Warn\0"
                    "Error\0"
                    "\0")) {
                d.cfg.LogMinLevel = Logger::LogLevelToString(kLogLevels[levelComboIndex]);
                Logger::Instance().SetMinLevel(kLogLevels[levelComboIndex]);
                ConfigManager::Save(d.cfg);
            }
            bool jiraBodies = d.cfg.LogJiraHttpBodies;
            if (ImGui::Checkbox("Log Jira HTTP bodies (truncated)", &jiraBodies)) {
                d.cfg.LogJiraHttpBodies = jiraBodies;
                Logger::Instance().SetLogJiraHttpBodies(jiraBodies);
                ConfigManager::Save(d.cfg);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Verbose: logs response text (capped per request). May include issue summaries and user-visible "
                    "data.");
            }
            bool logP4Io = d.cfg.LogP4Io;
            if (ImGui::Checkbox("Log Perforce p4 stdout (truncated, Trace level)", &logP4Io)) {
                d.cfg.LogP4Io = logP4Io;
                Logger::Instance().SetLogP4Io(logP4Io);
                ConfigManager::Save(d.cfg);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Requires min level Trace. Logs capped p4 stdout per command; stderr is logged on non-zero exit.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Blame Analysis")) {
            blameAnalysisUi_.DrawBlamePreferencesTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped(
        "Save & Sync writes the Jira, Assistant, and Integrations tabs to disk and refreshes the Jira connection. "
        "Appearance and Diagnostics options save immediately when changed. The Blame Analysis tab has its own Save "
        "settings and Reload settings buttons.");
    ImGui::Spacing();
    if (ImGui::Button("Save & Sync", ImVec2(140.0f, 0.0f))) {
        d.cfg.Domain = d.domainBuf;
        d.cfg.Email = d.emailBuf;
        d.cfg.ApiToken = d.tokenBuf;
        d.cfg.ProjectKey = d.projectKeyBuf;
        {
            std::vector<std::string> parsedInherit = ParseCsv(std::string(d.newIssueInheritFieldsBuf));
            d.cfg.NewIssueInheritFieldIds.clear();
            for (const auto& s : parsedInherit) {
                if (!s.empty() && s != "summary") {
                    d.cfg.NewIssueInheritFieldIds.push_back(s);
                }
            }
            if (d.cfg.NewIssueInheritFieldIds.empty()) {
                d.cfg.NewIssueInheritFieldIds = IssueDraftHelpers::DefaultNewIssueInheritFieldIds();
            }
            CopyStringToBuffer(d.newIssueInheritFieldsBuf, JoinCsv(d.cfg.NewIssueInheritFieldIds));
        }
        d.cfg.AiApiKey = d.aiApiKeyBuf;
        d.cfg.AiModel = d.aiModelBuf;
        d.cfg.AiBaseUrl = d.aiBaseUrlBuf;
        d.cfg.McpEnabled = d.mcpEnabled;
        d.cfg.McpPort = d.mcpPort;
        d.cfg.McpAllowRemote = d.mcpAllowRemote;
        d.cfg.McpAuthToken = d.mcpAuthTokenBuf;

        ConfigManager::Save(d.cfg);
        LOG_INFO("Updated Jira config. Domain='%s', Email='%s'", d.cfg.Domain.c_str(), d.cfg.Email.c_str());
        d.triggerCatalogRefetch = true;
        app.SyncWithBackend(&d.cfg, &ViewState.GetStore());
    }

    ImGui::End();
}

void SmatchetUI::drawViewsDashboardWindow(AppController& app) {
    auto& d = g_ui;
    if (!d.showViewsDashboard) {
        return;
    }
    // With DockSpaceOverViewport, "Views" often shares a tab bar with "Smatchet - Active Project"
    // (drawn later). showViewsDashboard can stay true while the Views tab is hidden behind another;
    // Open Views / menu then appears to do nothing unless we explicitly focus this window.
    const bool bFocusViews = d.requestViewsDashboardFocus;
    if (bFocusViews) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    ImGui::Begin("Views", &d.showViewsDashboard);
    if (bFocusViews) {
        ImGui::SetWindowFocus();
        d.requestViewsDashboardFocus = false;
    }

    ViewsStore& store = ViewState.GetStoreMutable();
    if (store.Views.empty()) {
        ViewState.EnsureLoaded(d.cfg);
    }

    auto loadBuffersFromView = [&](const ViewDefinition& view) {
        std::memset(d.fieldSearchBuf, 0, sizeof(d.fieldSearchBuf));
        CopyStringToBuffer(d.viewNameBuf, view.Name);
        CopyStringToBuffer(d.viewJqlBuf, view.Jql);
        const std::string selectedFieldsCsv = JoinCsv(view.Fields);
        CopyStringToBuffer(d.selectedFieldsBuf, selectedFieldsCsv);
        d.editingColumnOrder = view.ColumnOrder;
        if (d.editingColumnOrder.empty()) {
            d.editingColumnOrder = {"id"};
            for (const auto& fieldId : view.Fields) {
                d.editingColumnOrder.push_back("field:" + fieldId);
            }
        }
        d.selectedColumnOrderIndex = -1;
        d.editingViewId = view.Id;
        d.lastSyncedColumnOrder = view.ColumnOrder;
    };

    const ViewDefinition* activeView = ViewState.GetActiveView();
    if (activeView) {
        if (d.editingViewId != activeView->Id) {
            loadBuffersFromView(*activeView);
        } else if (activeView->ColumnOrder != d.lastSyncedColumnOrder) {
            loadBuffersFromView(*activeView);
        }
    }

    if (activeView) {
            ImGui::Text("Active View");
            if (ImGui::BeginCombo("##ActiveViewCombo", activeView->Name.c_str())) {
                for (const auto& view : store.Views) {
                    const bool isSelected = (view.Id == activeView->Id);
                    if (ImGui::Selectable(view.Name.c_str(), isSelected)) {
                        if (ViewState.Activate(view.Id)) {
                            const ViewDefinition* nowActive = ViewState.GetActiveView();
                            if (nowActive) {
                                d.cfg.JqlQuery = nowActive->Jql;
                                d.cfg.SelectedFields = nowActive->Fields;
                                SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                            }
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const bool disableBackNav = !d.navHistory.CanGoBack();
            if (disableBackNav) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("<")) {
                const NavigationEntry* entry = d.navHistory.GoBack();
                if (entry) {
                    d.cfg.JqlQuery = entry->Jql;
                    CopyStringToBuffer(d.viewJqlBuf, d.cfg.JqlQuery);
                    SyncWithCurrentView(app, d, ViewState.GetStore(), false);
                }
            }
            if (disableBackNav) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Run the previous JQL query from navigation history.");
            }
            ImGui::SameLine();
            const bool disableForwardNav = !d.navHistory.CanGoForward();
            if (disableForwardNav) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(">")) {
                const NavigationEntry* entry = d.navHistory.GoForward();
                if (entry) {
                    d.cfg.JqlQuery = entry->Jql;
                    CopyStringToBuffer(d.viewJqlBuf, d.cfg.JqlQuery);
                    SyncWithCurrentView(app, d, ViewState.GetStore(), false);
                }
            }
            if (disableForwardNav) {
                ImGui::EndDisabled();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Run the next JQL query from navigation history.");
            }

            ImGui::InputText("View Name", d.viewNameBuf, sizeof(d.viewNameBuf));
            ImGui::InputText("JQL", d.viewJqlBuf, sizeof(d.viewJqlBuf));
            ImGui::SetItemTooltip("Atlassian JQL used when fetching issues.");

            ImGui::Spacing();
            if (ImGui::Button("Apply View & Sync")) {
                ViewDefinition updated = *activeView;
                updated.Name = d.viewNameBuf;
                updated.Jql = d.viewJqlBuf;
                updated.Fields = ParseCsv(d.selectedFieldsBuf);
                updated.ColumnOrder = d.editingColumnOrder;
                if (ViewState.UpdateActive(updated)) {
                    d.cfg.JqlQuery = updated.Jql;
                    d.cfg.SelectedFields = updated.Fields;
                    SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Create New View")) {
                ViewDefinition created = *activeView;
                created.Name = std::string(d.viewNameBuf).empty() ? std::string("New View") : std::string(d.viewNameBuf);
                created.Jql = d.viewJqlBuf;
                created.Fields = ParseCsv(d.selectedFieldsBuf);
                created.ColumnOrder = d.editingColumnOrder;
                created.Id.clear();
                ViewState.Create(created);
                const ViewDefinition* nowActive = ViewState.GetActiveView();
                if (nowActive) {
                    loadBuffersFromView(*nowActive);
                }
            }
            ImGui::SameLine();
            const bool disableDeleteView = (store.Views.size() <= 1);
            if (disableDeleteView) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Delete View")) {
                if (ViewState.DeleteActive()) {
                    const ViewDefinition* nowActive = ViewState.GetActiveView();
                    if (nowActive) {
                        loadBuffersFromView(*nowActive);
                        d.cfg.JqlQuery = nowActive->Jql;
                        d.cfg.SelectedFields = nowActive->Fields;
                        SyncWithCurrentView(app, d, ViewState.GetStore(), true);
                    }
                }
            }
            if (disableDeleteView) {
                ImGui::EndDisabled();
            }

            const std::string& catalogError = app.GetJiraFieldCatalogError();
            if (!catalogError.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s", catalogError.c_str());
                ImGui::PopStyleColor();
            }
            if (!d.fieldCatalogWarning.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.25f, 1.0f));
                ImGui::TextWrapped("%s", d.fieldCatalogWarning.c_str());
                ImGui::PopStyleColor();
            }
            if (d.fieldCatalogLoading) {
                ImGui::TextDisabled("Loading available fields...");
            }

            ImGui::Separator();
            ImGui::Text("Field Picker");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint("##ViewFieldSearch", "Search by field id or name", d.fieldSearchBuf, sizeof(d.fieldSearchBuf));

            std::unordered_set<std::string> selectedFieldSet;
            for (const auto& fieldId : ParseCsv(d.selectedFieldsBuf)) {
                selectedFieldSet.insert(fieldId);
            }

            const auto& availableFields = app.GetAvailableJiraFields();
            std::vector<const JiraField*> visibleFields;
            std::vector<const JiraField*> systemFields;
            std::vector<const JiraField*> customFields;
            std::vector<const JiraField*> basicFields;

            auto isBasicFieldId = [](const std::string& id) {
                return id == "summary" ||
                       id == "assignee" ||
                       id == "priority" ||
                       id == "status" ||
                       id == "created" ||
                       id == "updated";
            };

            for (const auto& field : availableFields) {
                if (!ContainsCaseInsensitive(field.Id, d.fieldSearchBuf) &&
                    !ContainsCaseInsensitive(field.Name, d.fieldSearchBuf)) {
                    continue;
                }
                visibleFields.push_back(&field);
                if (field.IsCustom) {
                    customFields.push_back(&field);
                } else if (isBasicFieldId(field.Id)) {
                    basicFields.push_back(&field);
                } else {
                    systemFields.push_back(&field);
                }
            }

            const auto fieldSortLess = [](const JiraField* lhs, const JiraField* rhs) {
                if (!lhs || !rhs) return lhs != nullptr;
                if (lhs->Name != rhs->Name) return lhs->Name < rhs->Name;
                return lhs->Id < rhs->Id;
            };
            std::sort(systemFields.begin(), systemFields.end(), fieldSortLess);
            std::sort(customFields.begin(), customFields.end(), fieldSortLess);

            const auto syncSelectedFieldsBuffer = [&]() {
                const std::vector<std::string> updated = ToSortedVector(selectedFieldSet);
                const std::string csv = JoinCsv(updated);
                CopyStringToBuffer(d.selectedFieldsBuf, csv);
            };

            const bool disableFieldCatalogEditing = d.fieldCatalogLoading;
            if (disableFieldCatalogEditing) {
                ImGui::BeginDisabled();
            }

            ImGui::TextDisabled("Selected: %zu", selectedFieldSet.size());
            ImGui::SameLine();
            ImGui::TextDisabled("Visible: %zu", visibleFields.size());
            ImGui::SameLine();
            if (ImGui::Button("Select All Visible")) {
                for (const JiraField* field : visibleFields) if (field) selectedFieldSet.insert(field->Id);
                syncSelectedFieldsBuffer();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear Visible")) {
                for (const JiraField* field : visibleFields) if (field) selectedFieldSet.erase(field->Id);
                syncSelectedFieldsBuffer();
            }

            const auto renderFieldGroup = [&](const char* groupName, const std::vector<const JiraField*>& fields) {
                if (fields.empty()) return;
                const std::string label = std::string(groupName) + " (" + std::to_string(fields.size()) + ")";
                if (!ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
                for (const JiraField* field : fields) {
                    bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                    const std::string checkboxId = "##ViewField_" + field->Id;
                    if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                        if (checked) selectedFieldSet.insert(field->Id);
                        else selectedFieldSet.erase(field->Id);
                        syncSelectedFieldsBuffer();
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                }
            };

            ImGui::BeginChild("ViewFieldsList", ImVec2(0, 220), true);
            if (availableFields.empty()) {
                ImGui::TextDisabled("No field catalog loaded yet.");
            } else if (visibleFields.empty()) {
                ImGui::TextDisabled("No fields match current search.");
            } else {
                // Basic Fields group: ID (always shown) + core Jira fields.
                {
                    const char* groupName = "Basic Fields";
                    const bool hasVisibleId = ContainsCaseInsensitive("id", d.fieldSearchBuf);
                    if (!basicFields.empty() || hasVisibleId) {
                        const std::size_t count = basicFields.size() + 1; // +1 for ID
                        const std::string label = std::string(groupName) + " (" + std::to_string(count) + ")";
                        if (ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            // ID: always selected, not toggleable.
                            bool idChecked = true;
                            ImGui::Checkbox("##ViewField_id", &idChecked);
                            ImGui::SameLine();
                            ImGui::Text("ID (id, always selected)");

                            auto renderBasicById = [&](const char* fid) {
                                auto it = std::find_if(
                                    basicFields.begin(),
                                    basicFields.end(),
                                    [&](const JiraField* f) { return f && f->Id == fid; });
                                if (it == basicFields.end() || !*it) return;
                                const JiraField* field = *it;
                                bool checked = selectedFieldSet.find(field->Id) != selectedFieldSet.end();
                                const std::string checkboxId = "##ViewField_" + field->Id;
                                if (ImGui::Checkbox(checkboxId.c_str(), &checked)) {
                                    if (checked) selectedFieldSet.insert(field->Id);
                                    else selectedFieldSet.erase(field->Id);
                                    syncSelectedFieldsBuffer();
                                }
                                ImGui::SameLine();
                                ImGui::Text("%s (%s)", field->Name.c_str(), field->Id.c_str());
                            };

                            renderBasicById("summary");
                            renderBasicById("assignee");
                            renderBasicById("priority");
                            renderBasicById("status");
                            renderBasicById("created");
                            renderBasicById("updated");
                        }
                    }
                }

                renderFieldGroup("System Fields", systemFields);
                renderFieldGroup("Custom Fields", customFields);
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::Text("Column Order");
            ImGui::SameLine();
            ImGui::TextDisabled("(right-click a column to remove it from the view)");
            const std::vector<std::string> currentFields = ParseCsv(d.selectedFieldsBuf);
            std::unordered_set<std::string> validKeys = {"id"};
            for (const auto& f : currentFields) {
                validKeys.insert("field:" + f);
            }
            d.editingColumnOrder.erase(
                std::remove_if(d.editingColumnOrder.begin(), d.editingColumnOrder.end(),
                               [&](const std::string& key) { return validKeys.find(key) == validKeys.end(); }),
                d.editingColumnOrder.end());
            for (const auto& key : validKeys) {
                if (std::find(d.editingColumnOrder.begin(), d.editingColumnOrder.end(), key) == d.editingColumnOrder.end()) {
                    d.editingColumnOrder.push_back(key);
                }
            }

            ImGui::BeginChild("ColumnOrderList", ImVec2(0, 120), true);
            for (int i = 0; i < static_cast<int>(d.editingColumnOrder.size()); ++i) {
                const std::string& key = d.editingColumnOrder[static_cast<size_t>(i)];
                const bool selected = (d.selectedColumnOrderIndex == i);
                if (ImGui::Selectable(key.c_str(), selected)) {
                    d.selectedColumnOrderIndex = i;
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (key != "id" && ImGui::MenuItem("Remove column from view")) {
                        d.editingColumnOrder.erase(d.editingColumnOrder.begin() + i);
                        if (d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size())) {
                            d.selectedColumnOrderIndex = static_cast<int>(d.editingColumnOrder.size()) - 1;
                        }
                        if (key.rfind("field:", 0) == 0) {
                            const std::string fieldId = key.substr(6);
                            std::vector<std::string> fields = ParseCsv(d.selectedFieldsBuf);
                            fields.erase(std::remove(fields.begin(), fields.end(), fieldId), fields.end());
                            const std::string csv = JoinCsv(fields);
                            CopyStringToBuffer(d.selectedFieldsBuf, csv);
                            selectedFieldSet.clear();
                            for (const auto& fid : fields) {
                                selectedFieldSet.insert(fid);
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            const bool disableColumnMoveButtons =
                d.selectedColumnOrderIndex < 0 ||
                d.selectedColumnOrderIndex >= static_cast<int>(d.editingColumnOrder.size());
            if (disableColumnMoveButtons) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Move Up") && d.selectedColumnOrderIndex > 0) {
                std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                          d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex - 1)]);
                --d.selectedColumnOrderIndex;
            }
            ImGui::SameLine();
            if (ImGui::Button("Move Down") &&
                d.selectedColumnOrderIndex >= 0 &&
                d.selectedColumnOrderIndex < static_cast<int>(d.editingColumnOrder.size()) - 1) {
                std::swap(d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex)],
                          d.editingColumnOrder[static_cast<size_t>(d.selectedColumnOrderIndex + 1)]);
                ++d.selectedColumnOrderIndex;
            }
            if (disableColumnMoveButtons) {
                ImGui::EndDisabled();
            }

            if (disableFieldCatalogEditing) {
                ImGui::EndDisabled();
            }
        } else {
            ImGui::TextDisabled("No views available.");
        }

    ImGui::End();
}

void SmatchetUI::drawActiveProjectWindow(AppController& app) {
    auto& d = g_ui;
    ImGui::Begin("Smatchet - Active Project");
    const std::string& catalogError = app.GetJiraFieldCatalogError();
    const bool readOnlyMode = !catalogError.empty();

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:header");
        ImGui::Separator();
        const ViewDefinition* activeView = ViewState.GetActiveView();
        if (activeView) {
            ImGui::Text("View: %s", activeView->Name.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Refresh View")) {
                d.cfg.JqlQuery = activeView->Jql;
                d.cfg.SelectedFields = activeView->Fields;
                SyncWithCurrentView(app, d, ViewState.GetStore(), true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Views")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
        } else {
            ImGui::TextDisabled("No active view.");
            ImGui::SameLine();
            if (ImGui::Button("Open Views")) {
                d.showViewsDashboard = true;
                d.requestViewsDashboardFocus = true;
            }
        }

        ImGui::SameLine();
        if (d.cfg.McpEnabled) {
            if (d.cfg.McpAllowRemote) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "● MCP LIVE: %d (LAN)", d.cfg.McpPort);
            } else {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "● MCP LIVE: %d", d.cfg.McpPort);
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.25f, 1.0f), "● MCP DISABLED");
        }
        if (ImGui::IsItemHovered()) {
            if (d.cfg.McpEnabled) {
                ImGui::SetTooltip(
                    "MCP on port %d. %s Auth: %s.",
                    d.cfg.McpPort,
                    d.cfg.McpAllowRemote ? "Bound on all interfaces." : "Localhost only.",
                    d.cfg.McpAuthToken.empty() ? "loopback only (no token)." : "X-Smatchet-Token required.");
            } else {
                ImGui::SetTooltip("MCP server is disabled. Enable it under Settings → Preferences → Integrations.");
            }
        }

        ImGui::Separator();
        if (readOnlyMode) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.25f, 1.0f));
            ImGui::TextWrapped("Read-only mode: %s", catalogError.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Grid edits and quick comment actions are disabled until Jira connectivity is restored.");
            ImGui::Separator();
        }
    }

    const auto ticketsSnap = app.GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsSnap;

    JiraFieldCatalogIndex catalogIndex(app.GetAvailableJiraFields());
    const ViewDefinition* activeViewForGrid = ViewState.GetActiveView();
    const std::vector<TicketGridColumn> columns =
        activeViewForGrid ? TicketGridColumnsBuilder::Build(*activeViewForGrid, catalogIndex)
                   : std::vector<TicketGridColumn>();

    const bool viewChanged = activeViewForGrid && (activeViewForGrid->Id != d.lastGridActiveViewId);
    if (viewChanged && activeViewForGrid) {
        d.lastGridActiveViewId = activeViewForGrid->Id;
    }
    if (!activeViewForGrid) {
        d.lastGridActiveViewId.clear();
    }

    const std::string gridContextSignature = BuildGridContextSignature(activeViewForGrid, d.cfg.JqlQuery);
    const bool gridContextChanged =
        !d.lastGridContextSignature.empty() && gridContextSignature != d.lastGridContextSignature;
    if (gridContextChanged) {
        CancelUnfinishedNewIssueForGridChange(d);
    }
    d.lastGridContextSignature = gridContextSignature;

    const bool gridSortEnvironmentChanged = viewChanged || gridContextChanged;

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:prep");
        if (!d.gridEditError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.gridEditError.c_str());
            ImGui::PopStyleColor();
        } else if (!d.gridEditSuccess.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
            ImGui::TextWrapped("%s", d.gridEditSuccess.c_str());
            ImGui::PopStyleColor();
        }
    }

    std::vector<PendingFieldEdit> pendingEdits;
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_SortMulti |
        ImGuiTableFlags_NoSavedSettings;

    if (!columns.empty() && ImGui::BeginTable("TicketGrid", static_cast<int>(columns.size()), tableFlags)) {
        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.setup");
            for (const auto& column : columns) {
                const float persistedWidth = activeViewForGrid
                    ? (activeViewForGrid->ColumnWidths.count(column.Key) ? activeViewForGrid->ColumnWidths.at(column.Key) : 0.0f)
                    : 0.0f;
                const float defaultWidth = (column.ColumnKind == TicketGridColumn::Kind::Id) ? 90.0f : 180.0f;
                const float width = persistedWidth > 0.0f ? persistedWidth : defaultWidth;
                if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                    ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
                } else {
                    ImGui::TableSetupColumn(column.Label.c_str(), ImGuiTableColumnFlags_WidthFixed, width);
                }
            }
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
            for (int hci = 0; hci < static_cast<int>(columns.size()); ++hci) {
                ImGui::TableSetColumnIndex(hci);
                const TicketGridColumn& hcol = columns[static_cast<size_t>(hci)];
                ImGui::PushID(hci);
                ImGui::TableHeader(hcol.Label.c_str());
                const JiraField* hdrMeta =
                    (hcol.ColumnKind == TicketGridColumn::Kind::Id) ? nullptr : catalogIndex.Find(hcol.FieldId);
                DrawTicketGridHeaderContextMenu(hcol, hdrMeta);
                ImGui::PopID();
            }

            // Apply persisted sort from view when ImGui has no sort yet, or when we just switched view.
            if (activeViewForGrid && !activeViewForGrid->SortSpecs.empty()) {
                ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
                if (specs && (specs->SpecsCount == 0 || gridSortEnvironmentChanged)) {
                    for (size_t i = 0; i < activeViewForGrid->SortSpecs.size(); ++i) {
                        const ViewSortSpec& vs = activeViewForGrid->SortSpecs[i];
                        if (vs.Direction == 0) continue;
                        int colIndex = -1;
                        for (size_t c = 0; c < columns.size(); ++c) {
                            if (columns[c].Key == vs.ColumnKey) { colIndex = static_cast<int>(c); break; }
                        }
                        if (colIndex >= 0) {
                            ImGui::TableSetColumnSortDirection(colIndex, static_cast<ImGuiSortDirection>(vs.Direction), (i > 0));
                        }
                    }
                }
            }
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.sort");
            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (gridSortEnvironmentChanged) {
                d.cachedSortValid = false;
            }
            if (sortSpecs && sortSpecs->SpecsDirty) {
                d.cachedSortValid = false;
                sortSpecs->SpecsDirty = false;
            }
            const std::uint64_t activeTicketsRevision = app.GetActiveTicketsRevision();
            if (activeTicketsRevision != d.cachedSortTicketsRevision) {
                d.cachedSortValid = false;
            }
            const std::uint64_t catalogRevision = app.GetJiraFieldCatalogRevision();
            if (catalogRevision != d.cachedSortCatalogRevision) {
                d.cachedSortValid = false;
            }

            if (sortSpecs && sortSpecs->SpecsCount > 0 && sortSpecs->Specs != nullptr) {
                std::string fingerprint;
                fingerprint.reserve(static_cast<size_t>(sortSpecs->SpecsCount) * 48);
                for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
                    fingerprint += std::to_string(spec.ColumnIndex);
                    fingerprint.push_back(':');
                    fingerprint += std::to_string(static_cast<int>(spec.SortDirection));
                    fingerprint.push_back(':');
                    fingerprint += std::to_string(static_cast<int>(spec.SortOrder));
                    fingerprint.push_back('|');
                }

                if (!d.cachedSortValid ||
                    d.cachedSortFingerprint != fingerprint ||
                    d.cachedSortedIndices.size() != tickets.size()) {
                    d.cachedSortedIndices.resize(tickets.size());
                    for (size_t i = 0; i < tickets.size(); ++i) d.cachedSortedIndices[i] = i;
                    const std::vector<CachedTicket>* ticketsPtr = &tickets;
                    const std::vector<TicketGridColumn>* columnsPtr = &columns;
                    const JiraFieldCatalogIndex* catalogPtr = &catalogIndex;
                    std::stable_sort(
                        d.cachedSortedIndices.begin(),
                        d.cachedSortedIndices.end(),
                        [ticketsPtr, columnsPtr, catalogPtr, sortSpecs](size_t ia, size_t ib) {
                            const auto& tix = *ticketsPtr;
                            const auto& cols = *columnsPtr;
                            for (int s = 0; s < sortSpecs->SpecsCount; ++s) {
                                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[s];
                                const int colIndex = spec.ColumnIndex;
                                if (colIndex < 0 || colIndex >= static_cast<int>(cols.size())) continue;
                                const TicketGridColumn& col = cols[static_cast<size_t>(colIndex)];
                                const int dir = (spec.SortDirection == ImGuiSortDirection_Ascending) ? 1 : -1;
                                if (col.ColumnKind == TicketGridColumn::Kind::Id) {
                                    const bool less = CompareIssueKeyNatural(tix[ia].id, tix[ib].id);
                                    if (less) return dir > 0;
                                    if (!CompareIssueKeyNatural(tix[ib].id, tix[ia].id)) continue;
                                    return dir < 0;
                                }
                                const std::string aVal = tix[ia].GetFieldValue(col.FieldId);
                                const std::string bVal = tix[ib].GetFieldValue(col.FieldId);
                                const JiraField* fieldMeta = catalogPtr->Find(col.FieldId);
                                const int cmp = CompareFieldValuesForSort(
                                    col.FieldId,
                                    fieldMeta,
                                    aVal,
                                    bVal,
                                    spec.SortDirection);
                                if (cmp != 0) return (cmp * dir) < 0;
                            }
                            return ia < ib;
                        });
                    d.cachedSortFingerprint = std::move(fingerprint);
                    d.cachedSortValid = true;
                    d.cachedSortTicketsRevision = activeTicketsRevision;
                    d.cachedSortCatalogRevision = catalogRevision;
                }
            } else {
                d.cachedSortedIndices.clear();
                d.cachedSortFingerprint.clear();
                d.cachedSortValid = false;
                d.cachedSortTicketsRevision = activeTicketsRevision;
                d.cachedSortCatalogRevision = catalogRevision;
            }
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.rows");
            const std::vector<size_t>* indicesToUse =
                d.cachedSortedIndices.empty() ? nullptr : &d.cachedSortedIndices;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(tickets.size()));
            while (clipper.Step()) {
                for (int clippedRow = clipper.DisplayStart; clippedRow < clipper.DisplayEnd; ++clippedRow) {
                    const size_t r = static_cast<size_t>(clippedRow);
                    const size_t ticketIndex = indicesToUse ? (*indicesToUse)[r] : r;
                    const CachedTicket& ticket = tickets[ticketIndex];
                    bool isSelected = (d.gridState.SelectedId == ticket.id);
                    if (isSelected && !readOnlyMode) {
                        app.WarmIssueEditMetaAsync(ticket.id);
                    }
                    thread_local char rowPerfLabel[288];
                    const char* rowPerfScopeName = nullptr;
                    if (isSelected) {
                        std::snprintf(rowPerfLabel,
                                      sizeof(rowPerfLabel),
                                      "activeProject:row[%zu] %s",
                                      r,
                                      ticket.id.c_str());
                        rowPerfLabel[sizeof(rowPerfLabel) - 1] = '\0';
                        rowPerfScopeName = rowPerfLabel;
                    }
                    SMATCHET_UI_PERF_SCOPE(rowPerfScopeName);
                    ImGui::TableNextRow();

                    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
                        const auto& column = columns[static_cast<size_t>(colIndex)];
                        ImGui::TableSetColumnIndex(colIndex);

                        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
                            ImGui::BeginGroup();
                            if (ImGui::Selectable(ticket.id.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                                d.gridState.SelectRow(ticket.id);
                                if (ImGui::IsMouseDoubleClicked(0)) {
                                    const std::string url = BuildJiraBrowseUrl(d.cfg, ticket.id);
                                    app.OpenUrl(url);
                                }
                            }
                            ImGui::EndGroup();
                            DrawGridCellRightClickPopups(
                                BuildCellKey(ticket.id, "id"),
                                ticket.id,
                                std::string(),
                                column.Label,
                                ticket.id,
                                &app,
                                &d,
                                readOnlyMode);
                            continue;
                        }

                        const std::string currentValue = ticket.GetFieldValue(column.FieldId);
                        const JiraField* fieldMeta = catalogIndex.Find(column.FieldId);
                        const float cellStartY = ImGui::GetCursorScreenPos().y;
                        const float cellRightX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                        const float valueAvailWidth = cellRightX - ImGui::GetCursorScreenPos().x;
                        const std::string cellKey = BuildCellKey(ticket.id, column.FieldId);
                        const auto feedbackIt = d.cellFeedbackByKey.find(cellKey);
                        const bool isSavingThisCell =
                            feedbackIt != d.cellFeedbackByKey.end() &&
                            feedbackIt->second.State == CellWriteState::Saving;

                        bool showBadge = false;
                        ImVec4 badgeColor(1.0f, 1.0f, 1.0f, 1.0f);
                        std::string badgeText;
                        std::string badgeTooltip;
                        if (feedbackIt != d.cellFeedbackByKey.end() &&
                            feedbackIt->second.State == CellWriteState::Error &&
                            !feedbackIt->second.Message.empty()) {
                            showBadge = true;
                            badgeColor = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                            badgeText = "!";
                            badgeTooltip = feedbackIt->second.Message;
                        } else if (feedbackIt != d.cellFeedbackByKey.end() &&
                                   feedbackIt->second.State == CellWriteState::Success) {
                            showBadge = true;
                            badgeColor = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
                            badgeText = "OK";
                            badgeTooltip = "Saved";
                        }

                        if (isSavingThisCell) {
                            showBadge = true;
                            badgeColor = ImVec4(0.95f, 0.75f, 0.35f, 1.0f);
                            badgeText = "...";
                            badgeTooltip = "Saving...";
                            ImGui::BeginGroup();
                            const std::string saveDisplay =
                                DisplayValueForJiraDateField(column.FieldId, fieldMeta, currentValue);
                            const std::string* saveTip =
                                IsJiraDateOrDateTimeField(column.FieldId, fieldMeta) ? &currentValue : nullptr;
                            RenderClippedFieldText(
                                saveDisplay, valueAvailWidth, d.cfg.EnableFieldOverflowTooltips, true, saveTip);
                            ImGui::EndGroup();
                            DrawGridCellRightClickPopups(
                                cellKey, ticket.id, column.FieldId, column.Label, currentValue, nullptr, nullptr, readOnlyMode);
                        } else {
                            const bool allowEditsForCell =
                                !readOnlyMode &&
                                column.NeedsAllowEditsCheck &&
                                app.CanEditJiraFieldForIssue(ticket.id, column.FieldId, fieldMeta);
                            ImGui::BeginGroup();
                            TicketFieldEditor::RenderFieldCell(
                                app,
                                ticket,
                                column,
                                fieldMeta,
                                currentValue,
                                valueAvailWidth,
                                d.cfg.EnableFieldOverflowTooltips,
                                allowEditsForCell,
                                d.gridState,
                                pendingEdits,
                                d.jiraGridAsync);
                            ImGui::EndGroup();
                            DrawGridCellRightClickPopups(
                                cellKey, ticket.id, column.FieldId, column.Label, currentValue, nullptr, nullptr, readOnlyMode);
                        }

                        if (showBadge) {
                            const ImVec2 textSize = ImGui::CalcTextSize(badgeText.c_str());
                            const float badgeX = (std::max)(ImGui::GetCursorScreenPos().x, cellRightX - textSize.x);
                            ImGui::SetCursorScreenPos(ImVec2(badgeX, cellStartY));
                            ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
                            ImGui::TextUnformatted(badgeText.c_str());
                            ImGui::PopStyleColor();
                            if (!badgeTooltip.empty() && ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("%s", badgeTooltip.c_str());
                            }
                        }
                    }
                }
            }
        }

        if (!readOnlyMode) {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.newIssue");
            RenderNewIssueDraftRow(app, d, columns, d.cfg);
        }

        {
            SMATCHET_UI_PERF_SCOPE("activeProject:grid.post");
            RouteVerticalWheelToHorizontalAtTableVerticalEnds(ImGui::GetCurrentTable());

            // Persist column widths and sort specs back to the active view.
            if (activeViewForGrid) {
                ViewDefinition* mutableActive = ViewState.GetActiveViewMutable();
                if (mutableActive) {
                    bool metaChanged = false;
                    ImGuiTable* table = ImGui::GetCurrentTable();
                    if (table) {
                        // Persist column widths only; column order is controlled via the Views dashboard.
                        for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
                            const std::string& key = columns[static_cast<size_t>(i)].Key;
                            const float width = (i < table->ColumnsCount)
                                ? table->Columns[i].WidthGiven
                                : 0.0f;
                            const auto oldIt = mutableActive->ColumnWidths.find(key);
                            const float oldWidth = (oldIt == mutableActive->ColumnWidths.end()) ? 0.0f : oldIt->second;
                            if (std::abs(oldWidth - width) > 0.5f) {
                                mutableActive->ColumnWidths[key] = width;
                                metaChanged = true;
                            }
                        }
                    }
                    // Re-fetch sort specs from the table right before persisting so we use current state.
                    ImGuiTableSortSpecs* currentSortSpecs = ImGui::TableGetSortSpecs();
                    if (currentSortSpecs && currentSortSpecs->SpecsCount > 0 && currentSortSpecs->Specs != nullptr) {
                        std::vector<ViewSortSpec> newSortSpecs;
                        for (int s = 0; s < currentSortSpecs->SpecsCount; ++s) {
                            const int colIndex = currentSortSpecs->Specs[s].ColumnIndex;
                            if (colIndex >= 0 && colIndex < static_cast<int>(columns.size()) &&
                                currentSortSpecs->Specs[s].SortDirection != ImGuiSortDirection_None) {
                                ViewSortSpec vs;
                                vs.ColumnKey = columns[static_cast<size_t>(colIndex)].Key;
                                vs.Direction = static_cast<int>(currentSortSpecs->Specs[s].SortDirection);
                                newSortSpecs.push_back(vs);
                            }
                        }
                        if (newSortSpecs != mutableActive->SortSpecs) {
                            mutableActive->SortSpecs = std::move(newSortSpecs);
                            metaChanged = true;
                        }
                    } else {
                        // No active sort in the table; clear stored sort so it doesn't persist stale state.
                        if (!mutableActive->SortSpecs.empty()) {
                            mutableActive->SortSpecs.clear();
                            metaChanged = true;
                        }
                    }
                    if (metaChanged) {
                        const auto now = std::chrono::steady_clock::now();
                        const auto deadline = now + kViewStateSaveDebounce;
                        d.pendingViewStateSave = true;
                        d.pendingViewStateSaveAt = std::max(d.pendingViewStateSaveAt, deadline);
                    }
                }
            }
        }
        ImGui::EndTable();
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:edits.queue");
        // Keep queued edits latest-per-cell (drop older queued item for same cell).
        if (!readOnlyMode) {
            for (const auto& edit : pendingEdits) {
                const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
                for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
                    if (BuildCellKey(it->IssueId, it->Field.Id) == editKey) {
                        it = d.queuedFieldEdits.erase(it);
                    } else {
                        ++it;
                    }
                }
                d.queuedFieldEdits.push_back(edit);
            }
        } else if (!pendingEdits.empty()) {
            d.gridEditSuccess.clear();
            d.gridEditError = "Edit skipped: Jira is in read-only mode.";
        }

        if (readOnlyMode) {
            d.queuedFieldEdits.clear();
        }
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:edits.inFlight");
        if (!readOnlyMode && !d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
            d.inFlightEdit = d.queuedFieldEdits.front();
            d.queuedFieldEdits.pop_front();
            d.inFlightOriginalEstimateSnapshot.clear();
            d.inFlightRemainingEstimateSnapshot.clear();
            d.inFlightIssueTypeKeySnapshot.clear();
            const auto snapshotIt = std::find_if(
                tickets.begin(),
                tickets.end(),
                [&](const CachedTicket& ticket) { return ticket.id == d.inFlightEdit.IssueId; });
            if (snapshotIt != tickets.end()) {
                d.inFlightOriginalEstimateSnapshot = snapshotIt->GetFieldValue("timeoriginalestimate");
                d.inFlightRemainingEstimateSnapshot = snapshotIt->GetFieldValue("timeestimate");
                d.inFlightIssueTypeKeySnapshot =
                    ToLowerAsciiCopy(TrimCopy(snapshotIt->GetFieldValue("issuetype")));
            }
            d.hasInFlightEdit = true;
            d.inFlightCommitStarted = false;
            d.inFlightDelayFrames = 1;
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Saving;
            feedback.Message = "Saving to Jira...";
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id)] = feedback;
        }

        if (d.hasInFlightEdit) {
            if (d.inFlightDelayFrames > 0) {
                --d.inFlightDelayFrames;
            } else if (!d.inFlightCommitStarted) {
                const PendingFieldEdit edit = d.inFlightEdit;
                const std::string originalEstimateSnapshot = d.inFlightOriginalEstimateSnapshot;
                const std::string remainingEstimateSnapshot = d.inFlightRemainingEstimateSnapshot;
                const std::string issueTypeKeySnapshot = d.inFlightIssueTypeKeySnapshot;
                d.inFlightCommitFuture = std::async(std::launch::async,
                                                    [&app,
                                                     edit,
                                                     originalEstimateSnapshot,
                                                     remainingEstimateSnapshot,
                                                     issueTypeKeySnapshot]() {
                    FieldEditCommitResult result;
                    result.Ok = app.SubmitJiraFieldEditNetworkOnly(edit.IssueId,
                                                                   edit.Field,
                                                                   edit.Values,
                                                                   originalEstimateSnapshot,
                                                                   remainingEstimateSnapshot,
                                                                   issueTypeKeySnapshot,
                                                                   result.ApplyResult);
                    result.Error = result.ApplyResult.Error;
                    return result;
                });
                d.inFlightCommitStarted = true;
            } else {
                const std::string editKey = BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id);
                if (d.inFlightCommitFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                    // Async Jira commit still running; keep UI responsive.
                } else {
                    const FieldEditCommitResult result = d.inFlightCommitFuture.get();
                    std::string applyError;
                    const bool applied = result.Ok &&
                        app.ApplyJiraFieldEditResult(d.inFlightEdit.IssueId, result.ApplyResult, applyError);
                    if (!result.Ok || !applied) {
                        d.gridEditError = (!result.Ok ? result.Error : applyError).empty()
                        ? std::string("Failed to save Jira field update.")
                        : (!result.Ok ? result.Error : applyError);
                        d.gridEditSuccess.clear();
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Error;
                        feedback.Message = d.gridEditError;
                        feedback.FramesRemaining = 0;
                        d.cellFeedbackByKey[editKey] = feedback;
                    } else {
                        d.gridEditSuccess = "Field update saved to Jira.";
                        d.gridEditError.clear();
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Success;
                        feedback.Message = "Saved";
                        feedback.FramesRemaining = 180;
                        d.cellFeedbackByKey[editKey] = feedback;
                    }
                    d.hasInFlightEdit = false;
                    d.inFlightCommitStarted = false;
                }
            }
        }
    }

    {
        SMATCHET_UI_PERF_SCOPE("activeProject:cellFeedback");
        for (auto it = d.cellFeedbackByKey.begin(); it != d.cellFeedbackByKey.end();) {
            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining > 0) {
                --it->second.FramesRemaining;
            }

            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining <= 0) {
                it = d.cellFeedbackByKey.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!pendingEdits.empty()) {
        d.gridEditError.clear();
    }
    ImGui::End();
}

void SmatchetUI::drawAIAssistantWindow(AppController& app) {
    auto& d = g_ui;
    ImGui::Begin("AI Assistant");

    if (d.gridState.SelectedId.empty()) {
        ImGui::TextDisabled("Select a ticket to see AI insights.");
    } else {
        // Find the selected ticket data in the app's cache
        const auto ticketsSnapAi = app.GetActiveTicketsSnapshot();
        const auto& tickets = *ticketsSnapAi;
        auto it = std::find_if(tickets.begin(), tickets.end(),
                               [&](const CachedTicket& t){ return t.id == d.gridState.SelectedId; });

        if (it != tickets.end()) {
            const std::string summary = it->GetFieldValue("summary");
            ImGui::Text("Analyzing: %s", it->id.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", summary.empty() ? "<no summary field selected>" : summary.c_str());
            if (!it->fieldValues.empty()) {
                ImGui::Spacing();
                ImGui::Text("Selected Jira Fields");
                ImGui::Separator();
                JiraFieldCatalogIndex aiCatalog(app.GetAvailableJiraFields());
                for (const auto& kv : it->fieldValues) {
                    const JiraField* f = aiCatalog.Find(kv.first);
                    const std::string display = DisplayValueForJiraDateField(kv.first, f, kv.second);
                    const std::string* tip =
                        IsJiraDateOrDateTimeField(kv.first, f) ? &kv.second : nullptr;
                    const std::string label = kv.first + ": ";
                    ImGui::TextUnformatted(label.c_str());
                    ImGui::SameLine();
                    const float valueAvail = ImGui::GetContentRegionAvail().x;
                    RenderClippedFieldText(display, valueAvail, d.cfg.EnableFieldOverflowTooltips, false, tip);
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Model: %s", d.cfg.AiModel.empty() ? "(unset)" : d.cfg.AiModel.c_str());
            ImGui::TextDisabled("Endpoint: %s", d.cfg.AiBaseUrl.empty() ? "(unset)" : d.cfg.AiBaseUrl.c_str());
            if (d.cfg.AiApiKey.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                                   "Set AI API Key under Settings → Preferences → Assistant to enable generation.");
            }

            if (ImGui::Button("Generate Action Plan") && !d.aiIsThinking) {
                d.aiIsThinking = true;
                auto result = AiController::AnalyzeTicket(
                    it->id,
                    summary,
                    d.cfg.AiApiKey,
                    d.cfg.AiModel,
                    d.cfg.AiBaseUrl);
                d.aiResponse = result.Response;
                d.aiIsThinking = false;
            }

            if (d.aiIsThinking) ImGui::Text("AI is thinking...");

            if (!d.aiResponse.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 0.7f, 1.0f));
                ImGui::TextWrapped("%s", d.aiResponse.c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
}

void SmatchetUI::drawLogWindow() {
    auto& d = g_ui;
    Logger& logger = Logger::Instance();
    ImGui::Begin("Log");

    if (ImGui::Button("Clear Log")) {
        logger.Clear();
        d.lastSeenLogRevision = logger.GetRevision();
        d.logBuffer.assign(1, '\0');
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(application log)");
    ImGui::SameLine();
    ImGui::TextDisabled("Log level and verbose options: Settings → Preferences → Diagnostics.");

    ImGui::Separator();

    const std::uint64_t revision = logger.GetRevision();
    const bool rebuildLogBuffer = d.logBuffer.empty() || revision != d.lastSeenLogRevision;
    if (rebuildLogBuffer) {
        const auto entries = logger.GetEntriesSnapshot();
        std::string aggregated;
        aggregated.reserve(entries.size() * 64);

        for (const auto& e : entries) {
            const char* levelLabel;
            switch (e.level) {
                case LogLevel::Trace: levelLabel = "[TRACE] "; break;
                case LogLevel::Debug: levelLabel = "[DEBUG] "; break;
                case LogLevel::Info:  levelLabel = "[INFO ] "; break;
                case LogLevel::Warn:  levelLabel = "[WARN ] "; break;
                case LogLevel::Error: levelLabel = "[ERROR] "; break;
                default:              levelLabel = ""; break;
            }
            aggregated += levelLabel;
            aggregated += e.message;
            aggregated += '\n';
        }

        d.logBuffer.assign(aggregated.begin(), aggregated.end());
        d.logBuffer.push_back('\0');
        d.lastSeenLogRevision = revision;
    }

    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InputTextMultiline(
        "##LogText",
        d.logBuffer.data(),
        d.logBuffer.size(),
        ImVec2(-FLT_MIN, -FLT_MIN),
        ImGuiInputTextFlags_ReadOnly
    );
    ImGui::EndChild();
    ImGui::End();
}

namespace {

IssueTableSerializer::Format BulkFormatFromIndex(int idx) {
    switch (idx) {
        case 1: return IssueTableSerializer::Format::Csv;
        case 2: return IssueTableSerializer::Format::Tsv;
        case 3: return IssueTableSerializer::Format::Json;
        default: return IssueTableSerializer::Format::Auto;
    }
}

int BulkImportTextResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* buf = static_cast<std::vector<char>*>(data->UserData);
        buf->resize(static_cast<size_t>(data->BufTextLen) + 1);
        data->Buf = buf->data();
    }
    return 0;
}

bool ReadEntireFile(const std::string& path, std::string& outText, std::string& outError) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        outError = "Failed to open file: " + path;
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    outText = ss.str();
    return true;
}

bool WriteEntireFile(const std::string& path, const std::string& text, std::string& outError) {
    std::ofstream f(path, std::ios::binary);
    if (!f.good()) {
        outError = "Failed to open file for write: " + path;
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

} // anonymous namespace

void SmatchetUI::drawBulkImportWindow(AppController& app) {
    auto& d = g_ui;
    if (!d.showBulkImport) {
        if (d.bulkImportWasOpen) {
            d.bulkImportTextBuf.clear();
            d.bulkImportPathBuf[0] = '\0';
            d.bulkImportFormatSel = 0;
            d.bulkImportPreview = {};
            d.bulkImportStatus.clear();
            d.bulkImportFutures.clear();
            d.bulkImportNextSubmit = 0;
            d.bulkImportCompleted = 0;
            d.bulkImportRunning = false;
            d.bulkImportError.clear();
            d.bulkImportWasOpen = false;
        }
        return;
    }
    d.bulkImportWasOpen = true;

    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bulk import tickets", &d.showBulkImport)) {
        ImGui::End();
        return;
    }

    if (d.bulkImportTextBuf.empty()) d.bulkImportTextBuf.assign(1, '\0');

    ImGui::TextUnformatted("Source:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(520);
    ImGui::InputText("##bulkImportPath", d.bulkImportPathBuf, sizeof(d.bulkImportPathBuf));
    ImGui::SameLine();
    if (ImGui::Button("Load file")) {
        std::string text, err;
        if (ReadEntireFile(d.bulkImportPathBuf, text, err)) {
            d.bulkImportTextBuf.assign(text.begin(), text.end());
            d.bulkImportTextBuf.push_back('\0');
            d.bulkImportError.clear();
        } else {
            d.bulkImportError = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Paste clipboard")) {
        const char* clip = ImGui::GetClipboardText();
        if (clip && *clip) {
            const std::string s(clip);
            d.bulkImportTextBuf.assign(s.begin(), s.end());
            d.bulkImportTextBuf.push_back('\0');
            d.bulkImportError.clear();
        } else {
            d.bulkImportError = "Clipboard is empty.";
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const char* kFormats[] = { "Auto", "CSV", "TSV", "JSON" };
    ImGui::Combo("##bulkImportFmt", &d.bulkImportFormatSel, kFormats, IM_ARRAYSIZE(kFormats));
    ImGui::SameLine();
    if (ImGui::Button("Parse preview")) {
        const std::string text(d.bulkImportTextBuf.data());
        const IssueTableSerializer::Format fmt = BulkFormatFromIndex(d.bulkImportFormatSel);
        d.bulkImportPreview = IssueTableSerializer::ParseDrafts(
            text, fmt, app.GetAvailableJiraFields(),
            d.cfg.ProjectKey, d.cfg.DefaultIssueTypeId, d.cfg.DefaultIssueTypeName);
        d.bulkImportStatus.assign(d.bulkImportPreview.Rows.size(), std::string());
        d.bulkImportError = d.bulkImportPreview.Error;
        d.bulkImportNextSubmit = 0;
        d.bulkImportCompleted = 0;
        d.bulkImportRunning = false;
        d.bulkImportFutures.clear();
        d.bulkImportFutures.resize(d.bulkImportPreview.Rows.size());
    }

    if (!d.bulkImportError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.bulkImportError.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Paste / edit source text here (headers = field ids or display names):");
    ImGui::InputTextMultiline(
        "##bulkImportText",
        d.bulkImportTextBuf.data(),
        d.bulkImportTextBuf.size(),
        ImVec2(-FLT_MIN, 160.0f),
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
        &BulkImportTextResizeCallback,
        &d.bulkImportTextBuf);

    ImGui::Separator();
    ImGui::Text("Parsed rows: %zu", d.bulkImportPreview.Rows.size());
    ImGui::SameLine();
    const int maxConcurrent = (std::max)(1, d.cfg.ImportMaxConcurrent);
    ImGui::Text("| Max concurrent: %d", maxConcurrent);
    ImGui::SameLine();
    const bool canRun = !d.bulkImportPreview.Rows.empty() && !d.bulkImportRunning;
    if (!canRun) ImGui::BeginDisabled();
    if (ImGui::Button("Submit all")) {
        d.bulkImportRunning = true;
        d.bulkImportNextSubmit = 0;
        d.bulkImportCompleted = 0;
        d.bulkImportStatus.assign(d.bulkImportPreview.Rows.size(), "queued");
        d.bulkImportFutures.clear();
        d.bulkImportFutures.resize(d.bulkImportPreview.Rows.size());
    }
    if (!canRun) ImGui::EndDisabled();

    // Pump submissions + completions each frame.
    if (d.bulkImportRunning) {
        // Submit up to maxConcurrent in-flight.
        size_t inFlight = 0;
        for (size_t i = 0; i < d.bulkImportFutures.size(); ++i) {
            if (d.bulkImportFutures[i].valid()) ++inFlight;
        }
        while (d.bulkImportNextSubmit < d.bulkImportPreview.Rows.size() &&
               inFlight < static_cast<size_t>(maxConcurrent)) {
            const size_t idx = d.bulkImportNextSubmit++;
            const auto& row = d.bulkImportPreview.Rows[idx];
            if (!row.Error.empty()) {
                d.bulkImportStatus[idx] = "parse error: " + row.Error;
                ++d.bulkImportCompleted;
                continue;
            }
            d.bulkImportFutures[idx] = app.CreateIssueAsync(row.Draft);
            d.bulkImportStatus[idx] = "submitting...";
            ++inFlight;
        }
        // Reap.
        for (size_t i = 0; i < d.bulkImportFutures.size(); ++i) {
            auto& fut = d.bulkImportFutures[i];
            if (!fut.valid()) continue;
            if (fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
            IssueCreateResult r = fut.get();
            if (r.Ok) {
                d.bulkImportStatus[i] = "ok " + r.IssueKey;
            } else {
                std::string msg = r.Error.empty() ? "failed" : r.Error;
                if (!r.MissingFieldIds.empty()) {
                    msg += " [missing: ";
                    for (size_t k = 0; k < r.MissingFieldIds.size(); ++k) {
                        if (k) msg += ',';
                        msg += r.MissingFieldIds[k];
                    }
                    msg += "]";
                }
                d.bulkImportStatus[i] = msg;
            }
            ++d.bulkImportCompleted;
        }
        if (d.bulkImportCompleted >= d.bulkImportPreview.Rows.size()) {
            d.bulkImportRunning = false;
        }
    }

    ImGui::SameLine();
    ImGui::Text("Completed %zu / %zu", d.bulkImportCompleted, d.bulkImportPreview.Rows.size());

    {
        std::vector<std::string> keysNeedingHydration;
        for (const auto& row : d.bulkImportPreview.Rows) {
            const std::string& ek = row.Draft.ExistingIssueKey;
            if (ek.empty()) {
                continue;
            }
            keysNeedingHydration.push_back(ek);
        }
        app.PrefetchIssueTicketsForKeys(keysNeedingHydration);
    }

    auto ticketsSnap = app.GetActiveTicketsSnapshot();

    if (ImGui::BeginTable("bulkImportPreview", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(-FLT_MIN, 260.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Changes", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < d.bulkImportPreview.Rows.size(); ++i) {
            const auto& row = d.bulkImportPreview.Rows[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", row.SourceLine);

            ImGui::TableSetColumnIndex(1);
            if (!row.Draft.ExistingIssueKey.empty()) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "update %s",
                                   row.Draft.ExistingIssueKey.c_str());
            } else {
                ImGui::Text("create %s / %s", row.Draft.ProjectKey.c_str(),
                            row.Draft.IssueTypeName.empty() ? row.Draft.IssueTypeId.c_str()
                                                            : row.Draft.IssueTypeName.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            const auto sumIt = row.Draft.FieldValues.find("summary");
            ImGui::TextUnformatted(sumIt != row.Draft.FieldValues.end() ? sumIt->second.c_str() : "");

            ImGui::TableSetColumnIndex(3);
            if (!row.Draft.ExistingIssueKey.empty()) {
                const CachedTicket* existing = nullptr;
                if (ticketsSnap) {
                    for (const auto& t : *ticketsSnap) {
                        if (t.id == row.Draft.ExistingIssueKey) {
                            existing = &t;
                            break;
                        }
                    }
                }
                if (existing) {
                    const auto changes = IssueDraftHelpers::ComputeFieldChanges(row.Draft, *existing);
                    if (changes.empty()) {
                        ImGui::TextDisabled("no changes");
                    } else {
                        ImGui::Text("%zu field(s)", changes.size());
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
                            for (const auto& c : changes) {
                                ImGui::TextColored(ImVec4(0.8f, 0.9f, 1.0f, 1.0f), "%s",
                                                   c.FieldId.c_str());
                                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.6f, 1.0f), "- %s",
                                                   c.OldValue.empty() ? "(empty)" : c.OldValue.c_str());
                                ImGui::TextColored(ImVec4(0.6f, 1.0f, 0.6f, 1.0f), "+ %s",
                                                   c.NewValue.empty() ? "(empty)" : c.NewValue.c_str());
                                ImGui::Separator();
                            }
                            ImGui::PopTextWrapPos();
                            ImGui::EndTooltip();
                        }
                    }
                } else if (app.IsBulkImportPrefetchInFlight(row.Draft.ExistingIssueKey)) {
                    ImGui::TextDisabled("loading…");
                } else {
                    ImGui::TextDisabled("not in cache");
                }
            } else {
                ImGui::TextDisabled("new");
            }

            ImGui::TableSetColumnIndex(4);
            const char* status = (i < d.bulkImportStatus.size()) ? d.bulkImportStatus[i].c_str() : "";
            if (!row.Error.empty() && (i >= d.bulkImportStatus.size() || d.bulkImportStatus[i].empty())) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", row.Error.c_str());
            } else {
                ImGui::TextUnformatted(status);
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

void SmatchetUI::drawBulkExportWindow(AppController& app) {
    auto& d = g_ui;
    if (!d.showBulkExport) return;

    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bulk export tickets", &d.showBulkExport)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Destination path (for Save):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(420);
    ImGui::InputText("##bulkExportPath", d.bulkExportPathBuf, sizeof(d.bulkExportPathBuf));
    ImGui::SameLine();
    const char* kFormats[] = { "CSV", "TSV", "JSON" };
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("##bulkExportFmt", &d.bulkExportFormatSel, kFormats, IM_ARRAYSIZE(kFormats));

    ImGui::Checkbox("Only selected row", &d.bulkExportOnlySelected);

    auto buildText = [&]() -> std::string {
        auto snap = app.GetActiveTicketsSnapshot();
        std::vector<CachedTicket> tickets;
        if (d.bulkExportOnlySelected && !d.gridState.SelectedId.empty() && snap) {
            for (const auto& t : *snap) {
                if (t.id == d.gridState.SelectedId) { tickets.push_back(t); break; }
            }
        } else if (snap) {
            tickets = *snap;
        }
        IssueTableSerializer::Format fmt = IssueTableSerializer::Format::Csv;
        if (d.bulkExportFormatSel == 1) fmt = IssueTableSerializer::Format::Tsv;
        else if (d.bulkExportFormatSel == 2) fmt = IssueTableSerializer::Format::Json;
        return IssueTableSerializer::SerializeTickets(tickets, {}, fmt);
    };

    if (ImGui::Button("Copy to clipboard")) {
        const std::string text = buildText();
        ImGui::SetClipboardText(text.c_str());
        d.bulkExportFeedback = "Copied " + std::to_string(text.size()) + " bytes.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to file")) {
        const std::string text = buildText();
        std::string err;
        if (WriteEntireFile(d.bulkExportPathBuf, text, err)) {
            d.bulkExportFeedback = "Wrote " + std::to_string(text.size()) + " bytes.";
        } else {
            d.bulkExportFeedback = err;
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", d.bulkExportFeedback.c_str());

    ImGui::Separator();
    auto snap = app.GetActiveTicketsSnapshot();
    const size_t total = snap ? snap->size() : 0;
    ImGui::Text("Active tickets in view: %zu", total);
    ImGui::TextDisabled("Exports the current view's rows. Headers are field ids; import can read them back.");

    ImGui::End();
}
