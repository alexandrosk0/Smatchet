// libFuzzer driver for MarkdownConvert::MarkdownToAdf — converts untrusted
// Markdown (Jira/Plane issue descriptions + comments the user pastes/edits) into
// the ADF JSON document model via md4c. Slice E2b, testing-surface.md §6 P1
// Gap 3 / security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). The
// converter walks md4c's callback events and builds an nlohmann::json tree; it
// must never crash, read OOB, or trigger UB on ANY byte string. ASan+UBSan (in
// the fuzzer sanitizer set) catch violations. The md4c parser itself is also
// exercised transitively.
#include "Ui/MarkdownConvert.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string md(reinterpret_cast<const char*>(data), size);
    (void)MarkdownConvert::MarkdownToAdf(md);
    return 0;
}
