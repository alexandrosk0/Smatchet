#pragma once

#include "Ui/SmatchetFieldViewerModalUi.h"

#include <memory>
#include <string>

/// Viewer content for the synthetic History field: the plain `[Author] date` activity blob rendered as
/// Markdown. The blob is copied at open (the ticket snapshot may be rebuilt while the window is open).
std::unique_ptr<IFieldViewerContent> MakeActivityLogViewerContent(std::string blob);
