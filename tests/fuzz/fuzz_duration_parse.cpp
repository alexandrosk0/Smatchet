// libFuzzer driver for the time-tracking duration parser —
// TicketGridDurationSortPure::ParseDurationToSecondsForSort (TicketGridDurationSortPure.cpp).
// This runs inside a UI-thread sort comparator over tracker-supplied (untrusted) time-tracking
// cell values ("3h 30m", "1w 2d", "2.5h", ...), so it must TERMINATE on any input and never
// overflow long long: a non-unit char after a number used to stall the cursor forever (permanent
// UI freeze — CPP_CODE_AUDIT.md #3), and a huge magnitude used to overflow the accumulator
// (CPP_CODE_AUDIT.md #19). Both are now fixed; this target keeps the fix honest — a hang shows up
// as a libFuzzer timeout, UB/overflow as a UBSan abort. The function takes a RAW string, so the
// fuzz bytes go straight in. nightly fuzz-smoke Layer 3.
#include "TicketGridDurationSortPure.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string value(reinterpret_cast<const char*>(data), size);
    (void)TicketGridDurationSortPure::ParseDurationToSecondsForSort(value);
    // Split the input so the comparator path (which re-parses both sides) is exercised too.
    const std::size_t mid = size / 2;
    const std::string a(reinterpret_cast<const char*>(data), mid);
    const std::string b(reinterpret_cast<const char*>(data) + mid, size - mid);
    (void)TicketGridDurationSortPure::CompareTimeTrackingValues(a, b);
    return 0;
}
