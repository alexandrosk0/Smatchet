// AiModelSignature pure-helper tests — F2 "auto-clear chat on model change".
// Covers the five scenarios encoded in the helper:
//   1. Empty previous signature -> first turn, never clear.
//   2. Identical new signature -> no change, never clear.
//   3. Different provider -> clear.
//   4. Different model -> clear.
//   5. Reasoning-effort changes never enter the signature -> never trigger clear.

#include "AiModelSignature.h"

#include <doctest/doctest.h>

#include <string>

TEST_CASE("AiModelSignature: empty previous signature never clears (first turn)") {
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange(std::string(), "openai", "gpt-4o-mini");
    CHECK_FALSE(r.ShouldClear);
    // NewSignature must still be populated so the controller can cache it for next turn.
    CHECK(r.NewSignature == "openai|gpt-4o-mini");
}

TEST_CASE("AiModelSignature: identical signature is a no-op") {
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange("openai|gpt-4o-mini", "openai", "gpt-4o-mini");
    CHECK_FALSE(r.ShouldClear);
    CHECK(r.NewSignature == "openai|gpt-4o-mini");
}

TEST_CASE("AiModelSignature: provider change triggers clear") {
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange("openai|gpt-4o-mini", "anthropic", "gpt-4o-mini");
    CHECK(r.ShouldClear);
    CHECK(r.NewSignature == "anthropic|gpt-4o-mini");
}

TEST_CASE("AiModelSignature: model change (same provider) triggers clear") {
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange("openai|gpt-4o-mini", "openai", "gpt-4o");
    CHECK(r.ShouldClear);
    CHECK(r.NewSignature == "openai|gpt-4o");
}

TEST_CASE("AiModelSignature: DeepSeek <-> OpenAI transition triggers clear") {
    // Adjacent-provider transition shaped like the actual user-driven flow
    // when switching from OpenAI to DeepSeek via the Preferences combo.
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange("openai|gpt-4o-mini", "openai", "deepseek-chat");
    CHECK(r.ShouldClear);
    CHECK(r.NewSignature == "openai|deepseek-chat");
}

TEST_CASE("AiModelSignature: reasoning effort never enters the signature") {
    // The helper takes only (previous, providerName, model). Effort lives
    // outside the call surface so it CAN'T leak into NewSignature — making
    // effort-only changes (low / medium / high) silent by construction.
    const smatchet::ai::ModelSignatureChangeResult r =
        smatchet::ai::DetectModelChange("openai|deepseek-reasoner", "openai", "deepseek-reasoner");
    CHECK_FALSE(r.ShouldClear);
    // Assertion: NewSignature does NOT contain any of the effort strings.
    CHECK(r.NewSignature.find("low") == std::string::npos);
    CHECK(r.NewSignature.find("medium") == std::string::npos);
    CHECK(r.NewSignature.find("high") == std::string::npos);
    CHECK(r.NewSignature.find("auto") == std::string::npos);
}
