#include "SubprocessCapture.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifndef SMATCHET_TEST_HELPER_DIR
#error "SMATCHET_TEST_HELPER_DIR must be defined via target_compile_definitions"
#endif

namespace {

#ifdef _WIN32
const char* kExeSuffix = ".exe";
#else
const char* kExeSuffix = "";
#endif

std::string HelperPath(const char* basename) {
    std::string p = SMATCHET_TEST_HELPER_DIR;
    p.push_back('/');
    p.append(basename);
    p.append(kExeSuffix);
    return p;
}

} // namespace

TEST_SUITE("SubprocessCapture::Run") {

    TEST_CASE("exit code 0 is captured") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_exit");
        opts.args.push_back("0");
        SubprocessCapture::CaptureResult out;
        std::string err;
        REQUIRE(SubprocessCapture::Run(opts, out, err));
        CHECK(err.empty());
        CHECK(out.exitCode == 0);
        CHECK_FALSE(out.timedOut);
        CHECK_FALSE(out.cancelled);
    }

    TEST_CASE("non-zero exit code is captured") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_exit");
        opts.args.push_back("7");
        SubprocessCapture::CaptureResult out;
        std::string err;
        REQUIRE(SubprocessCapture::Run(opts, out, err));
        CHECK(out.exitCode == 7);
    }

    TEST_CASE("sleep within timeout succeeds") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_sleep");
        opts.args.push_back("50");
        opts.timeoutMs = 5000;
        SubprocessCapture::CaptureResult out;
        std::string err;
        REQUIRE(SubprocessCapture::Run(opts, out, err));
        CHECK(out.exitCode == 0);
        CHECK_FALSE(out.timedOut);
        CHECK(out.durationMs >= 40);
    }

    TEST_CASE("sleep beyond timeout flips timedOut and terminates promptly") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_sleep");
        opts.args.push_back("5000");
        opts.timeoutMs = 200;
        SubprocessCapture::CaptureResult out;
        std::string err;
        REQUIRE(SubprocessCapture::Run(opts, out, err));
        CHECK(out.timedOut);
        // Allow generous slack on overburdened CI runners; the killed
        // child should still be reaped well under the original 5 s
        // sleep budget.
        CHECK(out.durationMs < 2000);
        CHECK_FALSE(out.stderrText.empty());
    }

    TEST_CASE("stdout byte cap truncates oversize output") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_flood");
        opts.args.push_back("100000");
        opts.stdoutByteCap = 1024;
        opts.timeoutMs = 10000;
        SubprocessCapture::CaptureResult out;
        std::string err;
        REQUIRE(SubprocessCapture::Run(opts, out, err));
        CHECK(out.exitCode == 0);
        CHECK(out.stdoutCapped);
        // Cap byte budget (1024) was applied; the truncation suffix
        // is appended after the cap, so the final size is >= 1024
        // and bounded by 1024 + suffix length.
        CHECK(out.stdoutText.size() >= 1024);
        CHECK(out.stdoutText.size() < 1024 + 64);
    }

    TEST_CASE("cancel token interrupts sleeping child") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = HelperPath("subproc_helper_sleep");
        opts.args.push_back("5000");
        opts.timeoutMs = 10000;
        opts.cancelToken = std::make_shared<std::atomic<bool>>(false);
        auto token = opts.cancelToken;
        std::thread flipper([token]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            token->store(true);
        });
        SubprocessCapture::CaptureResult out;
        std::string err;
        const bool ran = SubprocessCapture::Run(opts, out, err);
        flipper.join();
        REQUIRE(ran);
        CHECK(out.cancelled);
        CHECK(out.durationMs < 2000);
    }

    TEST_CASE("spawn failure surfaces in outError") {
        SubprocessCapture::CaptureOptions opts;
        opts.argv0 = "smatchet_definitely_nonexistent_binary_xyz_h1";
        SubprocessCapture::CaptureResult out;
        std::string err;
        const bool ran = SubprocessCapture::Run(opts, out, err);
        // On Windows, CreateProcessW fails up-front for a missing exe;
        // on POSIX, the parent's `Run()` succeeds at fork() and the
        // child's execvp() fails → exit 127. Either contract is
        // acceptable so long as the failure is observable in the
        // CaptureResult (non-zero exit on POSIX, ran=false on Win32).
        if (ran) {
            CHECK(out.exitCode == 127);
        } else {
            CHECK_FALSE(err.empty());
        }
    }
}
