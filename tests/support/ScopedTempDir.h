#pragma once
// ScopedTempDir — RAII private directory under the system temp dir for tests that touch real files.
// The directory (and everything in it) is removed on destruction; failures to remove are ignored so a
// failing REQUIRE (which throws) never masks the real failure with a cleanup error.
//
// Header-only, C++14; needs ghc_filesystem (already linked by every test target that touches files).

#include <ghc/filesystem.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <system_error>

namespace smatchet_tests {

class ScopedTempDir {
  public:
    explicit ScopedTempDir(const char* prefix = "smatchet_test_dir_") {
        static std::atomic<int> counter{0};
        const long long stamp = static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::error_code ec;
        const ghc::filesystem::path base = ghc::filesystem::temp_directory_path(ec);
        path_ = ((ec ? ghc::filesystem::path(".") : base) /
                 (std::string(prefix) + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1))))
                    .string();
        ghc::filesystem::create_directories(path_, ec);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        ghc::filesystem::remove_all(path_, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::string& Path() const { return path_; }
    /// `Path()/name` as a string.
    std::string File(const std::string& name) const { return (ghc::filesystem::path(path_) / name).string(); }

  private:
    std::string path_;
};

} // namespace smatchet_tests
