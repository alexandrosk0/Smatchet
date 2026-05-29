#include "FieldEditAuditSource.h"

#include <vector>

namespace FieldEditAuditSource {
namespace {
thread_local std::vector<const char*> stack;
}

const char* Current() {
    if (stack.empty()) {
        return kUi;
    }
    const char* s = stack.back();
    return (s != nullptr && s[0] != '\0') ? s : kUi;
}

ScopedOverride::ScopedOverride(const char* source) {
    stack.push_back((source != nullptr && source[0] != '\0') ? source : kUi);
}

ScopedOverride::~ScopedOverride() {
    if (!stack.empty()) {
        stack.pop_back();
    }
}

} // namespace FieldEditAuditSource
