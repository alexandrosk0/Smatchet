// libFuzzer driver for ParseCallstackText — parses untrusted callstack text
// (pasted by the user, or extracted from a Jira/Plane comment) into structured
// frames for the annotate symbolication path. Slice E2b, testing-surface.md §6
// P1 Gap 3 / security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). The
// parser uses std::regex over the input; it must never crash, read OOB, or
// trigger UB on ANY byte string (incl. overflowing line numbers and pathological
// regex inputs). ASan+UBSan (in the fuzzer sanitizer set) catch violations.
#include "CallstackParser.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    (void)ParseCallstackText(text);
    return 0;
}
