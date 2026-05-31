// Full Font-Awesome 6 icon catalog (name slug + glyph) and the name->glyph lookup.
// Deliberately ImGui-free so the table is unit-testable on its own; the searchable
// picker grid lives in SmatchetIconPickerUi.cpp. Rows are generated from the FA
// header by scripts/dev/gen-fa-catalog.py.

#include "SmatchetIconPickerUi.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "IconsFontAwesome6.h"

namespace {

const std::vector<SmatchetToolbarIconEntry>& Catalog() {
    static const std::vector<SmatchetToolbarIconEntry> kCatalog = {
#include "IconsFontAwesome6_Catalog.inl"
    };
    return kCatalog;
}

}  // namespace

const std::vector<SmatchetToolbarIconEntry>& SmatchetToolbarIconCatalog() { return Catalog(); }

std::string SmatchetToolbarIconGlyph(const std::string& name) {
    if (name.empty()) {
        return std::string();
    }
    // Magic-static: the map is built exactly once, thread-safe under C++11+ static-init
    // rules, so concurrent callers never observe a half-filled index.
    static const std::unordered_map<std::string, std::string> index = []() {
        std::unordered_map<std::string, std::string> m;
        for (const SmatchetToolbarIconEntry& e : Catalog()) {
            m[e.Name] = e.Glyph;
        }
        return m;
    }();
    const std::unordered_map<std::string, std::string>::const_iterator it = index.find(name);
    return it == index.end() ? std::string() : it->second;
}
