// libFuzzer driver for smatchet::image_dim::ParseImageDimensionsFromBytes —
// the PNG/GIF/WEBP/JPEG header dimension parser that runs over untrusted
// attachment bytes (the in-app image preview path). Slice E2, testing-surface.md
// §6 P1 Gap 3 / security.md §59-60.
//
// libFuzzer drives this via its own main() (linked by -fsanitize=fuzzer). The
// parser is total — it must never crash, read OOB, or trigger UB on ANY byte
// string; ASan+UBSan (also in the fuzzer sanitizer set) catch violations.
#include "Ui/ImageDimensionsPure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::vector<unsigned char> bytes(data, data + size);
    // mimeType only steers the final degradation message, not the byte parsing;
    // a fixed value keeps the input space focused on the header-decode paths.
    (void)smatchet::image_dim::ParseImageDimensionsFromBytes(bytes, "image/png");
    return 0;
}
