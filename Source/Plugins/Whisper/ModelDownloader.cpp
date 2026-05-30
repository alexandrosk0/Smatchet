// ModelDownloader — see header for design pointers. Pattern A worker:
// `Start` dispatches via AppController::LaunchBackgroundTask, the worker
// streams bytes to disk through a cpr::WriteCallback (cancel atom polled
// between chunks), then re-opens the completed partial for a streaming
// SHA-256 pass before renaming onto the final path.
//
// Resume support — write set is `<dest>/<id>.bin.partial`. On a fresh
// invocation we stat that file; if it exists with non-zero size we send
// `Range: bytes=<size>-` and append. SHA-256 is computed over the whole
// completed file regardless of how many resume legs it took.

#include "ModelDownloader.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "ModelCatalog.h"
#include "WhisperConsentGate.h"

#include <cpr/cpr.h>
#include <ghc/filesystem.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#endif

namespace smatchet {
namespace whisper {

namespace {

#if defined(_WIN32)
// Streaming SHA-256 via Win32 BCrypt. Returns lowercase hex on success;
// empty string on failure. Streams the file in 64 KiB chunks so a 500 MB
// model does not balloon RSS during verification.
std::string ComputeSha256HexBcryptStreaming(const std::string& path) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (s != 0 || hAlg == nullptr) {
        return std::string();
    }
    struct AlgScope {
        BCRYPT_ALG_HANDLE h;
        ~AlgScope() {
            if (h != nullptr) {
                ::BCryptCloseAlgorithmProvider(h, 0);
            }
        }
    } algScope{hAlg};

    DWORD objLen = 0;
    DWORD copied = 0;
    s = ::BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &copied, 0);
    if (s != 0 || objLen == 0) {
        return std::string();
    }
    std::vector<unsigned char> obj(objLen);
    BCRYPT_HASH_HANDLE hHash = nullptr;
    s = ::BCryptCreateHash(hAlg, &hHash, obj.data(), objLen, nullptr, 0, 0);
    if (s != 0 || hHash == nullptr) {
        return std::string();
    }
    struct HashScope {
        BCRYPT_HASH_HANDLE h;
        ~HashScope() {
            if (h != nullptr) {
                ::BCryptDestroyHash(h);
            }
        }
    } hashScope{hHash};

    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return std::string();
    }
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) {
            break;
        }
        s = ::BCryptHashData(hHash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(got), 0);
        if (s != 0) {
            return std::string();
        }
    }

    unsigned char digest[32];
    s = ::BCryptFinishHash(hHash, digest, sizeof(digest), 0);
    if (s != 0) {
        return std::string();
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (std::size_t i = 0; i < 32; ++i) {
        out[2 * i + 0] = kHex[(digest[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}
#else
// Non-Win32 placeholder — the Whisper plugin is gated to Windows builds
// (Source/Plugins/Whisper/CMakeLists.txt's WASAPI link list is `if(WIN32)`), but
// keep the symbol so a future cross-platform port has one fewer thing to
// stub. Returns empty so verification always fails.
std::string ComputeSha256HexBcryptStreaming(const std::string& /*path*/) {
    return std::string();
}
#endif

std::string JoinPath(const std::string& dir, const std::string& name) {
    std::string out = dir;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') {
        out.push_back('/');
    }
    out += name;
    return out;
}

bool EnsureDirectoryExists(const std::string& dir, std::string& outError) {
    if (dir.empty()) {
        outError = "model destination directory is empty";
        return false;
    }
    std::error_code ec;
    ghc::filesystem::create_directories(dir, ec);
    if (ec) {
        outError = "failed to create model directory: " + dir + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

} // namespace

struct ModelDownloader::Impl {
    mutable std::mutex mu;
    Progress progress;
    std::shared_ptr<std::atomic<bool>> cancelAtom;
    std::atomic<bool> running{false};
};

ModelDownloader::ModelDownloader() : impl_(std::make_shared<Impl>()) {
    impl_->cancelAtom = std::make_shared<std::atomic<bool>>(false);
}

ModelDownloader::~ModelDownloader() {
    // Cooperative cancel; the worker may still be alive in
    // AppController's thread pool. Flag flip is harmless — the cpr
    // WriteCallback observes it between chunks.
    if (impl_ && impl_->cancelAtom) {
        impl_->cancelAtom->store(true, std::memory_order_release);
    }
}

ModelDownloader::Progress ModelDownloader::GetProgress() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->progress;
}

bool ModelDownloader::IsRunning() const {
    return impl_->running.load(std::memory_order_acquire);
}

void ModelDownloader::Cancel() {
    if (impl_ && impl_->cancelAtom) {
        impl_->cancelAtom->store(true, std::memory_order_release);
        LOG_INFO("ModelDownloader::Cancel requested.");
    }
}

bool ModelDownloader::Start(AppController& app, const std::string& modelId, const std::string& destDir,
                            std::string& outError) {
    outError.clear();
    if (modelId.empty()) {
        outError = "model id is empty";
        return false;
    }
    const smatchet::whisper::catalog::Entry* entry = smatchet::whisper::catalog::Find(modelId);
    if (entry == nullptr) {
        outError = "unknown model id: " + modelId;
        return false;
    }
    if (impl_->running.load(std::memory_order_acquire)) {
        outError = "model download is already running";
        return false;
    }
    std::string ensureErr;
    if (!EnsureDirectoryExists(destDir, ensureErr)) {
        outError = ensureErr;
        return false;
    }

    // Consent gate — refuse unless the user clicked "Download + enable"
    // (or the equivalent Preferences button) within the freshness window.
    // The 30 s default matches the packet's "fresh consent" definition.
    {
        const TrackerConfig cfg = ConfigManager::Load();
        const std::int64_t now = smatchet::whisper::consent::NowEpochSec();
        if (!smatchet::whisper::consent::CanDownloadModel(cfg, now, 30)) {
            outError = "consent required: click 'Download + enable' in the Whisper banner or "
                       "Preferences first (stale consent timestamp)";
            return false;
        }
    }

    // Refresh progress snapshot under the lock.
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->progress = Progress();
        impl_->progress.state = State::Downloading;
        impl_->progress.modelId = modelId;
        impl_->progress.bytesExpected = entry->SizeBytes; // pre-headers estimate
    }
    impl_->cancelAtom = std::make_shared<std::atomic<bool>>(false);
    impl_->running.store(true, std::memory_order_release);

    const std::string url = entry->Url;
    const std::string finalPath = JoinPath(destDir, entry->Id + ".bin");
    const std::string partialPath = finalPath + ".partial";
    const std::string expectedSha = entry->Sha256;
    std::shared_ptr<std::atomic<bool>> cancelAtom = impl_->cancelAtom;
    // Co-own Impl with the worker so destructor of ModelDownloader does not
    // invalidate mu/progress/running while the detached task is still running.
    std::shared_ptr<Impl> implPtr = impl_;

    app.LaunchBackgroundTask([implPtr, url, finalPath, partialPath, expectedSha, modelId, cancelAtom]() {
        auto setProgress = [&implPtr](State st, std::uint64_t got, std::uint64_t expected,
                                      const std::string& errMsg) {
            std::lock_guard<std::mutex> lock(implPtr->mu);
            implPtr->progress.state = st;
            if (got != 0) {
                implPtr->progress.bytesReceived = got;
            }
            if (expected != 0) {
                implPtr->progress.bytesExpected = expected;
            }
            implPtr->progress.error = errMsg;
        };
        auto finalize = [&implPtr](State st, const std::string& errMsg) {
            std::lock_guard<std::mutex> lock(implPtr->mu);
            implPtr->progress.state = st;
            implPtr->progress.error = errMsg;
        };

        // Resume: if `<final>.partial` exists with non-zero size, append
        // via Range: bytes=<size>-. Else start from scratch.
        std::uint64_t resumeFrom = 0;
        {
            std::error_code ec;
            const bool exists = ghc::filesystem::exists(partialPath, ec);
            if (!ec && exists) {
                const auto sz = ghc::filesystem::file_size(partialPath, ec);
                if (!ec) {
                    resumeFrom = static_cast<std::uint64_t>(sz);
                }
            }
        }

        std::ios_base::openmode mode = std::ios::binary;
        mode |= (resumeFrom > 0) ? std::ios::app : std::ios::trunc;
        std::ofstream ofs(partialPath, mode);
        if (!ofs.is_open()) {
            finalize(State::Failed, "failed to open partial file for write: " + partialPath);
            implPtr->running.store(false, std::memory_order_release);
            return;
        }

        std::uint64_t bytesWritten = resumeFrom;
        setProgress(State::Downloading, bytesWritten, 0, std::string());

        cpr::Header headers{{"Accept", "application/octet-stream"},
                            {"User-Agent", std::string("SmatchetWhisper/") + modelId}};
        if (resumeFrom > 0) {
            headers["Range"] = "bytes=" + std::to_string(resumeFrom) + "-";
        }
        cpr::Redirect redirect(true, true);
        bool cancelled = false;

        cpr::WriteCallback writeCb{[&, implPtr](const std::string& data, intptr_t /*sz*/) -> bool {
            if (cancelAtom && cancelAtom->load(std::memory_order_acquire)) {
                cancelled = true;
                return false;
            }
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!ofs.good()) {
                return false;
            }
            bytesWritten += data.size();
            // Cheap snapshot — UI polls each frame, no need to lock per byte.
            // Lock here to keep the snapshot consistent with `error` updates.
            std::lock_guard<std::mutex> lock(implPtr->mu);
            implPtr->progress.bytesReceived = bytesWritten;
            return true;
        }};

        cpr::Response resp = cpr::Get(cpr::Url{url},
                                      headers,
                                      redirect,
                                      writeCb,
                                      cpr::ConnectTimeout{10000},
                                      cpr::Timeout{0}); // disable total-timeout; large models > 2 min
        ofs.close();

        if (cancelled) {
            finalize(State::Cancelled, "download cancelled");
            implPtr->running.store(false, std::memory_order_release);
            LOG_INFO("ModelDownloader: cancelled at %llu bytes", static_cast<unsigned long long>(bytesWritten));
            return;
        }
        if (resp.error.code != cpr::ErrorCode::OK ||
            (resp.status_code != 200 && resp.status_code != 206 && resp.status_code != 0)) {
            std::string err = "HTTP fetch failed";
            if (!resp.error.message.empty()) {
                err += ": " + resp.error.message;
            } else if (resp.status_code > 0) {
                err += " (HTTP " + std::to_string(resp.status_code) + ")";
            }
            finalize(State::Failed, err);
            implPtr->running.store(false, std::memory_order_release);
            return;
        }

        // Verification pass.
        setProgress(State::Verifying, bytesWritten, 0, std::string());
        const std::string actualSha = ComputeSha256HexBcryptStreaming(partialPath);
        if (actualSha.empty()) {
            finalize(State::Failed, "SHA-256 hashing failed (BCrypt unavailable or read error)");
            implPtr->running.store(false, std::memory_order_release);
            return;
        }
        if (actualSha != expectedSha) {
            // Hash mismatch — delete the partial so a future Start() does
            // not append onto corrupt bytes.
            std::error_code rmEc;
            ghc::filesystem::remove(partialPath, rmEc);
            finalize(State::Failed,
                     std::string("SHA-256 mismatch (expected ") + expectedSha + ", got " + actualSha + ")");
            implPtr->running.store(false, std::memory_order_release);
            return;
        }

        // Atomic rename onto the final path. ghc::filesystem::rename
        // matches std::filesystem semantics — overwrites on Windows when
        // the target file exists.
        std::error_code mvEc;
        ghc::filesystem::rename(partialPath, finalPath, mvEc);
        if (mvEc) {
            finalize(State::Failed, "rename to final path failed: " + mvEc.message());
            implPtr->running.store(false, std::memory_order_release);
            return;
        }

        finalize(State::Complete, std::string());
        implPtr->running.store(false, std::memory_order_release);
        LOG_INFO("ModelDownloader: model %s ready at %s (%llu bytes)", modelId.c_str(), finalPath.c_str(),
                 static_cast<unsigned long long>(bytesWritten));
    });

    return true;
}

} // namespace whisper
} // namespace smatchet
