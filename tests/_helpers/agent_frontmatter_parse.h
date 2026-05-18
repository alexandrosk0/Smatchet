#ifndef SMATCHET_TESTS_AGENT_FRONTMATTER_PARSE_H
#define SMATCHET_TESTS_AGENT_FRONTMATTER_PARSE_H

// Minimal test-only YAML frontmatter parser for `agents/<name>.md` files.
// Regex-based, C++14, stdlib only — no yaml-cpp dependency. Lives under
// tests/_helpers so the doctest rig can assert agent frontmatter without
// pulling third-party YAML into the production build. Reused by H2
// (HandoffAgentFrontmatter.test.cpp) and H7 (PrIteratorAgentFrontmatter.test.cpp).
//
// Supported YAML subset:
//   - scalar:  `key: value`  (optionally `"value"` or `'value'`)
//   - block sequences: `key:\n  - foo\n  - bar`
//   - flow sequences:  `key: [foo, bar]`
//   - top-level keys only (nested maps go into rawScalars verbatim)
//
// Comments + blank lines tolerated. Unsupported constructs (anchors, multi-line
// scalars, block maps) are not used by Smatchet agent files.

#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {
namespace test {
namespace frontmatter {

struct ParsedFrontmatter {
    // Structured fields the H2 + H7 doctests read directly.
    std::string name;
    std::string description;
    std::string complexity;
    bool readOnly = false;
    std::vector<std::string> triggers;
    std::vector<std::string> delegatesTo;
    int version = 0;

    // Every parsed scalar (including the structured fields above and any extras
    // like `class`, `model`, custom keys) lives here keyed by the literal YAML
    // key name. Tests assert against rarely-needed fields via rawScalars.
    std::unordered_map<std::string, std::string> rawScalars;
};

// Parse the YAML frontmatter at the top of the given file. The file must
// start with a `---` line and contain a second `---` closing line; everything
// between is the frontmatter. Returns false + outError on malformed input.
// Body content past the closing `---` is ignored.
bool ParseFrontmatter(const std::string& filePath, ParsedFrontmatter& out, std::string& outError);

} // namespace frontmatter
} // namespace test
} // namespace smatchet

#endif // SMATCHET_TESTS_AGENT_FRONTMATTER_PARSE_H
