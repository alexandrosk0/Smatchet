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

} // namespace pure
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_AI_XML_ATTR_ESCAPE_H
