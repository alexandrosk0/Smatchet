#ifndef SMATCHET_P4V_LAUNCH_H
#define SMATCHET_P4V_LAUNCH_H

#include "ConfigManager.h"

#include <string>

/**
 * p4v / p4vc launcher, extracted verbatim from AnnotateAnalysisUi_Launch.cpp so
 * non-Annotate windows (User Info window) can reuse it. Behaviour unchanged:
 * direct `p4vc timelapse|change` by default; custom command templates require
 * the `annotate_allow_custom_commands` config opt-in. Windows-only (returns
 * false elsewhere).
 */
namespace P4vLaunch {

/**
 * Launch p4vc (or the configured custom command) for a timelapse or change view.
 * `timelapseTemplate` / `changeTemplate` may contain `{file}` / `{line}` / `{cl}`
 * placeholders; empty templates fall back to direct p4vc. Returns true when the
 * launch was handed to the shell successfully.
 */
bool LaunchP4VcLike(const AnnotateAnalysisConfig& cfg, const std::string& timelapseTemplate,
                    const std::string& changeTemplate, bool isTimelapse, const std::string& file, int line,
                    const std::string& cl);

} // namespace P4vLaunch

#endif
