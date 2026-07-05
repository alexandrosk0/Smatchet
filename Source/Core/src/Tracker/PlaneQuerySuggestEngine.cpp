#include "PlaneQuerySuggestEngine.h"

#include "AppController.h"
#include "PlaneQuerySuggestEnginePure.h"

// Thin shell over the pure engine (gap map Tier 1 #5): the whole parse / suggestion pipeline
// lives in PlaneQuerySuggestEnginePure.cpp, which takes the field catalog as an explicit
// vector so tests compile it without AppController. This TU only resolves the catalog getter
// and forwards.
void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const AppController& app, QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    BuildPlaneQuerySuggestionsPure(buf, bufLen, cursor, selStart, selEnd, app.GetAvailableFields(), out, metaOut);
}
