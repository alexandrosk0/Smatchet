#include "PlaneQuerySuggestEngine.h"

#include "AppController.h"
#include "Tracker/PlaneQuerySuggestEnginePure.h"

void BuildPlaneQuerySuggestions(const char* buf, int bufLen, int cursor, int selStart, int selEnd,
                                const AppController& app, QuerySuggestBuild& out, QuerySuggestMeta* metaOut) {
    PlaneQuerySuggestEnginePure::BuildPlaneQuerySuggestions(buf, bufLen, cursor, selStart, selEnd,
                                                            app.GetAvailableFields(), out, metaOut);
}
