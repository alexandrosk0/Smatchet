// CodingHarnessSeedBuilder — pure builder/parser for the SEED.{md,json}
// handoff envelope. Wire spec for the runner ↔ spawned-harness contract.
// Snapshot test in tests/Source_Core/CodingHarnessSeedBuilder.test.cpp locks
// the format.

#include "CodingHarnessSeedBuilder.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace CodingHarness {
namespace SeedBuilder {

namespace {

// Canonical top-level JSON keys. Listed once so the snapshot test can sanity-
// check the set against the production source.
const char* const kKeyProposalId        = "proposalId";
const char* const kKeySourceTracker     = "sourceTracker";
const char* const kKeyIssueKey          = "issueKey";
const char* const kKeyIssueTitle        = "issueTitle";
const char* const kKeyIssueBodyMarkdown = "issueBodyMarkdown";
const char* const kKeyApproachOutline   = "approachOutline";
const char* const kKeyComplexityHint    = "complexityHint";
const char* const kKeyTargetBranch      = "targetBranch";
const char* const kKeyWorkingDirectory  = "workingDirectory";
const char* const kKeyTimestampUnixSec  = "timestampUnixSec";

bool IsKnownTopLevelKey(const std::string& k) {
    return k == kKeyProposalId || k == kKeySourceTracker || k == kKeyIssueKey ||
           k == kKeyIssueTitle || k == kKeyIssueBodyMarkdown || k == kKeyApproachOutline ||
           k == kKeyComplexityHint || k == kKeyTargetBranch || k == kKeyWorkingDirectory ||
           k == kKeyTimestampUnixSec;
}

// (CR Bundle B1) Scan a markdown body for the longest run of consecutive
// backticks and return the run length. The outer fence then uses
// `max(3, longestRun + 1)` backticks so an embedded ```rust ... ``` block
// never terminates the wrapping fence early. Issue bodies pasted from
// CodeRabbit / GitHub regularly contain triple-backtick code blocks; without
// this, the second `'''` closes the seed's fence and the rest of the body
// leaks into the next markdown section.
size_t LongestBacktickRun(const std::string& body) {
    size_t longest = 0;
    size_t current = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '`') {
            ++current;
            if (current > longest) {
                longest = current;
            }
        } else {
            current = 0;
        }
    }
    return longest;
}

// Emit a Markdown fence around free-form text so subsequent sections never
// terminate early on an embedded heading.
void EmitFencedBlock(std::ostringstream& os, const char* heading, const std::string& body) {
    os << "## " << heading << "\n\n";
    if (body.empty()) {
        os << "_(empty)_\n\n";
        return;
    }
    // (CR Bundle B1) Adaptive fence length — at least three backticks, but
    // one more than the longest backtick run in the body. This lets the
    // wrapper survive a body that carries its own code fences.
    const size_t bodyRun = LongestBacktickRun(body);
    const size_t fenceLen = (bodyRun + 1 > 3) ? (bodyRun + 1) : 3;
    const std::string fence(fenceLen, '`');
    os << fence << "\n" << body;
    if (!body.empty() && body.back() != '\n') {
        os << '\n';
    }
    os << fence << "\n\n";
}

} // namespace

std::string FormatSeedMarkdown(const Seed& s) {
    std::ostringstream os;
    os << "# Smatchet handoff seed\n\n";
    os << "- **proposalId:** " << s.proposalId << "\n";
    os << "- **sourceTracker:** " << s.sourceTracker << "\n";
    os << "- **issueKey:** " << s.issueKey << "\n";
    os << "- **issueTitle:** " << s.issueTitle << "\n";
    os << "- **complexityHint:** " << s.complexityHint << "\n";
    os << "- **targetBranch:** " << s.targetBranch << "\n";
    os << "- **workingDirectory:** " << s.workingDirectory << "\n";
    os << "- **timestampUnixSec:** " << s.timestampUnixSec << "\n\n";

    EmitFencedBlock(os, "Issue body", s.issueBodyMarkdown);
    EmitFencedBlock(os, "Approach outline", s.approachOutline);

    return os.str();
}

nlohmann::json FormatSeedJson(const Seed& s) {
    nlohmann::json j = nlohmann::json::object();
    j[kKeyProposalId]        = s.proposalId;
    j[kKeySourceTracker]     = s.sourceTracker;
    j[kKeyIssueKey]          = s.issueKey;
    j[kKeyIssueTitle]        = s.issueTitle;
    j[kKeyIssueBodyMarkdown] = s.issueBodyMarkdown;
    j[kKeyApproachOutline]   = s.approachOutline;
    j[kKeyComplexityHint]    = s.complexityHint;
    j[kKeyTargetBranch]      = s.targetBranch;
    j[kKeyWorkingDirectory]  = s.workingDirectory;
    j[kKeyTimestampUnixSec]  = s.timestampUnixSec;

    // Forward-compat passthrough: any key the producer attached to payloadExtra
    // that doesn't collide with a known top-level field is merged in. Collisions
    // with known keys are dropped — the typed field wins to keep round-tripping
    // honest.
    if (s.payloadExtra.is_object()) {
        for (auto it = s.payloadExtra.begin(); it != s.payloadExtra.end(); ++it) {
            if (IsKnownTopLevelKey(it.key())) {
                continue;
            }
            j[it.key()] = it.value();
        }
    }
    return j;
}

bool ParseSeedJson(const nlohmann::json& j, Seed& out, std::string& outError) {
    if (!j.is_object()) {
        outError = "SEED.json root must be an object";
        return false;
    }

    // Required string fields — any missing entry is a hard fail. Empty-string
    // is permitted (e.g. issueTitle may legitimately be empty when the tracker
    // returns no title), but the key MUST be present.
    auto requireString = [&](const char* key, std::string& dst) -> bool {
        const auto it = j.find(key);
        if (it == j.end()) {
            outError = std::string("SEED.json missing required field: ") + key;
            return false;
        }
        if (!it->is_string()) {
            outError = std::string("SEED.json field is not a string: ") + key;
            return false;
        }
        dst = it->get<std::string>();
        return true;
    };

    if (!requireString(kKeySourceTracker,     out.sourceTracker))     return false;
    if (!requireString(kKeyIssueKey,          out.issueKey))          return false;
    if (!requireString(kKeyIssueTitle,        out.issueTitle))        return false;
    if (!requireString(kKeyIssueBodyMarkdown, out.issueBodyMarkdown)) return false;
    if (!requireString(kKeyApproachOutline,   out.approachOutline))   return false;
    if (!requireString(kKeyComplexityHint,    out.complexityHint))    return false;
    if (!requireString(kKeyTargetBranch,      out.targetBranch))      return false;
    if (!requireString(kKeyWorkingDirectory,  out.workingDirectory))  return false;

    {
        const auto it = j.find(kKeyProposalId);
        if (it == j.end() || !it->is_number_integer()) {
            outError = "SEED.json missing or non-integer field: proposalId";
            return false;
        }
        out.proposalId = it->get<int64_t>();
    }
    {
        const auto it = j.find(kKeyTimestampUnixSec);
        if (it == j.end() || !it->is_number_integer()) {
            outError = "SEED.json missing or non-integer field: timestampUnixSec";
            return false;
        }
        out.timestampUnixSec = it->get<int64_t>();
    }

    // Capture every non-known top-level key into payloadExtra so the parse is
    // lossless. Producers that bumped the wire schema mid-flight survive a
    // round-trip without coordination.
    out.payloadExtra = nlohmann::json::object();
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (IsKnownTopLevelKey(it.key())) {
            continue;
        }
        out.payloadExtra[it.key()] = it.value();
    }

    outError.clear();
    return true;
}

} // namespace SeedBuilder
} // namespace CodingHarness
