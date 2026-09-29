#pragma once

// CappedBodyAccumulator — collects a streamed HTTP response body until a byte cap would be crossed, so a
// hostile or misconfigured server can never stream an unbounded body into memory. The cpr write callback
// forwards each chunk to Append and aborts the transfer when it returns false. Pure (no cpr, no I/O) so
// the accept/abort contract is unit-tested directly.

#include <cstddef>
#include <string>
#include <utility>

class CappedBodyAccumulator {
  public:
    explicit CappedBodyAccumulator(std::size_t maxBytes, std::size_t reserveBytes = 64u * 1024u) : maxBytes_(maxBytes) {
        body_.reserve(reserveBytes < maxBytes ? reserveBytes : maxBytes);
    }

    /// True (keep streaming) when `chunk` fits under the cap. False (abort) when it would cross it: the
    /// chunk is rejected whole, so the body never holds a truncated prefix, and Exceeded() turns true.
    bool Append(const std::string& chunk) {
        if (chunk.size() > maxBytes_ - body_.size()) {
            exceeded_ = true;
            return false;
        }
        body_.append(chunk);
        return true;
    }

    bool Exceeded() const { return exceeded_; }
    const std::string& Body() const { return body_; }
    std::string TakeBody() { return std::move(body_); }

  private:
    std::size_t maxBytes_;
    std::string body_;
    bool exceeded_ = false;
};
