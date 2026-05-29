#include "AiModelSignature.h"

namespace smatchet {
namespace ai {

ModelSignatureChangeResult DetectModelChange(const std::string& previousSignature, const std::string& providerName,
                                             const std::string& model) {
    ModelSignatureChangeResult r;
    r.ShouldClear = false;
    r.NewSignature = providerName;
    r.NewSignature.push_back('|');
    r.NewSignature.append(model);
    if (!previousSignature.empty() && previousSignature != r.NewSignature) {
        r.ShouldClear = true;
    }
    return r;
}

} // namespace ai
} // namespace smatchet
