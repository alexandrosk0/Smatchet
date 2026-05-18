#include "SmatchetAgentProposalsUiPure.h"

namespace SmatchetAgentProposalsUiPure {

std::string TruncatePreview(const std::string& s) {
    if (s.size() <= kRationalePreviewBytes) {
        return s;
    }
    std::size_t cut = kRationalePreviewBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut) + std::string("...");
}

} // namespace SmatchetAgentProposalsUiPure
