// CoderabbitCommentClassifier.test.cpp — phase-3 of the
// `coderabbit-react-loop` plan. Exercises the concrete classifier impl
// + its 18-rule override table.
//
// Test surface:
//   - bot-author allow-list + Skip paths (handoff marker, react-reply
//     marker, non-bot author),
//   - each of the 18 override rules with a positive AND a negative case,
//   - ≥ 3 survivor / Dispatch cases (real-shape CodeRabbit suggestions
//     that should NOT reject — they route to coderabbit-triage),
//   - SetShortCircuitRejectEnabled(false) forces Dispatch even on
//     rule-1-shaped bodies (debugging knob),
//   - markdown-table guard: parse agents/coderabbit-triage.md § Override
//     rules and assert the rule-number set + short-reason column match
//     what's encoded in the C++ descriptor table.

#include "CoderabbitCommentClassifier.h"

#include "AgenticHandoffController.h" // PostedComment
#include "CoderabbitCommentClassifierPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef SMATCHET_TESTS_REPO_ROOT
#error "SMATCHET_TESTS_REPO_ROOT must be defined to locate agents/coderabbit-triage.md"
#endif

using ::smatchet::agentic::ClassificationResult;
using ::smatchet::agentic::ClassifierVerdict;
using ::smatchet::agentic::CoderabbitCommentClassifier;
using ::smatchet::agentic::PostedComment;

namespace {

// Build a PostedComment with sensible defaults so tests can override only
// the field they exercise.
PostedComment MakeComment(const std::string& body, const std::string& author = "coderabbitai[bot]") {
    PostedComment c;
    c.id = "id-test";
    c.author = author;
    c.body = body;
    c.createdAtSec = 0;
    return c;
}

// Wrap a body fragment inside a CodeRabbit-style ```suggestion fence.
std::string Suggestion(const std::string& inside) {
    std::ostringstream os;
    os << "Some prose.\n\n```suggestion\n" << inside << "\n```\n";
    return os.str();
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Read a file into a string. Used by the markdown-table guard test.
std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    REQUIRE_MESSAGE(in.is_open(), ("Cannot open: " + path).c_str());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string AsciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z')
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        else
            out.push_back(c);
    }
    return out;
}

} // namespace

TEST_SUITE("CoderabbitCommentClassifier") {

    // ----------------------------------------------------------------------
    // Bot-author / Skip-path coverage
    // ----------------------------------------------------------------------

    TEST_CASE("Skip: smatchet-handoff marker") {
        CoderabbitCommentClassifier cls;
        const std::string body = "<!-- smatchet-handoff -->\n\nWe already handled this PR.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Skip);
        CHECK(r.skipReason == "smatchet marker");
    }

    TEST_CASE("Skip: smatchet-coderabbit-react marker (our own reject reply)") {
        CoderabbitCommentClassifier cls;
        std::string body =
            std::string(CoderabbitCommentClassifier::RejectReplyMarker()) + "\n\nOverride rule #1 — C++14 hard.\n";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Skip);
        CHECK(r.skipReason == "smatchet marker");
    }

    TEST_CASE("Skip: non-bot author (human reviewer)") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Looks good!", "alice-reviewer"));
        CHECK(r.verdict == ClassifierVerdict::Skip);
        CHECK(r.skipReason == "non-bot author");
    }

    TEST_CASE("Skip: empty allow-list rejects every author") {
        CoderabbitCommentClassifier cls;
        cls.SetBotLoginAllowList({});
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::string_view foo;")));
        CHECK(r.verdict == ClassifierVerdict::Skip);
    }

    TEST_CASE("Allow-list is case-insensitive — `CodeRabbitAI[bot]` matches") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::string_view foo;"), "CodeRabbitAI[bot]"));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
    }

    // ----------------------------------------------------------------------
    // 18 override rules — positive + negative case per rule
    // ----------------------------------------------------------------------

    TEST_CASE("Rule #1 — std::string_view rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::string_view name;")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #1"));
        CHECK(Contains(r.rejectReplyBody, "#1"));
    }
    TEST_CASE("Rule #1 — std::optional rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::optional<int> v;")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #1"));
    }
    TEST_CASE("Rule #1 — `if constexpr` rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("if constexpr (sizeof(T) > 4) {}")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #1"));
    }
    TEST_CASE("Rule #1 — structured binding rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("auto [a, b] = pair;")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #1"));
    }
    TEST_CASE("Rule #1 — NEGATIVE: prose-only `string view` survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r =
            cls.Classify(MakeComment("Consider taking a string view of the substring (no std:: token)."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #2 — #include <GLFW/...> in header rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body = "In Source_Core/include/Foo.h\n" + Suggestion("#include <GLFW/glfw3.h>");
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #2"));
    }
    TEST_CASE("Rule #2 — NEGATIVE: GLFW include with no header context survives") {
        CoderabbitCommentClassifier cls;
        // No `.h` token in body — body-only heuristic cannot prove header.
        ClassificationResult r = cls.Classify(MakeComment("Mention of GLFW in prose, nothing about header files."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #3 — local #define IMGUI_USE_WCHAR32 rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("#define IMGUI_USE_WCHAR32 1")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #3"));
    }
    TEST_CASE("Rule #3 — NEGATIVE: mention of IMGUI_USE_WCHAR32 without #define survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Note: IMGUI_USE_WCHAR32 is PUBLIC on ImGuiLib."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #4 — replace LOG_INFO with fprintf(stderr) rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("fprintf(stderr, \"error\\n\");")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #4"));
    }
    TEST_CASE("Rule #4 — std::cerr rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::cerr << \"bad\" << std::endl;")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #4"));
    }
    TEST_CASE("Rule #4 — NEGATIVE: prose-only mention of stderr survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("The error message should go to standard error eventually."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #5 — nlohmann::json brace-list reassign rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body = Suggestion("nlohmann::json out;\nout = {{\"foo\", 1}, {\"bar\", 2}};");
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #5"));
    }
    TEST_CASE("Rule #5 — NEGATIVE: proper obj[\"k\"] = v survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("obj[\"key\"] = value;")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #6 — raw `new Foo()` rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("Foo* f = new Foo();")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #6"));
    }
    TEST_CASE("Rule #6 — NEGATIVE: `new line` prose survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Add a new line between the two declarations."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #7 — bare cpr::Get bypass rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r =
            cls.Classify(MakeComment(Suggestion("auto r = cpr::Get(cpr::Url{\"https://example.com\"});")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #7"));
    }
    TEST_CASE("Rule #7 — NEGATIVE: cpr::Get suggestion that also mentions TrackerHttpClient survives") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            "Inside TrackerHttpClient::Get the call already uses cpr::Get correctly; consider naming consistency.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #8 — sync SQLite call inside ImGui::Begin frame rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            Suggestion("ImGui::Begin(\"Panel\");\nSQLite::Database db(\"file.db\");\nImGui::End();");
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #8"));
    }
    TEST_CASE("Rule #8 — NEGATIVE: SQLite usage in a worker (no ImGui token) survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r =
            cls.Classify(MakeComment(Suggestion("SQLite::Database db(\"file.db\"); // worker thread")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #9 — drop noisy LOG_TRACE rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body = "Please remove this LOG_TRACE call for cleanliness — it's noisy.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #9"));
    }
    TEST_CASE("Rule #9 — NEGATIVE: prose mentioning LOG_TRACE without removal intent survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r =
            cls.Classify(MakeComment("Note: LOG_TRACE in this branch is good — it surfaces the slow path."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #10 — backward-compat shim suggestion rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body = "Consider keeping a backward compat alias for the old symbol name.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #10"));
    }
    TEST_CASE("Rule #10 — NEGATIVE: ordinary refactor without compat-shim prose survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Consider renaming the method for clarity. ish."));
        // Body contains 'rename' (rule 18 candidate) but no ' to ' connector — should dispatch.
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #11 — add WHAT-style comment about this PR rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            "Add a comment explaining what this PR does at the top of the file so future readers know.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #11"));
    }
    TEST_CASE("Rule #11 — NEGATIVE: WHY-style comment suggestion survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Consider documenting why this branch is needed."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #12 — defensive try/catch suggestion rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body = Suggestion("try {\n  doWork();\n} catch (const std::exception& e) {\n  // log\n}");
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #12"));
    }
    TEST_CASE("Rule #12 — NEGATIVE: no try/catch in suggestion survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("doWork();")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #13 — split trivial-visual PR rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            "Consider splitting this PR — the SmatchetTheme.cpp palette tweak should be a separate commit.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #13"));
    }
    TEST_CASE("Rule #13 — NEGATIVE: split suggestion for non-visual files survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(
            MakeComment("Consider splitting this PR — the SQLite migration should be separate from the controller."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #14 — touch *_DX12 target rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Update SmatchetCore_DX12 CMakeLists.txt to include this."));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #14"));
    }
    TEST_CASE("Rule #14 — NEGATIVE: Standalone-only suggestion survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Update SmatchetStandalone target's linkage."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #15 — new FetchContent_Declare dep rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r =
            cls.Classify(MakeComment(Suggestion("FetchContent_Declare(spdlog GIT_REPOSITORY ...)")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #15"));
    }
    TEST_CASE("Rule #15 — NEGATIVE: find_package(nlohmann_json) on allowed dep survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("find_package(nlohmann_json CONFIG REQUIRED)")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }
    TEST_CASE("Rule #15 — mixed body: allowed + new disallowed dep rejected") {
        // Per-occurrence evaluation — a body that paints an allowed
        // find_package alongside a disallowed one must NOT short-circuit
        // to allowed.
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(
            Suggestion("find_package(nlohmann_json CONFIG REQUIRED)\nfind_package(spdlog CONFIG REQUIRED)")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #15"));
    }
    TEST_CASE("Rule #8 — NEGATIVE: bare std::mutex declaration no longer false-triggers") {
        // A `std::mutex` declaration inside an ImGui-frame body must not
        // trip rule #8 — only lock-acquisition forms (`.lock(`, `lock_guard`,
        // `unique_lock`) count as the blocking-call signal.
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(
            MakeComment(Suggestion("ImGui::Begin(\"Panel\");\nstd::mutex stateMtx;\nImGui::End();")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }
    TEST_CASE("Rule #8 — std::lock_guard inside ImGui::Begin frame rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment(
            Suggestion("ImGui::Begin(\"Panel\");\nstd::lock_guard<std::mutex> g(stateMtx);\nImGui::End();")));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #8"));
    }

    TEST_CASE("Rule #16 — drop const& pass by value rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            "Drop const& and pass by value — the compiler will elide the copy for the std::string argument.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #16"));
    }
    TEST_CASE("Rule #16 — NEGATIVE: const& suggestion that ADDS the reference survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Pass the std::string by const reference, not by value."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #17 — std::map for deterministic order rejected") {
        CoderabbitCommentClassifier cls;
        const std::string body =
            "Switch from std::unordered_map to std::map<Key, Value> for deterministic iteration order.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #17"));
    }
    TEST_CASE("Rule #17 — NEGATIVE: std::map suggestion with no determinism prose survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Use std::map<Key, Value> here since lookup count is tiny."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    TEST_CASE("Rule #18 — rename public symbol rejected") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("Rename FooBar to FooBaz across the codebase."));
        CHECK(r.verdict == ClassifierVerdict::RejectShortCircuit);
        CHECK(Contains(r.rejectRuleCitation, "override #18"));
    }
    TEST_CASE("Rule #18 — NEGATIVE: prose mentioning rename without explicit target survives") {
        CoderabbitCommentClassifier cls;
        ClassificationResult r = cls.Classify(MakeComment("The variable name is fine — no need to change anything."));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
    }

    // ----------------------------------------------------------------------
    // Survivor / Dispatch cases — real-shape CodeRabbit suggestions that
    // should NOT reject. The watcher hands these off to coderabbit-triage.
    // ----------------------------------------------------------------------

    TEST_CASE("Survivor: nitpick about a missing LOG_INFO call (no override fires)") {
        CoderabbitCommentClassifier cls;
        const std::string body = "Consider adding a LOG_INFO after the cache-miss branch so the user sees the wait.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
        CHECK(r.dispatchTargetAgent == "coderabbit-triage");
    }

    TEST_CASE("Survivor: actionable suggestion to move work to a worker thread (no override fires)") {
        CoderabbitCommentClassifier cls;
        const std::string body = "This synchronous call should move onto a worker thread per pillar 2.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
        CHECK(r.dispatchTargetAgent == "coderabbit-triage");
    }

    TEST_CASE("Survivor: chore suggestion to clean up unused includes (no override fires)") {
        CoderabbitCommentClassifier cls;
        const std::string body = "Chore: drop the unused `<sstream>` include from this file.";
        ClassificationResult r = cls.Classify(MakeComment(body));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
        CHECK(r.dispatchTargetAgent == "coderabbit-triage");
    }

    // ----------------------------------------------------------------------
    // SetShortCircuitRejectEnabled(false) — debugging knob
    // ----------------------------------------------------------------------

    TEST_CASE("Short-circuit disabled: rule-1-shaped body falls through to Dispatch") {
        CoderabbitCommentClassifier cls;
        cls.SetShortCircuitRejectEnabled(false);
        CHECK(cls.GetShortCircuitRejectEnabled() == false);
        ClassificationResult r = cls.Classify(MakeComment(Suggestion("std::string_view name;")));
        CHECK(r.verdict == ClassifierVerdict::Dispatch);
        CHECK(r.dispatchTargetAgent == "coderabbit-triage");
    }

    TEST_CASE("Short-circuit disabled: skip path still wins on smatchet marker") {
        CoderabbitCommentClassifier cls;
        cls.SetShortCircuitRejectEnabled(false);
        ClassificationResult r = cls.Classify(MakeComment("<!-- smatchet-handoff -->\n\nstd::string_view banned"));
        CHECK(r.verdict == ClassifierVerdict::Skip);
    }

    // ----------------------------------------------------------------------
    // Static helper coverage
    // ----------------------------------------------------------------------

    TEST_CASE("BuildRejectReplyBody includes marker + rule number + reason") {
        const std::string body = CoderabbitCommentClassifier::BuildRejectReplyBody(7, "Tracker invariant");
        CHECK(Contains(body, CoderabbitCommentClassifier::RejectReplyMarker()));
        CHECK(Contains(body, "rule #7"));
        CHECK(Contains(body, "Tracker invariant"));
    }

    TEST_CASE("OverrideRuleCount is exactly 18") { CHECK(CoderabbitCommentClassifier::OverrideRuleCount() == 18); }

    TEST_CASE("LookupOverrideShortReason returns non-empty for 1..18, empty otherwise") {
        for (int i = 1; i <= 18; ++i) {
            CHECK(!CoderabbitCommentClassifier::LookupOverrideShortReason(i).empty());
        }
        CHECK(CoderabbitCommentClassifier::LookupOverrideShortReason(0).empty());
        CHECK(CoderabbitCommentClassifier::LookupOverrideShortReason(19).empty());
        CHECK(CoderabbitCommentClassifier::LookupOverrideShortReason(-1).empty());
    }

    TEST_CASE("GetBotLoginAllowList default is just `coderabbitai[bot]`") {
        CoderabbitCommentClassifier cls;
        const std::vector<std::string>& list = cls.GetBotLoginAllowList();
        REQUIRE(list.size() == 1);
        CHECK(list[0] == "coderabbitai[bot]");
    }

    // ----------------------------------------------------------------------
    // Markdown-table guard — agents/coderabbit-triage.md drift check
    // ----------------------------------------------------------------------

    TEST_CASE("18-rule numbers match agents/coderabbit-triage.md table") {
        const std::string path = std::string(SMATCHET_TESTS_REPO_ROOT) + "/agents/coderabbit-triage.md";
        const std::string md = ReadFile(path);

        // Parse the markdown table by walking lines, finding the header row
        // "| # | Suggestion shape | Reject because |", then collecting the
        // 18 data rows that follow. We accept any pipe-delimited row that
        // starts with `|` and whose first non-empty cell is an integer.
        std::vector<std::pair<int, std::string>> tableRows; // (number, rejectColumn)
        std::istringstream is(md);
        std::string line;
        bool seenHeader = false;
        while (std::getline(is, line)) {
            if (!seenHeader) {
                if (line.find("| # |") != std::string::npos && line.find("Suggestion shape") != std::string::npos) {
                    seenHeader = true;
                }
                continue;
            }
            if (line.empty() || line[0] != '|') {
                if (!tableRows.empty()) {
                    break; // table ended
                }
                continue;
            }
            // Split on `|`.
            std::vector<std::string> cells;
            std::string cell;
            std::istringstream ls(line);
            while (std::getline(ls, cell, '|')) {
                cells.push_back(cell);
            }
            if (cells.size() < 4) {
                continue;
            }
            // cells[0] is the prefix-empty cell before the first `|`;
            // cells[1] is the rule number, cells[2] is the suggestion shape,
            // cells[3] is the reject reason.
            std::string numCell = cells[1];
            // Trim whitespace.
            while (!numCell.empty() && std::isspace(static_cast<unsigned char>(numCell.front())))
                numCell.erase(numCell.begin());
            while (!numCell.empty() && std::isspace(static_cast<unsigned char>(numCell.back())))
                numCell.pop_back();
            if (numCell.empty() || !std::isdigit(static_cast<unsigned char>(numCell[0]))) {
                // Skip separator row (the `|---|...|` immediately after the header).
                continue;
            }
            int n = std::atoi(numCell.c_str());
            std::string reject = cells[3];
            while (!reject.empty() && std::isspace(static_cast<unsigned char>(reject.front())))
                reject.erase(reject.begin());
            while (!reject.empty() && std::isspace(static_cast<unsigned char>(reject.back())))
                reject.pop_back();
            tableRows.push_back(std::make_pair(n, reject));
        }

        REQUIRE_MESSAGE(
            tableRows.size() == 18,
            ("Expected exactly 18 rule rows in markdown table; got " + std::to_string(tableRows.size())).c_str());

        // Confirm rule numbers are {1..18}.
        std::set<int> numbers;
        for (std::size_t i = 0; i < tableRows.size(); ++i) {
            numbers.insert(tableRows[i].first);
        }
        for (int i = 1; i <= 18; ++i) {
            CHECK_MESSAGE(numbers.count(i) == 1, ("Markdown table missing rule #" + std::to_string(i)).c_str());
        }

        // Compare short-reason column to LookupOverrideShortReason — allow
        // partial substring match in one direction so minor prose drift
        // (added qualifiers, punctuation differences) doesn't break CI.
        // Strip markdown formatting (backticks) before comparing so the
        // markdown source can keep `code-style` formatting while the C++
        // short-reason carries the same prose without the fences.
        auto stripFormatting = [](const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c != '`') {
                    out.push_back(c);
                }
            }
            return out;
        };
        for (std::size_t i = 0; i < tableRows.size(); ++i) {
            const int n = tableRows[i].first;
            const std::string mdReason = stripFormatting(AsciiLower(tableRows[i].second));
            const std::string cppReason =
                stripFormatting(AsciiLower(CoderabbitCommentClassifier::LookupOverrideShortReason(n)));
            REQUIRE_MESSAGE(!cppReason.empty(), ("C++ table missing rule #" + std::to_string(n)).c_str());
            // Find a "key fragment" from the shorter string and check it
            // appears in the OTHER side. The previous code derived the key
            // from one side and searched the same side, making the check
            // tautological — every non-empty key would trivially be found
            // in its source string.
            const bool cppShorter = cppReason.size() < mdReason.size();
            const std::string& shorter = cppShorter ? cppReason : mdReason;
            const std::string& longer = cppShorter ? mdReason : cppReason;
            // Trim to first ~24 chars to keep the fragment short.
            const std::string key = shorter.size() > 24 ? shorter.substr(0, 24) : shorter;
            const bool anyMatch = longer.find(key) != std::string::npos;
            CHECK_MESSAGE(anyMatch, ("Rule #" + std::to_string(n) +
                                     " short-reason drift between markdown table and C++ classifier")
                                        .c_str());
        }
    }
}
