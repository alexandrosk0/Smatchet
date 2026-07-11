#include <doctest/doctest.h>

#include "TicketFieldEditorCommitPolicyPure.h"

#include <cstddef>

// DR23 — the inline cell text editor seeds the stored value into a fixed char[512] buffer. When the
// stored value needs the whole buffer its tail is dropped on seed; committing that truncated copy
// would silently overwrite the tracker field. InlineEditLoadedTruncated is ANDed into the commit
// gate to refuse such a commit (the value is preserved) and surface a warning. The single-line hard
// cap stays; the long-text modal (already 64 KB + a "too large" banner) handles full documents.

using TicketFieldEditorCommitPolicyPure::InlineEditLoadedTruncated;

namespace {
constexpr std::size_t kInlineBufferCapacity = 512; // == sizeof(SpreadsheetState::EditBuffer)
}

TEST_CASE("InlineEditLoadedTruncated: a value that fits is not truncated -> commits normally") {
    CHECK_FALSE(InlineEditLoadedTruncated(/*originalValueSize=*/0, kInlineBufferCapacity));
    CHECK_FALSE(InlineEditLoadedTruncated(/*originalValueSize=*/10, kInlineBufferCapacity));
    // 511 bytes + the NUL terminator exactly fills the buffer with no loss.
    CHECK_FALSE(InlineEditLoadedTruncated(/*originalValueSize=*/511, kInlineBufferCapacity));
}

TEST_CASE("InlineEditLoadedTruncated: a value at/over capacity dropped its tail -> commit is refused") {
    // 512 bytes cannot coexist with the NUL in a 512-byte buffer: the last byte was dropped.
    CHECK(InlineEditLoadedTruncated(/*originalValueSize=*/512, kInlineBufferCapacity));
    CHECK(InlineEditLoadedTruncated(/*originalValueSize=*/4096, kInlineBufferCapacity));
}

TEST_CASE("InlineEditLoadedTruncated: a zero-capacity buffer never reports truncation (guard)") {
    CHECK_FALSE(InlineEditLoadedTruncated(/*originalValueSize=*/0, /*bufferCapacity=*/0));
    CHECK_FALSE(InlineEditLoadedTruncated(/*originalValueSize=*/100, /*bufferCapacity=*/0));
}
