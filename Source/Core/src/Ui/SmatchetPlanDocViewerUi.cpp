// Plan-doc viewer — see SmatchetPlanDocViewerUi.h for the surface contract.
// Slice 4/5 follow-up: this viewer now uses the rich MarkdownPreviewRender
// path (md4c → ImGui draw calls with heading scaling, BoldItalic font,
// fenced-code BeginChild with CppSyntaxHighlight, clickable links) wrapped in
// a SelectableText region for drag-select + Ctrl+A + Ctrl+C + right-click
// Copy. The original TextEditor + Markdown LD implementation (slice 2) was
// retired here so the viewer's visual style matches the AI chat + ticket
// description preview surfaces.

#include "SmatchetPlanDocViewerUi.h"

#include "AsyncLoadGatePure.h"
#include "Logger.h"
#include "MarkdownPreviewRender.h"
#include "SmatchetDockNodeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace fs = ghc::filesystem;

namespace smatchet {

namespace {

constexpr std::size_t kMaxDocBytes = 1024 * 1024; // 1 MiB — plan docs are ~ tens of KiB

// Walk up from `start` looking for a directory that contains `docs/plans/active`.
// Returns the directory containing that path on hit; empty path on miss.
fs::path FindRepoRoot(const fs::path& start, std::size_t maxDepth = 8) {
    std::error_code ec;
    fs::path dir = start;
    if (dir.is_relative()) {
        dir = fs::absolute(dir, ec);
        if (ec) {
            return fs::path();
        }
    }
    for (std::size_t depth = 0; depth <= maxDepth; ++depth) {
        fs::path candidate = dir / "docs" / "design";
        std::error_code statEc;
        if (fs::exists(candidate, statEc) && !statEc && fs::is_directory(candidate, statEc)) {
            return dir;
        }
        if (!dir.has_parent_path()) {
            break;
        }
        fs::path parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
    return fs::path();
}

// Enumerate `*.md` files directly under `dir` (no recursion). Returns
// generic-form paths sorted alphabetically.
std::vector<std::string> ListMarkdownFiles(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return out;
    }
    fs::directory_iterator it(dir, ec);
    if (ec) {
        return out;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& entry = *it;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || fileEc) {
            continue;
        }
        const fs::path& p = entry.path();
        const std::string ext = p.extension().generic_string();
        if (ext == ".md" || ext == ".MD") {
            out.push_back(p.generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Read up to `cap` bytes from `path`. Returns the loaded content; sets `oversize`
// to true if the file was larger than `cap` (extra bytes truncated).
//
// Pillar 2: runs on the std::async worker owned by ViewerState::loadFuture, never on
// the UI thread. The 1 MiB cap bounds the bytes, but nothing bounds the LATENCY of a
// single open/read on a network share or an antivirus-filtered path — which is why the
// call had to leave the render thread even though the sizes are small.
std::string ReadCapped(const std::string& path, std::size_t cap, bool& oversize) {
    oversize = false;
    /* PILLAR2_WORKER_ONLY */ // est-latency: ~5ms — <=1 MiB read on the std::async worker owned by
                              // ViewerState::loadFuture; no frame budget applies.
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::string();
    }
    std::string out;
    out.resize(cap + 1);
    in.read(&out[0], static_cast<std::streamsize>(out.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
        out.clear();
        return out;
    }
    out.resize(static_cast<std::size_t>(got));
    if (out.size() > cap) {
        out.resize(cap);
        oversize = true;
    }
    return out;
}

struct IndexResult {
    fs::path repoRoot;
    std::vector<std::string> files;
};

// Worker payload for one document load. `path` is echoed back so the poll can
// discard a result whose selection moved on while the read was in flight.
struct LoadResult {
    std::string path;
    std::string body;
    bool oversize = false;
};

struct ViewerState {
    bool indexed = false; // file list scanned at least once
    bool indexInFlight = false;
    fs::path repoRoot;
    std::vector<std::string> files; // absolute generic paths, sorted
    int selectedIdx = -1;
    std::string loadedPath; // path matching the cached body
    std::string body;       // cached markdown source for the active doc
    std::future<IndexResult> indexFuture;
    bool loadInFlight = false; // a document read is running on the worker
    std::future<LoadResult> loadFuture;
};

ViewerState& State() {
    static ViewerState s;
    return s;
}

IndexResult BuildIndex() {
    IndexResult out;
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) {
        return out;
    }
    out.repoRoot = FindRepoRoot(cwd);
    if (out.repoRoot.empty()) {
        return out;
    }
    const fs::path designDir = out.repoRoot / "docs" / "design";
    const fs::path adrDir = out.repoRoot / "docs" / "adr";
    std::vector<std::string> designFiles = ListMarkdownFiles(designDir);
    std::vector<std::string> adrFiles = ListMarkdownFiles(adrDir);
    out.files.reserve(designFiles.size() + adrFiles.size());
    for (std::size_t i = 0; i < designFiles.size(); ++i) {
        out.files.push_back(designFiles[i]);
    }
    for (std::size_t i = 0; i < adrFiles.size(); ++i) {
        out.files.push_back(adrFiles[i]);
    }
    return out;
}

void StartRescanIndex(ViewerState& s) {
    if (s.indexInFlight) {
        return;
    }
    s.indexed = false;
    s.indexInFlight = true;
    s.files.clear();
    s.selectedIdx = -1;
    s.loadedPath.clear();
    s.body.clear();
    // An in-flight document read is left to land on its own — PollLoadResult discards it
    // because its path no longer matches the (now empty) selection. Re-assigning
    // `loadFuture` here instead would block the frame releasing the old shared state.
    s.indexFuture = std::async(std::launch::async, []() { return BuildIndex(); });
}

void PollIndexResult(ViewerState& s) {
    if (!s.indexInFlight || !s.indexFuture.valid()) {
        return;
    }
    if (s.indexFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    IndexResult result = s.indexFuture.get();
    s.repoRoot = std::move(result.repoRoot);
    s.files = std::move(result.files);
    s.indexInFlight = false;
    s.indexed = true;
    if (s.selectedIdx < 0 && !s.files.empty()) {
        s.selectedIdx = 0; // the per-frame StartLoadSelected kick picks this up
    }
}

// Path currently selected in the combo, or empty when the selection is out of range.
std::string SelectedPath(const ViewerState& s) {
    if (s.selectedIdx < 0 || s.selectedIdx >= static_cast<int>(s.files.size())) {
        return std::string();
    }
    return s.files[static_cast<std::size_t>(s.selectedIdx)];
}

// Pillar 2: kick the document read onto a worker. Called unconditionally once per frame —
// it no-ops unless the selection has moved off the cached body and nothing is in flight.
// A selection change made WHILE a read is running is not dropped: PollLoadResult re-kicks
// after discarding the stale result.
void StartLoadSelected(ViewerState& s) {
    const std::string path = SelectedPath(s);
    if (!async_load::ShouldKickLoad(s.loadInFlight, path, s.loadedPath)) {
        return;
    }
    s.loadInFlight = true;
    s.loadFuture = std::async(std::launch::async, [path]() {
        LoadResult result;
        result.path = path;
        result.body = ReadCapped(path, kMaxDocBytes, result.oversize);
        return result;
    });
}

void PollLoadResult(ViewerState& s) {
    if (!s.loadInFlight || !s.loadFuture.valid()) {
        return;
    }
    if (s.loadFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }
    LoadResult result = s.loadFuture.get();
    s.loadInFlight = false;
    if (!async_load::ResultIsCurrent(result.path, SelectedPath(s))) {
        // Selection moved (or the index was rescanned) mid-read — drop this body and let
        // the caller's re-kick load whatever is selected now.
        StartLoadSelected(s);
        return;
    }
    std::string body = std::move(result.body);
    if (body.empty()) {
        // P2-L12: an unreadable (or genuinely empty) file used to render a silently
        // blank body — indistinguishable from a working viewer on an empty doc.
        body = "[Couldn't read this file (it may have moved, or be empty): " + result.path + "]";
        LOG_WARN("plan-doc viewer: empty/unreadable file %s", result.path.c_str());
    }
    if (result.oversize) {
        body.append("\n\n---\n[truncated at 1 MiB — open the file directly to view the full content]\n");
        LOG_WARN("plan-doc viewer: truncated oversized file %s", result.path.c_str());
    }
    s.body = std::move(body);
    s.loadedPath = result.path;
}

// Derive a display label from an absolute path:
//   "C:/dev/Smatchet/docs/plans/active/foo.md" -> "design/foo.md"
//   "C:/dev/Smatchet/docs/adr/0001-bar.md" -> "adr/0001-bar.md"
std::string DisplayLabel(const fs::path& repoRoot, const std::string& absPath) {
    const std::string root = repoRoot.generic_string();
    if (!root.empty() && absPath.size() > root.size() && absPath.compare(0, root.size(), root) == 0) {
        std::size_t off = root.size();
        if (absPath[off] == '/') {
            ++off;
        }
        const std::string rel = absPath.substr(off);
        const std::string docsPrefix = "docs/";
        if (rel.compare(0, docsPrefix.size(), docsPrefix) == 0) {
            return rel.substr(docsPrefix.size());
        }
        return rel;
    }
    return absPath;
}

} // namespace

void DrawPlanDocViewer(UiDrawSession& d) {
    if (!d.showPlanDocViewer) {
        return;
    }

    ViewerState& s = State();
    if (!s.indexed && !s.indexInFlight) {
        StartRescanIndex(s);
    }
    PollIndexResult(s);
    PollLoadResult(s);
    StartLoadSelected(s);

    if (!ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        ImGui::SetNextWindowDockID(SmatchetDockNodeIds::kBottomPanel, ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f, 540.0f), ImGuiCond_FirstUseEver);
    const bool wantFocus = d.requestPlanDocViewerFocus;
    if (wantFocus) {
        // SetNextWindowFocus BEFORE Begin is the only path that activates a docked tab.
        // Post-Begin SetWindowFocus below is belt-and-braces for floating state.
        ImGui::SetNextWindowFocus();
    }
    bool open = d.showPlanDocViewer;
    if (!ImGui::Begin("Plan Docs", &open)) {
        d.showPlanDocViewer = open;
        if (wantFocus) {
            d.requestPlanDocViewerFocus = false;
        }
        ImGui::End();
        return;
    }
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestPlanDocViewerFocus = false;
        LOG_DEBUG("Plan docs window: focused via menu request");
    }

    if (s.indexInFlight) {
        ImGui::TextDisabled("Scanning plan docs...");
    } else if (s.files.empty()) {
        ImGui::TextDisabled("No plan docs found under docs/design or docs/adr."); // P2-L12: the dirs actually scanned
        if (ImGui::Button("Rescan")) {
            StartRescanIndex(s);
        }
    } else {
        const int curIdx = (s.selectedIdx >= 0 && s.selectedIdx < static_cast<int>(s.files.size())) ? s.selectedIdx : 0;
        const std::string curLabel = DisplayLabel(s.repoRoot, s.files[static_cast<std::size_t>(curIdx)]);
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::BeginCombo("##plan_doc_picker", curLabel.c_str())) {
            for (std::size_t i = 0; i < s.files.size(); ++i) {
                const std::string label = DisplayLabel(s.repoRoot, s.files[i]);
                const bool isSel = (static_cast<int>(i) == curIdx);
                if (ImGui::Selectable(label.c_str(), isSel)) {
                    // The read is kicked from the per-frame StartLoadSelected at the top of
                    // Draw — never inline here (Pillar 2: no file I/O on the render thread).
                    s.selectedIdx = static_cast<int>(i);
                }
                if (isSel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) {
            StartRescanIndex(s);
        }
        if (s.loadInFlight) {
            // Visible cue for the off-thread read (Pillar 2 cue contract — see Source/Core/src/Ui/AGENTS.md).
            ImGui::SameLine();
            ImGui::TextDisabled("Loading...");
        }
    }
    ImGui::Separator();

    // Rich render + selection overlay. Scrolling lives on the inner BeginChild
    // so the body can be longer than the window; HorizontalScrollbar handles
    // wide fenced-code blocks. selectableId opens a fresh SelectableText
    // Context per frame inside the render — the doc is one logical surface so
    // one Context per render is right.
    ImGui::BeginChild("##plan_doc_body", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    // Draw the cached body only while it still describes the selection. `body` outlives a
    // selection change until the worker's read lands, so an unguarded render would show the
    // previous document's contents under the newly picked document's name; the pane goes blank
    // behind the "Loading..." cue instead.
    if (!s.body.empty() && async_load::ResultIsCurrent(s.loadedPath, SelectedPath(s))) {
        MarkdownPreviewRender::Options opts;
        opts.mode = MarkdownPreviewRender::Mode::Full;
        opts.clickableLinks = true;
        opts.selectableId = "##PlanDocViewerSel";
        MarkdownPreviewRender::Render(s.body, opts);
    }
    ImGui::EndChild();

    ImGui::End();
    d.showPlanDocViewer = open;
}

} // namespace smatchet
