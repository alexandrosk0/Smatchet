#pragma once

// ModelDownloader — Pattern A worker that streams a ggml whisper model from
// the huggingface mirror into `<userData>/whisper/<id>.bin`, with resumable
// `.partial` staging + post-completion SHA-256 verification.
// See docs/plans/shipped/whisper-dictation.md § Phase C / ModelDownloader.
// Consent contract — Start() refuses unless WhisperConsentGate::CanDownloadModel
// returns true (WhisperEnabled + fresh consent timestamp). Callers should
// stamp `cfg.WhisperConsentTimestampSec = NowEpochSec()` and save the config
// before calling Start; the banner + Preferences download buttons do so.
// Threading — Start() spawns a single background thread via
// IAppThreading::LaunchBackgroundTask; UI-thread callers poll `GetProgress()`
// each frame for the progress bar. Cancel() flips the shared cancel atom
// between chunks. The worker writes to `<dest>.partial` and renames onto
// `<dest>` only after SHA-256 verification passes — a mid-flight crash
// leaves the partial file in place and the next Start() call resumes via
// the HTTP Range header.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class IAppThreading; // narrow AppController threading facet — see Commands/IAppThreading.h

namespace smatchet {
namespace whisper {

class ModelDownloader {
  public:
    enum class State {
        Idle,        ///< No download has been started, or Cancel() ran to completion.
        Downloading, ///< HTTP fetch in progress; `bytesReceived` advances.
        Verifying,   ///< Bytes complete; SHA-256 hashing the partial file.
        Complete,    ///< Verification passed; file renamed to its final path.
        Failed,      ///< Aborted with an error; see `Progress::error`.
        Cancelled    ///< User cancelled via Cancel(); partial preserved for resume.
    };

    struct Progress {
        State state = State::Idle;
        /// Bytes already written to the on-disk `.partial` file.
        std::uint64_t bytesReceived = 0;
        /// Total bytes the worker expects to receive once the response
        /// headers arrive. 0 until Content-Length is known (or if the
        /// server omitted it — rare on the huggingface mirror).
        std::uint64_t bytesExpected = 0;
        /// Last error string when state is Failed; empty otherwise.
        std::string error;
        /// Catalog id being fetched ("ggml-tiny.en" etc.). Empty when Idle.
        std::string modelId;
    };

    ModelDownloader();
    ~ModelDownloader();

    ModelDownloader(const ModelDownloader&) = delete;
    ModelDownloader& operator=(const ModelDownloader&) = delete;

    /// Starts a background fetch for `modelId` into `destDir` (e.g.
    /// `<userData>/whisper`). Refuses on:
    ///   - empty / unknown `modelId` (not in ModelCatalog),
    ///   - already-running fetch (use Cancel() first),
    ///   - failed consent check against the live TrackerConfig (caller is
    ///     responsible for stamping WhisperConsentTimestampSec; see
    ///     header preamble).
    /// Returns true when the worker was successfully dispatched; the
    /// caller polls GetProgress() each frame to drive the UI. Returns
    /// false on validation / consent failure, with the reason in
    /// `outError` (also surfaced through Progress::error after Start).
    bool Start(IAppThreading& app, const std::string& modelId, const std::string& destDir, std::string& outError);

    /// Thread-safe snapshot of the worker's current state. Cheap; the UI
    /// polls every ~1 frame for the progress bar.
    Progress GetProgress() const;

    /// Best-effort cancel. Idempotent; safe to call when no download is
    /// running. Returns immediately — the worker observes the flag
    /// between HTTP chunks and exits within ~100 ms typically.
    void Cancel();

    /// Returns true when a worker thread is currently active (state ==
    /// Downloading or Verifying). Used by Preferences to disable the
    /// "Download" button while a fetch is in flight.
    bool IsRunning() const;

  private:
    struct Impl;

    // Worker-thread body for Start(). Streams the HTTP download to the
    // `.partial` file, verifies SHA-256, and renames onto the final path.
    // Co-owns `impl` so it outlives the ModelDownloader. Free of `this` —
    // static so the dispatched task never dereferences a destroyed owner.
    static void RunDownloadWorker(std::shared_ptr<Impl> impl, const std::string& url, const std::string& finalPath,
                                  const std::string& partialPath, const std::string& expectedSha,
                                  const std::string& modelId, std::shared_ptr<std::atomic<bool>> cancelAtom);

    // Post-download verify + rename phase: SHA-256 the completed `.partial`,
    // compare against `expectedSha`, then atomically rename onto `finalPath`.
    // Updates progress + the running flag through `impl`. Split out of
    // RunDownloadWorker to keep both halves under the size cap.
    static void FinalizeDownload(std::shared_ptr<Impl> impl, const std::string& finalPath,
                                 const std::string& partialPath, const std::string& expectedSha,
                                 const std::string& modelId, std::uint64_t bytesWritten);
    // shared_ptr so the detached worker dispatched by Start() can co-own Impl
    // until the task exits. The destructor only flips the cancel atom; the
    // worker captures its own shared_ptr<Impl> and tears down on its own
    // thread once cancellation is observed between HTTP chunks.
    std::shared_ptr<Impl> impl_;
};

} // namespace whisper
} // namespace smatchet
