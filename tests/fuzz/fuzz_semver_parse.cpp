// libFuzzer driver for the update-check version parser —
// smatchet::version::ParseSemanticVersion (SemanticVersionPure.cpp). The raw string is a GitHub
// release tag drawn straight from an update-check HTTP response (untrusted network ingress), e.g.
// "v1.2.3" or a hostile "v99999999999999999999.0.0". The parser must terminate and never overflow:
// std::atoi on an unbounded digit run past INT_MAX is UB, so each segment is all-digit-gated then
// range-checked via std::stoll (CPP_CODE_AUDIT.md #17). The fuzz bytes are the raw tag; a hang
// surfaces as a libFuzzer timeout, integer UB as a UBSan abort. nightly fuzz-smoke Layer 3.
#include "SemanticVersionPure.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string tag(reinterpret_cast<const char*>(data), size);
    const smatchet::version::SemanticVersion parsed = smatchet::version::ParseSemanticVersion(tag);
    // Exercise the compare path against a fixed baseline so both functions stay covered.
    static const smatchet::version::SemanticVersion baseline = {1, 0, 0, true};
    (void)smatchet::version::CompareSemanticVersion(parsed, baseline);
    return 0;
}
