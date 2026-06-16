// libFuzzer driver for AiSseParser::Feed — parses untrusted Server-Sent-Events
// byte streams from AI providers (OpenAI / Anthropic chat completions). Slice
// E2b, testing-surface.md §6 P1 Gap 3 / security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). libcurl
// hands the parser arbitrarily-split byte chunks (TCP boundaries, never SSE frame
// boundaries), so the driver split-feeds each input in two chunks at a
// data-derived offset to fuzz buffer reassembly, then Flush()es to exercise the
// EOF / trailing-frame path. The parser must never crash, read OOB, or trigger UB
// on ANY byte string. ASan+UBSan (in the fuzzer sanitizer set) catch violations.
#include "AiSseParser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    AiSseParser parser;
    const char* p = reinterpret_cast<const char*>(data);
    const size_t split = size ? (data[0] % (size + 1)) : 0;
    auto sink = [](const auto&) {};
    parser.Feed(p, split, sink);
    parser.Feed(p + split, size - split, sink);
    parser.Flush(sink);
    return 0;
}
