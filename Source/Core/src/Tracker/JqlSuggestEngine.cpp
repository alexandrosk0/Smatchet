#include "JqlSuggestEngine.h"

#include "AppController.h"
#include "Tracker/JqlSuggestEnginePure.h"

void BuildJqlSuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd, const AppController& app,
                         QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    JqlSuggestEnginePure::BuildJqlSuggestions(buf, bufLen, cursor, selStart, selEnd, app.GetAvailableFields(),
                                              app.GetAvailableUsers(), out, metaOut);
}
