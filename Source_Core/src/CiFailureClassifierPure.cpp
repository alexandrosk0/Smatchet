// CiFailureClassifierPure.cpp — phase-5 of the `coderabbit-react-loop`
// plan. Pure C++14 helpers backing the (phase-6) concrete
// `CiFailureClassifier` and the abstract `PrCheckRunClassifier` interface
// (Source_Core/include/PrCheckRunClassifier.h).
//
// No third-party / banned-dep includes — this TU compiles into both
// SmatchetStandalone (OpenGL) and SmatchetCore_DX12 (Unreal) and the
// doctest rig links it directly. No `#if defined(SMATCHET_WITH_AGENTIC)`
// gate — the helpers are pure stdlib + portable (mirrors
// CoderabbitCommentClassifierPure.cpp).

#include "CiFailureClassifierPure.h"

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace agentic {
namespace ci_pure {

namespace {

// Known check-run names — exact strings as authored in the workflow YAML
// files (case-sensitive). New variants must be added here explicitly;
// unknown names fall through to `CheckRunCategory::Unknown` so the
// watcher LOG_WARNs rather than silently mis-classifying.
//
// Sources:
//   - .github/workflows/build-and-test.yml  → 3 jobs (full / no-whisper /
//                                             no-agentic), all category
//                                             CmakeOrCtest
//   - .github/workflows/coverage-gate.yml   → 1 job, CoverageGate
//   - .github/workflows/coverage.yml        → 1 job, CoverageAdvisory
//                                             (continue-on-error until
//                                             2026-05-30)
struct CheckRunNameRow {
    const char* name;
    CheckRunCategory category;
};

const CheckRunNameRow kCheckRunNameTable[] = {
    {"Windows + MSYS2 UCRT64", CheckRunCategory::CmakeOrCtest},
    {"Windows + MSYS2 UCRT64 (SMATCHET_WITH_WHISPER=OFF)", CheckRunCategory::CmakeOrCtest},
    {"Windows + MSYS2 UCRT64 (SMATCHET_WITH_AGENTIC=OFF)", CheckRunCategory::CmakeOrCtest},
    {"Test-delta gate", CheckRunCategory::CoverageGate},
    {"Coverage (windows-2022 + OpenCppCoverage)", CheckRunCategory::CoverageAdvisory},
};

bool ContainsSubstring(const std::string& haystack, const char* needle) {
    if (haystack.empty() || needle == nullptr) {
        return false;
    }
    return haystack.find(needle) != std::string::npos;
}

bool ContainsAny(const std::string& text, const char* const* needles, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (ContainsSubstring(text, needles[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

CheckRunCategory MapCheckRunNameToCategory(const std::string& checkRunName) {
    if (checkRunName.empty()) {
        return CheckRunCategory::Unknown;
    }
    for (const CheckRunNameRow& row : kCheckRunNameTable) {
        if (checkRunName == row.name) {
            return row.category;
        }
    }
    return CheckRunCategory::Unknown;
}

bool MatchesCmakeConfigError(const std::string& text) {
    static const char* const kNeedles[] = {
        "CMake Error",
        "CMake Configure Step Failed",
        "target_link_libraries called with",
        "FetchContent_Declare(",
        " not found by find_package",
    };
    return ContainsAny(text, kNeedles, sizeof(kNeedles) / sizeof(kNeedles[0]));
}

bool MatchesLinkError(const std::string& text) {
    static const char* const kNeedles[] = {
        "undefined reference to", "multiple definition of", "ld: error:", "lld-link: error:", "LNK2019", "LNK2001",
    };
    return ContainsAny(text, kNeedles, sizeof(kNeedles) / sizeof(kNeedles[0]));
}

bool MatchesCtestFailure(const std::string& text) {
    static const char* const kNeedles[] = {
        "FAILED:",
        "The following tests FAILED:",
        "Errors while running CTest",
        "doctest::String",
        "FAILED:  smatchet",
    };
    return ContainsAny(text, kNeedles, sizeof(kNeedles) / sizeof(kNeedles[0]));
}

bool MatchesSanitizerHit(const std::string& text) {
    static const char* const kNeedles[] = {
        "AddressSanitizer:",        "UndefinedBehaviorSanitizer:", "ThreadSanitizer:",
        "MemorySanitizer:",         "LeakSanitizer:",              "==ERROR: ",
    };
    return ContainsAny(text, kNeedles, sizeof(kNeedles) / sizeof(kNeedles[0]));
}

bool MatchesTransientFlake(const std::string& text) {
    // Single-substring fingerprints — any of these alone is sufficient
    // evidence to treat as transient.
    static const char* const kNeedles[] = {
        "Operation timed out",
        "Could not resolve host: mirror",
        "could not connect to host",
        "Temporary failure resolving",
        "curl: (28)",
        "FetchContent: download failed",
    };
    if (ContainsAny(text, kNeedles, sizeof(kNeedles) / sizeof(kNeedles[0]))) {
        return true;
    }
    // Paired fingerprint: a connection-reset error against an MSYS2 mirror
    // URL. Either substring alone is too generic to claim transient — they
    // must both appear.
    if (ContainsSubstring(text, "http://mirror.msys2.org") && ContainsSubstring(text, "connection reset")) {
        return true;
    }
    return false;
}

CmakeOrCtestKind FingerprintCmakeOrCtest(const std::string& annotationsConcatenated, const std::string& logTail) {
    // Priority order: Transient first (a flake masquerading as a real
    // build / link / test error must not be re-classified as the real
    // failure), then SanitizerHit, CtestFailure, LinkError, CmakeConfigError.
    // First match wins across both the annotations and the log tail.
    const std::string& a = annotationsConcatenated;
    const std::string& l = logTail;

    if (MatchesTransientFlake(a) || MatchesTransientFlake(l)) {
        return CmakeOrCtestKind::TransientFlake;
    }
    if (MatchesSanitizerHit(a) || MatchesSanitizerHit(l)) {
        return CmakeOrCtestKind::SanitizerHit;
    }
    if (MatchesCtestFailure(a) || MatchesCtestFailure(l)) {
        return CmakeOrCtestKind::CtestFailure;
    }
    if (MatchesLinkError(a) || MatchesLinkError(l)) {
        return CmakeOrCtestKind::LinkError;
    }
    if (MatchesCmakeConfigError(a) || MatchesCmakeConfigError(l)) {
        return CmakeOrCtestKind::CmakeConfigError;
    }
    return CmakeOrCtestKind::Unknown;
}

std::string ConcatenateAnnotations(const std::vector<std::string>& annotationMessages, std::size_t maxLen) {
    if (annotationMessages.empty()) {
        return std::string();
    }
    // Reserve a sensible upper bound to dodge re-allocs in the common path.
    std::size_t reserve = 0;
    for (const std::string& msg : annotationMessages) {
        reserve += msg.size() + 1; // +1 for the joining newline
    }
    std::string out;
    out.reserve(reserve);
    for (std::size_t i = 0; i < annotationMessages.size(); ++i) {
        if (i > 0) {
            out.push_back('\n');
        }
        out.append(annotationMessages[i]);
    }
    // Cap the result from the **start** — the tail is where the actual
    // error usually sits (final FAILED line, sanitizer footer, etc).
    if (out.size() > maxLen) {
        out.erase(0, out.size() - maxLen);
    }
    return out;
}

} // namespace ci_pure
} // namespace agentic
} // namespace smatchet
