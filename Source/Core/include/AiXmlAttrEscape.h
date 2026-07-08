#ifndef SMATCHET_AI_XML_ATTR_ESCAPE_H
#define SMATCHET_AI_XML_ATTR_ESCAPE_H

#include <string>

namespace smatchet {
namespace ai {
namespace pure {

/// XML-attribute-escape a context-block name before it is interpolated into the
/// `<smatchet_context block="...">` wrapper. A `"`/`&`/`<`/`>` in a Lua-supplied or
/// future dynamic block name would otherwise corrupt the wrapper or inject markup
/// (#826). `&` is replaced first so the entity-introducing ampersands emitted for the
/// other cases are not themselves re-escaped. `inline` so this header can be included
/// without dragging in the AiAssistantController TU's AppController dependency — the
/// same link-without-the-controller-TU rationale as `ComposeSystemPrompt`. Extracted
/// to a standalone pure header so the doctest rig can exercise it without the heavy
/// controller header chain (cpr / AppController / Ui session).
inline std::string EscapeXmlAttr(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (std::string::const_iterator it = value.begin(); it != value.end(); ++it) {
        const char c = *it;
        switch (c) {
        case '&':
            out.append("&amp;");
            break;
        case '"':
            out.append("&quot;");
            break;
        case '<':
            out.append("&lt;");
            break;
        case '>':
            out.append("&gt;");
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

/// Neutralize `<smatchet_context` / `</smatchet_context` sequences inside a context-block
/// *body* before it is wrapped in the real `<smatchet_context block="...">` tags. Block
/// bodies (ticket summaries, labels, audit-trail strings, visible grid rows) are
/// attacker-influenceable via the tracker backend — a body containing the closing tag
/// would otherwise break out of its wrapper and smuggle instructions into the system
/// prompt. The leading `<` of each matched sequence becomes `&lt;`, which preserves the
/// text for the model while making a tag-boundary parse impossible. Everything else is
/// copied verbatim. `inline` for the same link-without-the-controller-TU rationale as
/// `EscapeXmlAttr` above.
inline std::string NeutralizeContextBody(const std::string& body) {
    static const char kTagStem[] = "smatchet_context";
    static const std::string::size_type kStemLen = sizeof(kTagStem) - 1;
    std::string out;
    out.reserve(body.size());
    for (std::string::size_type pos = 0; pos < body.size(); ++pos) {
        if (body[pos] == '<') {
            std::string::size_type stemPos = pos + 1;
            if (stemPos < body.size() && body[stemPos] == '/') {
                ++stemPos;
            }
            if (body.compare(stemPos, kStemLen, kTagStem) == 0) {
                out.append("&lt;");
                continue;
            }
        }
        out.push_back(body[pos]);
    }
    return out;
}

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_XML_ATTR_ESCAPE_H
