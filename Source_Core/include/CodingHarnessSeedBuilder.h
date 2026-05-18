#ifndef SMATCHET_CODING_HARNESS_SEED_BUILDER_H
#define SMATCHET_CODING_HARNESS_SEED_BUILDER_H

// Pure builder/parser for the SEED.{md,json} handoff envelope. Deterministic —
// the SEED.json shape produced here is the wire spec between the runner side
// (Smatchet code, AgenticHandoffController + ClaudeCodeLocalRunner) and the
// spawned harness's first delegate (`handoff-implementer`). The snapshot
// doctest at tests/Source_Core/CodingHarnessSeedBuilder.test.cpp locks the
// shape; drift here = silent runtime breakage.
//
// AGENTIC-gated — Seed consumers reference AgentProposal which itself lives
// behind the gate via ConfigManager. CMake list-conditionalises this TU; the
// header should only be included under `#if defined(SMATCHET_WITH_AGENTIC)`.

#include "CodingHarnessTypes.h"

#include <nlohmann/json.hpp>

#include <string>

namespace CodingHarness {
namespace SeedBuilder {

// Format the human-readable SEED.md companion. Deterministic: same Seed →
// byte-identical output. Used both at runner-spawn time and in the doctest
// snapshot. The Markdown shape is part of the wire spec — formatting changes
// require a coordinated update to handoff-implementer agent docs.
std::string FormatSeedMarkdown(const Seed& s);

// Serialise a Seed to nlohmann::json. Known fields land at top-level under
// canonical names (camelCase); payloadExtra keys merge in at top level after
// the known fields so the seed-builder is forward-compatible.
nlohmann::json FormatSeedJson(const Seed& s);

// Inverse of FormatSeedJson — parse a JSON object back into a Seed. Returns
// false + outError on missing required fields. Used by tests and by the
// spawned-harness side when it round-trips SEED.json. Unknown keys land in
// out.payloadExtra so consumers can carry forward-compat data without losing
// fidelity.
bool ParseSeedJson(const nlohmann::json& j, Seed& out, std::string& outError);

} // namespace SeedBuilder
} // namespace CodingHarness

#endif // SMATCHET_CODING_HARNESS_SEED_BUILDER_H
