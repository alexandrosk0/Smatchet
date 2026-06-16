// libFuzzer driver for AiNdjsonParser::Feed — parses untrusted newline-delimited
// JSON byte streams from the Ollama /api/chat endpoint. Slice E2b,
// testing-surface.md §6 P1 Gap 3 / security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). libcurl
// hands the parser arbitrarily-split byte chunks (TCP boundaries, never line
// boundaries), so the driver split-feeds each input in two chunks at a
// data-derived offset to fuzz buffer reassembly, then Flush()es to exercise the
// EOF / trailing-line path. Each completed line is parsed as JSON (nlohmann); the
// parser must never crash, read OOB, or trigger UB on ANY byte string — including
// malformed JSON, which surfaces via onError rather than throwing. ASan+UBSan (in
// the fuzzer sanitizer set) catch violations.
#include "AiNdjsonParser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    AiNdjsonParser parser;
    const char* p = reinterpret_cast<const char*>(data);
    const size_t split = size ? (data[0] % (size + 1)) : 0;
    auto onLine = [](const auto&) {};
    auto onError = [](const auto&) {};
    parser.Feed(p, split, onLine, onError);
    parser.Feed(p + split, size - split, onLine, onError);
    parser.Flush(onLine, onError);
    return 0;
}
