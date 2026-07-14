// libFuzzer driver for the shared bounded-JSON ingress parser —
// smatchet::json_safe::ParseBounded / ParseBoundedOrDiscarded
// (Json/BoundedJsonParse.h). This is the single depth/node/byte-bounded parse
// that every attacker-controlled JSON ingress routes through: the Lua
// `decode_json` sink, the MCP REST/JSON-RPC POST handlers, the Lua-MCP-tool
// params path, and (command-input-hardening Phase 0) the `ParamType::Json`
// command-argument coercion + `config.set` value parse. Its Pillar-3 contract is
// that NO throw ever escapes the call boundary (allow_exceptions=false) and that
// a depth-bomb ("[[[[...]]]]") is rejected BEFORE nlohmann builds a deep DOM that
// would overflow the stack on recursive teardown — so this driver hammers that
// contract on adversarial bytes under ASan+UBSan.
//
// The first two input bytes are consumed as cap knobs (maxDepth, maxNodes) so the
// fuzzer explores the boundary where the SAX handler aborts — the interesting UB
// surface is the abort-mid-parse path, not just the happy DOM. The remaining bytes
// are the JSON text. Both entry points are driven (ParseBounded's err-string
// contract + ParseBoundedOrDiscarded's discarded-on-failure contract), plus a
// tiny-byte-cap pass to hit the TooLargeError() early-out.
#include "Json/BoundedJsonParse.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Derive small, input-controlled caps from the leading bytes so the fuzzer can
    // steer the parse into the depth/node abort paths without needing a genuinely
    // 256-deep payload. Clamp to a sane non-zero floor so a legitimate parse is
    // still reachable.
    int maxDepth = 256;
    std::size_t maxNodes = 200000u;
    const uint8_t* body = data;
    std::size_t bodyLen = size;
    if (size >= 1) {
        maxDepth = 1 + (data[0] % 64); // [1, 64]
        body += 1;
        bodyLen -= 1;
    }
    if (bodyLen >= 1) {
        maxNodes = 1u + (static_cast<std::size_t>(body[0]) % 256u); // [1, 256]
        body += 1;
        bodyLen -= 1;
    }

    const std::string text(reinterpret_cast<const char*>(body), bodyLen);

    // Full default caps — the production ingress path.
    {
        std::string err;
        (void)smatchet::json_safe::ParseBounded(text, err);
    }
    // Input-controlled tight caps — exercises the OverflowError() depth/node abort.
    {
        std::string err;
        (void)smatchet::json_safe::ParseBounded(text, err, /*maxBytes=*/4u * 1024u * 1024u, maxDepth, maxNodes);
    }
    // Tiny byte cap — exercises the TooLargeError() early-out on any non-empty text.
    {
        std::string err;
        (void)smatchet::json_safe::ParseBounded(text, err, /*maxBytes=*/8u, maxDepth, maxNodes);
    }
    // The discarded-on-failure convenience wrapper (its own is_discarded() contract).
    (void)smatchet::json_safe::ParseBoundedOrDiscarded(text, /*maxBytes=*/4u * 1024u * 1024u, maxDepth, maxNodes);

    return 0;
}
