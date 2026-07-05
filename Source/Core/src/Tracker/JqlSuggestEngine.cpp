#include "JqlSuggestEngine.h"

#include "AppController.h"
#include "JqlSuggestEnginePure.h"

// Thin shell over the pure engine (gap map Tier 1 #5): the whole tokenizer / mode-resolver /
// suggestion pipeline lives in JqlSuggestEnginePure.cpp, which takes the field/user catalogs
// as explicit vectors so tests compile it without AppController. This TU only resolves the
// two catalog getters and forwards.
void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd, const AppController& app,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    BuildJqlSuggestionsPure(buf, bufLen, cursor, selStart, selEnd, app.GetAvailableFields(), app.GetAvailableUsers(),
                            out, metaOut);
}
