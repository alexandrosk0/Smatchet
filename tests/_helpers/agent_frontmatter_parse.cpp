// Minimal YAML frontmatter parser for agent files — test-only. Regex-based,
// no third-party deps. Reused by HandoffAgentFrontmatter.test.cpp (H2) and
// PrIteratorAgentFrontmatter.test.cpp (H7).

#include "agent_frontmatter_parse.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace smatchet {
namespace test {
namespace frontmatter {

namespace {

void TrimInPlace(std::string& s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

// Strip a single surrounding pair of matching quotes (single or double). Used
// when a scalar value is quoted in the YAML — `name: "handoff-implementer"`
// should yield `handoff-implementer`, not the quoted form.
std::string Unquote(const std::string& v) {
    if (v.size() >= 2) {
        const char first = v.front();
        const char last  = v.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return v.substr(1, v.size() - 2);
        }
    }
    return v;
}

// Parse a YAML flow sequence body — `foo, bar, "baz, qux"` → {"foo","bar","baz, qux"}.
// Handles quoted entries with embedded commas; rejects unbalanced quotes by treating
// the rest of the input as one entry (test-only — agent files do not need bulletproof
// quote handling).
std::vector<std::string> ParseFlowSequence(const std::string& body) {
    std::vector<std::string> out;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (!inSingle && c == '"') {
            inDouble = !inDouble;
            current.push_back(c);
            continue;
        }
        if (!inDouble && c == '\'') {
            inSingle = !inSingle;
            current.push_back(c);
            continue;
        }
        if (!inSingle && !inDouble && c == ',') {
            std::string entry = current;
            TrimInPlace(entry);
            if (!entry.empty()) {
                out.push_back(Unquote(entry));
            }
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    std::string tail = current;
    TrimInPlace(tail);
    if (!tail.empty()) {
        out.push_back(Unquote(tail));
    }
    return out;
}

// Split the frontmatter block into lines; strips the trailing '\r' from
// CRLF-encoded files transparently.
std::vector<std::string> SplitLines(const std::string& block) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream is(block);
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        out.push_back(line);
    }
    return out;
}

bool ExtractFrontmatterBlock(const std::string& fileContent, std::string& outBlock, std::string& outError) {
    // Frontmatter must start with `---` on the very first line — agent files
    // that lack frontmatter (no `---` at line 0) are rejected explicitly so the
    // doctest can distinguish "file missing frontmatter" from "frontmatter
    // malformed".
    if (fileContent.size() < 4 || fileContent.substr(0, 3) != "---" ||
        (fileContent[3] != '\n' && fileContent[3] != '\r')) {
        outError = "file does not begin with `---`";
        return false;
    }
    size_t cursor = 3;
    while (cursor < fileContent.size() && (fileContent[cursor] == '\r' || fileContent[cursor] == '\n')) {
        ++cursor;
    }
    const size_t blockStart = cursor;

    // Walk forward looking for a line whose first three chars are `---`.
    while (cursor < fileContent.size()) {
        const size_t lineStart = cursor;
        while (cursor < fileContent.size() && fileContent[cursor] != '\n') {
            ++cursor;
        }
        const std::string line = fileContent.substr(lineStart, cursor - lineStart);
        std::string trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r') {
            trimmed.pop_back();
        }
        if (trimmed == "---") {
            outBlock = fileContent.substr(blockStart, lineStart - blockStart);
            return true;
        }
        if (cursor < fileContent.size()) {
            ++cursor; // step past '\n'
        }
    }

    outError = "frontmatter block missing closing `---`";
    return false;
}

} // namespace

bool ParseFrontmatter(const std::string& filePath, ParsedFrontmatter& out, std::string& outError) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs) {
        outError = "cannot open file: " + filePath;
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    const std::string content = oss.str();

    std::string block;
    if (!ExtractFrontmatterBlock(content, block, outError)) {
        return false;
    }

    const auto lines = SplitLines(block);

    // Two-state walker: when we encounter `key:` with nothing after the colon
    // (or only whitespace), the next contiguous run of `  - <value>` lines is
    // captured as a block sequence under that key.
    std::string pendingSequenceKey;
    std::vector<std::string> pendingSequenceItems;

    auto flushPending = [&]() {
        if (pendingSequenceKey.empty()) {
            return;
        }
        if (pendingSequenceKey == "triggers") {
            out.triggers = pendingSequenceItems;
        } else if (pendingSequenceKey == "delegates-to" || pendingSequenceKey == "delegatesTo") {
            out.delegatesTo = pendingSequenceItems;
        }
        // Always store joined form in rawScalars too — tests reading uncommon
        // sequence keys (e.g. `capabilities`) can split on `,`.
        std::ostringstream joined;
        for (size_t i = 0; i < pendingSequenceItems.size(); ++i) {
            if (i > 0) joined << ",";
            joined << pendingSequenceItems[i];
        }
        out.rawScalars[pendingSequenceKey] = joined.str();
        pendingSequenceKey.clear();
        pendingSequenceItems.clear();
    };

    const std::regex scalarLine(R"(^([A-Za-z_][A-Za-z0-9_\-]*)\s*:\s*(.*)$)");
    const std::regex sequenceItemLine(R"(^\s+-\s+(.*)$)");

    for (const auto& raw : lines) {
        if (raw.empty()) {
            // Blank line terminates a pending sequence.
            flushPending();
            continue;
        }
        // Comments — `# ...` lines are skipped; a `#` after content stays as
        // part of the value (YAML treats it as a comment but agent files do
        // not use inline comments).
        if (!raw.empty() && raw[0] == '#') {
            continue;
        }

        std::smatch seqMatch;
        if (!pendingSequenceKey.empty() && std::regex_match(raw, seqMatch, sequenceItemLine)) {
            std::string item = seqMatch[1].str();
            TrimInPlace(item);
            pendingSequenceItems.push_back(Unquote(item));
            continue;
        }

        std::smatch scalarMatch;
        if (!std::regex_match(raw, scalarMatch, scalarLine)) {
            // Lines we don't understand (nested-map continuations like
            // `  claude-code:`) — flush any pending sequence and skip. We do
            // not capture nested map contents.
            flushPending();
            continue;
        }

        // New top-level key encountered — flush pending sequence first.
        flushPending();

        const std::string key  = scalarMatch[1].str();
        std::string value      = scalarMatch[2].str();
        TrimInPlace(value);

        if (value.empty()) {
            // Bare `key:` — start collecting a block sequence under this key.
            pendingSequenceKey = key;
            pendingSequenceItems.clear();
            continue;
        }

        // Flow sequence shorthand: `key: [a, b, c]`.
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            std::vector<std::string> entries = ParseFlowSequence(value.substr(1, value.size() - 2));
            if (key == "triggers") {
                out.triggers = entries;
            } else if (key == "delegates-to" || key == "delegatesTo") {
                out.delegatesTo = entries;
            }
            std::ostringstream joined;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) joined << ",";
                joined << entries[i];
            }
            out.rawScalars[key] = joined.str();
            continue;
        }

        // Scalar value.
        const std::string unq = Unquote(value);
        out.rawScalars[key] = unq;
        if (key == "name") {
            out.name = unq;
        } else if (key == "description") {
            out.description = unq;
        } else if (key == "complexity") {
            out.complexity = unq;
        } else if (key == "read-only" || key == "readOnly") {
            out.readOnly = (unq == "true" || unq == "True" || unq == "yes");
        } else if (key == "version") {
            try {
                out.version = std::stoi(unq);
            } catch (...) {
                outError = "version is not an integer: " + unq;
                return false;
            }
        }
    }

    // Final flush for a sequence that ran up to EOF.
    flushPending();

    outError.clear();
    return true;
}

} // namespace frontmatter
} // namespace test
} // namespace smatchet
