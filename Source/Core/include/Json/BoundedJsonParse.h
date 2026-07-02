#pragma once

// BoundedJsonParse.h — depth/node-bounded JSON parse for ATTACKER-CONTROLLED
// input (Pillar 3 — Never crash).

// An unbounded `json::parse` on a deeply-nested payload ("[[[[...]]]]") builds the
// full DOM first: nlohmann's parser is iterative (it drives the DOM builder from
// `sax_parse`, NOT a per-level recursion), so the stack does NOT overflow during the
// parse. The danger is the resulting DOM — `~json` destroys nested containers
// RECURSIVELY, so tearing down a deep tree overflows the C++ stack (a remote crash,
// NOT a catchable C++ exception), and building it grows the heap unboundedly. A byte
// cap alone does NOT bound depth: ~1M nested arrays fit in ~1 MiB. We therefore drive
// nlohmann's own DOM builder from `sax_parse` and ABORT once a live-container depth or
// a total-node cap is hit — before any deep / oversized DOM is ever constructed.

// This is the single shared implementation. All bounded-parse ingress sites route
// here (DRY / Pillar 5): the `decode_json` Lua sink, the MCP REST/JSON-RPC POST
// handlers, and the Lua-MCP-tool params path. Do NOT fork BoundedDecodeSax again.

// Header-only + nlohmann-only so it links cleanly into both Source/Core and the
// Source/Plugins/Mcp strict zone, and into the pure doctest rig. No throw escapes
// any function here (`allow_exceptions=false`).

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

namespace smatchet {
namespace json_safe {

// Default caps — far above any legitimate JSON these ingress sites exchange
// (tracker payloads are a handful of levels deep, a few thousand nodes) while
// bounding stack + heap growth well short of exhaustion.
constexpr int kDefaultMaxDepth = 256;
constexpr std::size_t kDefaultMaxNodes = 200000u;

// Stable error strings written to `errOut` on failure. Callers map these to the
// protocol-correct error for their transport; they never contain attacker input.
inline const char* OverflowError() { return "input too deeply nested or too many elements"; }
inline const char* TooLargeError() { return "input too large"; }
inline const char* InvalidJsonError() { return "invalid JSON"; }

// Bounded SAX handler: wraps nlohmann's own DOM builder, rejecting once the live
// container depth exceeds maxDepth_ or the total node count exceeds maxNodes_.
// Returning `false` from any callback aborts sax_parse without descending
// further, so the parser's recursion is hard-bounded by the cap (ASAN-safe even
// on a hostile arbitrarily-deep string).
class BoundedDecodeSax : public nlohmann::detail::json_sax_dom_parser<nlohmann::json> {
  public:
    using base = nlohmann::detail::json_sax_dom_parser<nlohmann::json>;
    BoundedDecodeSax(nlohmann::json& root, int maxDepth, std::size_t maxNodes)
        : base(root, /*allow_exceptions=*/false), maxDepth_(maxDepth), maxNodes_(maxNodes) {}

    bool null() { return Count() && base::null(); }
    bool boolean(bool v) { return Count() && base::boolean(v); }
    bool number_integer(nlohmann::json::number_integer_t v) { return Count() && base::number_integer(v); }
    bool number_unsigned(nlohmann::json::number_unsigned_t v) { return Count() && base::number_unsigned(v); }
    bool number_float(nlohmann::json::number_float_t v, const nlohmann::json::string_t& s) {
        return Count() && base::number_float(v, s);
    }
    bool string(nlohmann::json::string_t& v) { return Count() && base::string(v); }
    bool binary(nlohmann::json::binary_t& v) { return Count() && base::binary(v); }

    bool start_object(std::size_t elements) {
        if (!Count() || !Descend()) {
            return false;
        }
        return base::start_object(elements);
    }
    bool end_object() {
        --depth_;
        return base::end_object();
    }
    bool start_array(std::size_t elements) {
        if (!Count() || !Descend()) {
            return false;
        }
        return base::start_array(elements);
    }
    bool end_array() {
        --depth_;
        return base::end_array();
    }
    bool key(nlohmann::json::string_t& v) { return base::key(v); }

    bool Overflowed() const { return overflowed_; }

  private:
    bool Count() {
        if (++nodes_ > maxNodes_) {
            overflowed_ = true;
            return false;
        }
        return true;
    }
    bool Descend() {
        if (depth_ >= maxDepth_) {
            overflowed_ = true;
            return false;
        }
        ++depth_;
        return true;
    }

    int maxDepth_;
    std::size_t maxNodes_;
    int depth_ = 0;
    std::size_t nodes_ = 0;
    bool overflowed_ = false;
};

// Parse `text` into a nlohmann::json under hard byte / depth / node caps.
// Contract:
//   - Success → returns the parsed json; `errOut` is cleared (empty).
//   - Failure → returns a null json; `errOut` holds a stable, input-free message:
//       * TooLargeError()    — `text` exceeded maxBytes
//       * OverflowError()    — depth or node cap hit (the depth-bomb class)
//       * InvalidJsonError() — malformed JSON within the caps
//   - NEVER throws across the call boundary (allow_exceptions=false; the byte
//     check is a size compare). Callers test `errOut.empty()` for success.
inline nlohmann::json ParseBounded(const std::string& text, std::string& errOut,
                                   std::size_t maxBytes = 4u * 1024u * 1024u, int maxDepth = kDefaultMaxDepth,
                                   std::size_t maxNodes = kDefaultMaxNodes) {
    errOut.clear();
    if (text.size() > maxBytes) {
        errOut = TooLargeError();
        return nlohmann::json();
    }
    nlohmann::json j;
    BoundedDecodeSax sax(j, maxDepth, maxNodes);
    const bool ok = nlohmann::json::sax_parse(text, &sax, nlohmann::json::input_format_t::json,
                                              /*strict=*/true, /*ignore_comments=*/false);
    if (!ok) {
        errOut = sax.Overflowed() ? OverflowError() : InvalidJsonError();
        return nlohmann::json();
    }
    return j;
}

// Convenience wrapper for the many ingress sites that historically used
// `json::parse(text, nullptr, false)` and branch on `.is_discarded()`. Parses
// `text` under the SAME hard byte/depth/node caps as ParseBounded, returning a
// DISCARDED json on ANY failure (invalid / too large / too deeply-nested). This
// lets those sites adopt the depth-bomb guard with a one-line swap — their
// existing `.is_discarded()` checks keep working unchanged, and the pre-existing
// "invalid → discarded" behaviour is preserved while the new "too deep/large →
// discarded" (instead of a SIGSEGV on deep-DOM teardown) is added. New code that
// wants the specific failure reason should call ParseBounded directly.
inline nlohmann::json ParseBoundedOrDiscarded(const std::string& text,
                                              std::size_t maxBytes = 4u * 1024u * 1024u,
                                              int maxDepth = kDefaultMaxDepth,
                                              std::size_t maxNodes = kDefaultMaxNodes) {
    std::string err;
    nlohmann::json j = ParseBounded(text, err, maxBytes, maxDepth, maxNodes);
    if (!err.empty()) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return j;
}

} // namespace json_safe
} // namespace smatchet
