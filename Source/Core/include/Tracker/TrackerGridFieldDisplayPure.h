#ifndef SMATCHET_TRACKER_GRID_FIELD_DISPLAY_PURE_H
#define SMATCHET_TRACKER_GRID_FIELD_DISPLAY_PURE_H

#include "Types/AttachmentTypes.h"

#include <string>
#include <vector>

/**
 * Pure render-model builders lifted from TrackerGridFieldDisplay.cpp's anonymous namespace so
 * the tracker-supplied (untrusted) field-value parsing can be unit-tested without dragging in
 * ImGui / AppController (the production TU also hosts the grid draw code, which pulls
 * `<imgui.h>` and the AppController async plumbing). No GUI, no I/O. Implementations are
 * byte-identical to the originals (arithmetic + control flow preserved); the only changes are
 * namespace + linkage and `AppController::AttachmentDescriptor` spelled through its underlying
 * `::AttachmentDescriptor` alias target (`Types/AttachmentTypes.h`, a rank-0 leaf) so the pure
 * TU never includes AppController.h.
 *
 * Every builder takes the raw cell string a tracker backend produced (Jira/Plane/GitHub HTTP
 * response bytes cached into CachedTicket fields — off-host origin) and returns a display
 * model. All JSON goes through `TryParseJsonMaybeDoubleEncoded` (ParseBounded under the hood),
 * so hostile depth/size payloads degrade to the unparsed-fallback model instead of recursing
 * the native stack (SECURITY_AUDIT.md unbounded-parse class).
 *
 * Owner: `tracker-backend` subsystem. Test surface: tests/Core/TrackerGridFieldDisplayPure.test.cpp.
 */
namespace TrackerGridFieldDisplayPure {

struct AttachmentRenderModel {
    bool parsed = false;
    bool explicitEmpty = false;
    std::vector<AttachmentDescriptor> descriptors;
    std::string display;
    std::string tooltip;
};

struct WatchersRenderModel {
    bool parsed = false;
    bool isWatching = false;
    std::string line;
    std::string tooltip;
};

struct VotesRenderModel {
    bool parsed = false;
    std::string line;
    std::string tooltip;
};

struct WorklogRenderModel {
    bool parsed = false;
    std::string line;
    std::string tooltip;
};

struct IssueRestrictionRenderModel {
    bool rendered = false;
    std::string display;
};

struct ProgressRenderModel {
    bool rendered = false;
    float fraction = 0.0f;
};

AttachmentRenderModel BuildAttachmentRenderModel(const std::string& currentValue);
WatchersRenderModel BuildWatchersRenderModel(const std::string& currentValue);
VotesRenderModel BuildVotesRenderModel(const std::string& currentValue);
WorklogRenderModel BuildWorklogRenderModel(const std::string& currentValue);
IssueRestrictionRenderModel BuildIssueRestrictionRenderModel(const std::string& currentValue);
ProgressRenderModel BuildProgressRenderModel(const std::string& currentValue);

} // namespace TrackerGridFieldDisplayPure

#endif // SMATCHET_TRACKER_GRID_FIELD_DISPLAY_PURE_H
