// Bucket-A doctest for DR11: the LuaConsole script selection is tracked by
// path/name identity rather than by list index, so a background RefreshScriptList
// re-sort (every ~0.35s) can never retarget a save at an unrelated file.
//
// Exercises the pure, ImGui-free resolver
// `lua_console_detail::ResolveSelectedScriptIndex` in isolation, mirroring the
// three invariants from the plugin's `SyncSelectionToList`:
//   - selection survives insert/delete/reorder (index re-derived from the name);
//   - a missing tracked name resolves to -1 (caller clears, never clobbers);
//   - an empty selection resolves to -1.
//
// Anchors: Source/Plugins/LuaConsole/LuaConsolePlugin_detail.h,
//          Source/Plugins/LuaConsole/LuaConsolePlugin.cpp (SaveCurrentScript).

#include "LuaConsolePlugin_detail.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

using lua_console_detail::ResolveSelectedScriptIndex;

// Mirror of LuaConsolePlugin::SyncSelectionToList so the test asserts the full
// caller policy (default-to-first vs clear) that guards against DR11, not just
// the resolver in isolation.
void SyncSelection(const std::vector<std::string>& list, std::string& name, int& index) {
    const int resolved = ResolveSelectedScriptIndex(list, name);
    if (resolved >= 0) {
        index = resolved;
        return;
    }
    if (name.empty() && !list.empty()) {
        index = 0;
        name = list.front();
        return;
    }
    index = -1;
    name.clear();
}

} // namespace

TEST_CASE("ResolveSelectedScriptIndex finds the tracked name") {
    const std::vector<std::string> list = {"Automation.lua", "RunLua.lua", "SmatchetHooks.lua"};
    CHECK(ResolveSelectedScriptIndex(list, "Automation.lua") == 0);
    CHECK(ResolveSelectedScriptIndex(list, "RunLua.lua") == 1);
    CHECK(ResolveSelectedScriptIndex(list, "SmatchetHooks.lua") == 2);
}

TEST_CASE("ResolveSelectedScriptIndex returns -1 for empty or missing names") {
    const std::vector<std::string> list = {"a.lua", "b.lua"};
    CHECK(ResolveSelectedScriptIndex(list, "") == -1);
    CHECK(ResolveSelectedScriptIndex(list, "gone.lua") == -1);
    CHECK(ResolveSelectedScriptIndex({}, "a.lua") == -1);
}

TEST_CASE("DR11: selection index follows the tracked file across an insert re-sort") {
    // User is editing "m.lua" (index 1). A new file "b.lua" appears and the list
    // re-sorts, shifting "m.lua" to index 2. An index-based selection would now
    // point at "z.lua" and a save would clobber it; name tracking re-resolves.
    std::string name = "m.lua";
    int index = 1;
    std::vector<std::string> list = {"a.lua", "m.lua", "z.lua"};
    SyncSelection(list, name, index);
    REQUIRE(index == 1);

    list = {"a.lua", "b.lua", "m.lua", "z.lua"}; // re-sorted after insert
    SyncSelection(list, name, index);
    CHECK(name == "m.lua"); // identity preserved
    CHECK(index == 2);      // index re-derived, not stale
}

TEST_CASE("DR11: selection index follows the tracked file across a delete re-sort") {
    std::string name = "m.lua";
    int index = 2;
    std::vector<std::string> list = {"a.lua", "b.lua", "m.lua", "z.lua"};
    SyncSelection(list, name, index);
    REQUIRE(index == 2);

    list = {"a.lua", "m.lua", "z.lua"}; // "b.lua" removed, list re-sorted
    SyncSelection(list, name, index);
    CHECK(name == "m.lua");
    CHECK(index == 1);
}

TEST_CASE("DR11: a vanished tracked file clears the selection instead of clobbering") {
    // The edited file disappears from the list. The selection must clear (index
    // -1, empty name) so a subsequent save has no target and cannot overwrite the
    // unrelated file that now sits at the old index.
    std::string name = "m.lua";
    int index = 1;
    std::vector<std::string> list = {"a.lua", "z.lua"}; // "m.lua" gone
    SyncSelection(list, name, index);
    CHECK(index == -1);
    CHECK(name.empty());
}

TEST_CASE("DR11: an unset selection defaults to the first entry, not a clear") {
    std::string name; // never selected
    int index = -1;
    const std::vector<std::string> list = {"a.lua", "b.lua"};
    SyncSelection(list, name, index);
    CHECK(index == 0);
    CHECK(name == "a.lua");
}

// --- #2042: buffer provenance, the hole DR11's identity tracking does NOT cover ----------------
//
// DR11 anchored the SELECTION to a file across a background re-sort. #2042 is the complementary
// gap: the editor BUFFER can stop corresponding to the selection entirely. When an async script
// read is dropped by PollScriptLoad's name-mismatch bail, the editor is left holding the blank
// placeholder with diskSnapshot_ re-baselined onto it, no read in flight and no refusal latched —
// and a later SyncSelectionToList auto-selects an unrelated script, so Save truncates a real file.
//
// These exercise the production predicate LuaConsolePlugin::SaveCurrentScript calls, not a copy of
// it. LuaConsolePlugin.cpp is ImGui-bound and outside the Linux compile lane, which is exactly why
// the decision was extracted into the pure header.

using lua_console_detail::EditorBufferMatchesSelection;

TEST_CASE("#2042: a buffer loaded for the selected script may be saved") {
    CHECK(EditorBufferMatchesSelection("m.lua", "m.lua"));
}

TEST_CASE("#2042: the blank in-flight placeholder is never saveable") {
    // StartLoadSelectedScriptIntoEditor blanks the editor and clears the provenance before kicking
    // the read; until PollScriptLoad lands a body, no write may be issued.
    CHECK_FALSE(EditorBufferMatchesSelection("", "m.lua"));
}

TEST_CASE("#2042: the reported sequence — dropped read then auto-selected script — cannot save") {
    // 1. Select A -> read kicked, editor blanked, provenance cleared.
    std::string editorContentName;
    std::string selected = "A.lua";
    CHECK_FALSE(EditorBufferMatchesSelection(editorContentName, selected));

    // 2/3. A vanishes from disk; SyncSelectionToList clears the selection and the in-flight result
    //      for A is dropped by the name-mismatch bail (provenance stays empty).
    selected.clear();
    CHECK_FALSE(EditorBufferMatchesSelection(editorContentName, selected));

    // 4. A later tick auto-selects the first script, B — an unrelated, intact file.
    selected = "B.lua";
    // 5. Save must refuse: the blank buffer never loaded B. This is the truncation in the issue.
    CHECK_FALSE(EditorBufferMatchesSelection(editorContentName, selected));

    // Reselecting B kicks a read that actually lands, which arms the buffer for B and only B.
    editorContentName = "B.lua";
    CHECK(EditorBufferMatchesSelection(editorContentName, selected));
}

TEST_CASE("#2042: a stale buffer is never written to a different selection") {
    // The editor still holds A's text while the selection has moved to B (a retarget that raced the
    // editor). Writing here would overwrite B with A's contents — the DR11 failure mode arriving
    // through the buffer rather than through the index.
    CHECK_FALSE(EditorBufferMatchesSelection("A.lua", "B.lua"));
}

TEST_CASE("#2042: an empty selection is never a save target, whatever the buffer holds") {
    CHECK_FALSE(EditorBufferMatchesSelection("A.lua", ""));
    CHECK_FALSE(EditorBufferMatchesSelection("", ""));
}
