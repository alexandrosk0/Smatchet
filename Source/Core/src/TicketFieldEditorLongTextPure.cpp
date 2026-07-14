#include "TicketFieldEditorLongTextPure.h"

#include "MarkdownConvert.h"

#include "Json/BoundedJsonParse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
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
        std::string parseErr;
        const nlohmann::json parsed = smatchet::json_safe::ParseBounded(rich, parseErr);
        if (parseErr.empty() && parsed.is_object() && parsed.value("type", std::string()) == "doc") {
            return LongTextRichKind::Adf;
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
            std::string parseErr;
            const nlohmann::json adf = smatchet::json_safe::ParseBounded(rich, parseErr);
            if (!parseErr.empty()) {
                return strippedFallback;
            }
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

RoundTripPreview ComputeRoundTripPreview(LongTextRichKind kind, const std::string& markdown) {
    RoundTripPreview result;
    try {
        if (kind == LongTextRichKind::Html) {
            const std::string html = MarkdownConvert::MarkdownToHtml(markdown);
            bool fellBack = false;
            result.Rendered = MarkdownConvert::HtmlSubsetToMarkdown(html, &fellBack);
            result.Lossy = fellBack;
        } else {
            // Default to ADF for None / Adf — covers Jira description and generic ADF
            // fields. New issues without a stored rich value still go through ADF.
            const nlohmann::json adf = MarkdownConvert::MarkdownToAdf(markdown);
            std::vector<std::string> dropped;
            result.Rendered = MarkdownConvert::AdfToMarkdown(adf, &dropped);
            result.Lossy = !dropped.empty();
        }
    } catch (...) {
        // Converter blew up on the in-progress edit (rare; usually mid-token).
        // Fall back to the raw buffer so the preview keeps updating, but flag the failure so a
        // save on genuinely unconvertible content is gated behind acknowledgement.
        result.Rendered = markdown;
        result.Lossy = false;
        result.ConversionFailed = true;
    }
    return result;
}

std::string RichValueToTooltipMarkdown(const std::string& rich, const std::string& strippedFallback) {
    if (rich.empty()) {
        return strippedFallback;
    }
    std::vector<std::string> dropped;
    bool rawMode = false;
    const LongTextRichKind kind = ClassifyRichValue(rich);
    std::string md = ComputeLongTextSeed(kind, rich, strippedFallback, dropped, rawMode);
    if (rawMode || md.empty()) {
        return strippedFallback;
    }
    return md;
}

LongTextSeedPlan PlanSeedCopy(const std::string& seed, std::size_t bufferCapacity) {
    LongTextSeedPlan plan;
    if (bufferCapacity == 0) {
        return plan;
    }
    const std::size_t copyLen = (std::min)(seed.size(), bufferCapacity - 1);
    plan.Shown.assign(seed.data(), copyLen);
    plan.Truncated = seed.size() > copyLen;
    return plan;
}

bool ShouldQueueLongTextEdit(const std::string& newValue, const std::string& shownSeed) {
    return newValue != shownSeed;
}

LongTextSaveFidelity AssessLongTextSaveFidelity(bool rawMode, const std::vector<std::string>& droppedAdfNodeTypes,
                                                bool roundTripLossy, bool seedTruncated, bool conversionFailed) {
    LongTextSaveFidelity out;
    std::vector<std::string> reasons;
    if (conversionFailed) {
        out.RequireAck = true;
        reasons.push_back("the document could not be converted for saving (its formatting may not be preserved)");
    }
    if (seedTruncated) {
        out.RequireAck = true;
        reasons.push_back("the document exceeds the editor buffer and the text beyond it was not loaded");
    }
    if (!droppedAdfNodeTypes.empty()) {
        out.RequireAck = true;
        std::string list;
        for (std::size_t i = 0; i < droppedAdfNodeTypes.size() && i < 5; ++i) {
            if (!list.empty())
                list += ", ";
            list += droppedAdfNodeTypes[i];
        }
        if (droppedAdfNodeTypes.size() > 5)
            list += ", ...";
        reasons.push_back("constructs not in the Markdown subset will be dropped (" + list + ")");
    }
    if (roundTripLossy) {
        out.RequireAck = true;
        reasons.push_back("some formatting will change in the round-trip conversion");
    }
    if (rawMode) {
        // Raw HTML is stored verbatim (best-effort, no structured validation) — warn, never block.
        reasons.push_back("raw HTML is saved verbatim without validation");
    }
    out.LossPossible = !reasons.empty();
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        if (i != 0)
            out.ToastSummary += "; ";
        out.ToastSummary += reasons[i];
    }
    return out;
}

} // namespace TicketFieldEditorLongTextPure
