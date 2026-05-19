#include "AiChatMarkdownRender.h"

extern "C" {
#include "md4c.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace AiChatMarkdownRender {

namespace {

// Walker state for `md_parse`. md4c reports block / span enter/leave events
// in document order. We accumulate the current block's flattened text in
// `current.Text` while inline spans run, then flush a finalised Block into
// `blocks` on block-leave.
struct WalkState {
    std::vector<Block> blocks;

    // Current in-progress block. `Kind` is set on block-enter; `Text` is
    // appended to during span/text callbacks; the whole struct is pushed
    // into `blocks` on block-leave.
    Block current;

    // Stack-tracked nesting for OL/UL — we emit one ListItem block per <li>,
    // so we need to remember the current ordered-counter and bullet kind
    // across nested lists.
    struct ListFrame {
        bool ordered;
        int counter; // 1-based; valid only when ordered
    };
    std::vector<ListFrame> listStack;

    // Table cell separator: md4c reports MD_BLOCK_TH/TD enter/leave; we
    // append " | " between cells and "\n" between rows. The leading " | "
    // is suppressed on the first cell of each row.
    bool tableRowFirstCell = true;

    // Link href stash — set on MD_SPAN_A enter, consumed on leave.
    std::string linkHref;

    // Block-quote: paragraphs inside a quote get absorbed into one Quote
    // block so the user sees one selectable widget for the whole quote.
    int quoteDepth = 0;
    bool quoteLineStart = true;
};

void AppendBytes(std::string& dst, const MD_CHAR* text, MD_SIZE size) { dst.append(text, text + size); }

void FlushBlock(WalkState& s) {
    if (s.current.Text.empty() && s.current.Kind != BlockKind::CodeFence) {
        s.current = Block();
        return;
    }
    while (!s.current.Text.empty() && s.current.Text.back() == '\n') {
        s.current.Text.pop_back();
    }
    s.blocks.push_back(std::move(s.current));
    s.current = Block();
}

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    WalkState& s = *static_cast<WalkState*>(userdata);
    switch (type) {
    case MD_BLOCK_H: {
        const MD_BLOCK_H_DETAIL* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
        s.current.Kind = BlockKind::Heading;
        s.current.HeadingLevel = static_cast<int>(d->level);
        for (int i = 0; i < s.current.HeadingLevel; ++i) {
            s.current.Text.push_back('#');
        }
        s.current.Text.push_back(' ');
        break;
    }
    case MD_BLOCK_P: {
        if (s.quoteDepth > 0) {
            if (!s.current.Text.empty() && s.current.Text.back() != '\n') {
                s.current.Text.push_back('\n');
            }
            s.current.Text.append("> ");
            s.quoteLineStart = false;
        } else {
            s.current.Kind = BlockKind::Paragraph;
        }
        break;
    }
    case MD_BLOCK_CODE: {
        const MD_BLOCK_CODE_DETAIL* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
        s.current.Kind = BlockKind::CodeFence;
        s.current.UseMonospace = true;
        if (d && d->lang.text && d->lang.size > 0) {
            s.current.LangTag.assign(d->lang.text, d->lang.text + d->lang.size);
        }
        break;
    }
    case MD_BLOCK_UL: {
        WalkState::ListFrame f;
        f.ordered = false;
        f.counter = 0;
        s.listStack.push_back(f);
        break;
    }
    case MD_BLOCK_OL: {
        const MD_BLOCK_OL_DETAIL* d = static_cast<const MD_BLOCK_OL_DETAIL*>(detail);
        WalkState::ListFrame f;
        f.ordered = true;
        f.counter = static_cast<int>(d ? d->start : 1);
        s.listStack.push_back(f);
        break;
    }
    case MD_BLOCK_LI: {
        s.current.Kind = BlockKind::ListItem;
        if (!s.listStack.empty()) {
            WalkState::ListFrame& f = s.listStack.back();
            if (f.ordered) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d. ", f.counter);
                s.current.Text.append(buf);
                ++f.counter;
            } else {
                s.current.Text.append("- ");
            }
        } else {
            s.current.Text.append("- ");
        }
        break;
    }
    case MD_BLOCK_QUOTE: {
        ++s.quoteDepth;
        s.current.Kind = BlockKind::Quote;
        s.quoteLineStart = true;
        break;
    }
    case MD_BLOCK_TABLE: {
        s.current.Kind = BlockKind::Table;
        s.current.UseMonospace = true;
        break;
    }
    case MD_BLOCK_TR: {
        s.tableRowFirstCell = true;
        break;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
        if (!s.tableRowFirstCell) {
            s.current.Text.append(" | ");
        }
        s.tableRowFirstCell = false;
        break;
    }
    case MD_BLOCK_HR: {
        s.current.Kind = BlockKind::Other;
        s.current.Text.assign("---");
        break;
    }
    default:
        break;
    }
    return 0;
}

int OnLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    WalkState& s = *static_cast<WalkState*>(userdata);
    switch (type) {
    case MD_BLOCK_H:
    case MD_BLOCK_P: {
        if (s.quoteDepth > 0 && type == MD_BLOCK_P) {
            return 0;
        }
        FlushBlock(s);
        break;
    }
    case MD_BLOCK_CODE: {
        FlushBlock(s);
        break;
    }
    case MD_BLOCK_LI: {
        FlushBlock(s);
        break;
    }
    case MD_BLOCK_UL:
    case MD_BLOCK_OL: {
        if (!s.listStack.empty()) {
            s.listStack.pop_back();
        }
        break;
    }
    case MD_BLOCK_QUOTE: {
        if (s.quoteDepth > 0) {
            --s.quoteDepth;
        }
        if (s.quoteDepth == 0) {
            FlushBlock(s);
        }
        break;
    }
    case MD_BLOCK_TR: {
        s.current.Text.push_back('\n');
        break;
    }
    case MD_BLOCK_TABLE: {
        FlushBlock(s);
        break;
    }
    case MD_BLOCK_HR: {
        FlushBlock(s);
        break;
    }
    default:
        break;
    }
    return 0;
}

int OnEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    WalkState& s = *static_cast<WalkState*>(userdata);
    switch (type) {
    case MD_SPAN_STRONG:
        s.current.Text.append("**");
        break;
    case MD_SPAN_EM:
        s.current.Text.append("*");
        break;
    case MD_SPAN_CODE:
        s.current.Text.append("`");
        break;
    case MD_SPAN_DEL:
        s.current.Text.append("~~");
        break;
    case MD_SPAN_A: {
        const MD_SPAN_A_DETAIL* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
        s.current.Text.push_back('[');
        s.linkHref.clear();
        if (d && d->href.text && d->href.size > 0) {
            s.linkHref.assign(d->href.text, d->href.text + d->href.size);
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

int OnLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    WalkState& s = *static_cast<WalkState*>(userdata);
    switch (type) {
    case MD_SPAN_STRONG:
        s.current.Text.append("**");
        break;
    case MD_SPAN_EM:
        s.current.Text.append("*");
        break;
    case MD_SPAN_CODE:
        s.current.Text.append("`");
        break;
    case MD_SPAN_DEL:
        s.current.Text.append("~~");
        break;
    case MD_SPAN_A: {
        s.current.Text.append("](");
        s.current.Text.append(s.linkHref);
        s.current.Text.push_back(')');
        s.linkHref.clear();
        break;
    }
    default:
        break;
    }
    return 0;
}

int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    WalkState& s = *static_cast<WalkState*>(userdata);
    switch (type) {
    case MD_TEXT_NORMAL:
    case MD_TEXT_ENTITY:
    case MD_TEXT_HTML:
        AppendBytes(s.current.Text, text, size);
        break;
    case MD_TEXT_CODE:
        AppendBytes(s.current.Text, text, size);
        break;
    case MD_TEXT_SOFTBR:
    case MD_TEXT_BR:
        s.current.Text.push_back('\n');
        if (s.quoteDepth > 0 && s.current.Kind == BlockKind::Quote) {
            s.current.Text.append("> ");
        }
        break;
    case MD_TEXT_NULLCHAR:
        s.current.Text.append("\xEF\xBF\xBD");
        break;
    default:
        break;
    }
    return 0;
}

} // namespace

std::vector<Block> ParseBlocks(const std::string& md) {
    WalkState s;
    if (md.empty()) {
        return s.blocks;
    }
    MD_PARSER parser;
    std::memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags =
        MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_TASKLISTS | MD_FLAG_NOHTMLBLOCKS;
    parser.enter_block = &OnEnterBlock;
    parser.leave_block = &OnLeaveBlock;
    parser.enter_span = &OnEnterSpan;
    parser.leave_span = &OnLeaveSpan;
    parser.text = &OnText;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &s);
    if (!s.current.Text.empty()) {
        s.blocks.push_back(std::move(s.current));
    }
    return s.blocks;
}

} // namespace AiChatMarkdownRender
