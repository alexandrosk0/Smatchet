// Plan-doc viewer — see SmatchetPlanDocViewerUi.h for the surface contract.
//
// Slice 4/5 follow-up: this viewer now uses the rich MarkdownPreviewRender
// path (md4c → ImGui draw calls with heading scaling, BoldItalic font,
// fenced-code BeginChild with CppSyntaxHighlight, clickable links) wrapped in
// a SelectableText region for drag-select + Ctrl+A + Ctrl+C + right-click
// Copy. The original TextEditor + Markdown LD implementation (slice 2) was
// retired here so the viewer's visual style matches the AI chat + ticket
// description preview surfaces.

#include "SmatchetPlanDocViewerUi.h"

#include "Logger.h"
#include "MarkdownPreviewRender.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <ghc/filesystem.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace fs = ghc::filesystem;

namespace smatchet {

namespace {

constexpr std::size_t kMaxDocBytes = 1024 * 1024; // 1 MiB — plan docs are ~ tens of KiB

// Walk up from `start` looking for a directory that contains `docs/design`.
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
    for (const auto& entry : it) {
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
std::string ReadCapped(const std::string& path, std::size_t cap, bool& oversize) {
    oversize = false;
    // TODO(pillar2): bug-2026-05-20-ui-sync-reads — UI-thread plan-doc read on combo-change.
    // 1 MiB cap, local disk, typically sub-ms. Move to worker + dispatcher post-back when bandwidth allows.
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

struct ViewerState {
    bool indexed = false; // file list scanned at least once
    fs::path repoRoot;
    std::vector<std::string> files; // absolute generic paths, sorted
    int selectedIdx = -1;
    std::string loadedPath; // path matching the cached body
    std::string body;       // cached markdown source for the active doc
};

ViewerState& State() {
    static ViewerState s;
    return s;
}

void RescanIndex(ViewerState& s) {
    s.files.clear();
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) {
        return;
    }
    s.repoRoot = FindRepoRoot(cwd);
    if (s.repoRoot.empty()) {
        return;
    }
    const fs::path designDir = s.repoRoot / "docs" / "design";
    const fs::path adrDir = s.repoRoot / "docs" / "adr";
    std::vector<std::string> designFiles = ListMarkdownFiles(designDir);
    std::vector<std::string> adrFiles = ListMarkdownFiles(adrDir);
    s.files.reserve(designFiles.size() + adrFiles.size());
    for (std::size_t i = 0; i < designFiles.size(); ++i) {
        s.files.push_back(designFiles[i]);
    }
    for (std::size_t i = 0; i < adrFiles.size(); ++i) {
        s.files.push_back(adrFiles[i]);
    }
}

void LoadSelected(ViewerState& s) {
    if (s.selectedIdx < 0 || s.selectedIdx >= static_cast<int>(s.files.size())) {
        return;
    }
    const std::string& path = s.files[static_cast<std::size_t>(s.selectedIdx)];
    if (path == s.loadedPath) {
        return;
    }
    bool oversize = false;
    std::string body = ReadCapped(path, kMaxDocBytes, oversize);
    if (oversize) {
        body.append("\n\n---\n[truncated at 1 MiB — open the file directly to view the full content]\n");
        LOG_WARN("plan-doc viewer: truncated oversized file %s", path.c_str());
    }
    s.body = std::move(body);
    s.loadedPath = path;
}

// Derive a display label from an absolute path:
//   "C:/dev/Smatchet/docs/design/foo.md" -> "design/foo.md"
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
    if (!s.indexed) {
        RescanIndex(s);
        s.indexed = true;
        if (s.selectedIdx < 0 && !s.files.empty()) {
            s.selectedIdx = 0;
            LoadSelected(s);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(720.0f, 540.0f), ImGuiCond_FirstUseEver);
    bool open = d.showPlanDocViewer;
    if (!ImGui::Begin("Plan docs", &open)) {
        d.showPlanDocViewer = open;
        ImGui::End();
        return;
    }

    if (s.files.empty()) {
        ImGui::TextDisabled("No plan docs found under docs/design or docs/adr.");
        if (ImGui::Button("Rescan")) {
            s.indexed = false;
            s.selectedIdx = -1;
            s.loadedPath.clear();
            s.body.clear();
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
                    s.selectedIdx = static_cast<int>(i);
                    LoadSelected(s);
                }
                if (isSel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) {
            s.indexed = false;
            s.loadedPath.clear();
            s.body.clear();
        }
    }
    ImGui::Separator();

    // Rich render + selection overlay. Scrolling lives on the inner BeginChild
    // so the body can be longer than the window; HorizontalScrollbar handles
    // wide fenced-code blocks. selectableId opens a fresh SelectableText
    // Context per frame inside the render — the doc is one logical surface so
    // one Context per render is right.
    ImGui::BeginChild("##plan_doc_body", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    if (!s.body.empty()) {
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
