#include "TicketFieldEditorLongTextPure.h"

#include "MarkdownConvert.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace TicketFieldEditorLongTextPure {

LongTextRichKind ClassifyRichValue(const std::string& rich) {
    if (rich.empty())
        return LongTextRichKind::None;
    size_t i = 0;
    while (i < rich.size() && (rich[i] == ' ' || rich[i] == '\t' || rich[i] == '\n' || rich[i] == '\r'))
        ++i;
    if (i >= rich.size())
        return LongTextRichKind::None;
    if (rich[i] == '{') {
        try {
            auto parsed = nlohmann::json::parse(rich, nullptr, false);
            if (parsed.is_object() && parsed.value("type", std::string()) == "doc") {
                return LongTextRichKind::Adf;
            }
        } catch (...) {
        }
    }
    if (rich[i] == '<') {
        return LongTextRichKind::Html;
    }
    return LongTextRichKind::None;
}

std::string ComputeLongTextSeed(LongTextRichKind kind, const std::string& rich, const std::string& strippedFallback,
                                std::vector<std::string>& outDroppedAdfNodeTypes, bool& outRawMode) {
    outRawMode = false;
    outDroppedAdfNodeTypes.clear();
    switch (kind) {
    case LongTextRichKind::Adf: {
        try {
            const auto adf = nlohmann::json::parse(rich);
            return MarkdownConvert::AdfToMarkdown(adf, &outDroppedAdfNodeTypes);
        } catch (...) {
            return strippedFallback;
        }
    }
    case LongTextRichKind::Html: {
        bool fellBack = false;
        std::string seed = MarkdownConvert::HtmlSubsetToMarkdown(rich, &fellBack);
        if (fellBack) {
            outRawMode = true;
            return rich;
        }
        return seed;
    }
    case LongTextRichKind::None:
    default:
        return strippedFallback;
    }
}

} // namespace TicketFieldEditorLongTextPure
