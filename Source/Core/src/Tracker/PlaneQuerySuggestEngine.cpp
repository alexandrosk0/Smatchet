#include "PlaneQuerySuggestEngine.h"

#include "PlaneQuerySuggestEnginePure.h"

// Thin shell over the pure engine (gap map Tier 1 #5): the whole parse / suggestion pipeline
// lives in PlaneQuerySuggestEnginePure.cpp, which takes the field catalog as an explicit
// vector so tests compile it without AppController. This entry point takes the same catalog
// vector (extracted by callers that hold the full app object) and forwards.
void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const std::vector<TrackerField>& fields, QuerySuggestBuild& out,
                                QuerySuggestMeta* metaOut) {
    BuildPlaneQuerySuggestionsPure(buf, bufLen, cursor, selStart, selEnd, fields, out, metaOut);
}
