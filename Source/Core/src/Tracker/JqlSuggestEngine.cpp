#include "JqlSuggestEngine.h"

#include "JqlSuggestEnginePure.h"

// Thin shell over the pure engine (gap map Tier 1 #5): the whole tokenizer / mode-resolver /
// suggestion pipeline lives in JqlSuggestEnginePure.cpp, which takes the field/user catalogs
// as explicit vectors so tests compile it without AppController. This entry point takes the
// same catalog vectors (extracted by callers that hold the full app object) and forwards.
void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                         const std::vector<TrackerField>& fields, const std::vector<TrackerUser>& users,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    BuildJqlSuggestionsPure(buf, bufLen, cursor, selStart, selEnd, fields, users, out, metaOut);
}
