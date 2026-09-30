- 2026-09-30 · deviation renewal · [debt] · P3 — tracker clients repeat the same GET / ParseBounded / error-classification idiom

  Details: the Jira, GitHub, Plane and Linear clients each spell out the same sequence for a JSON GET:
  `TrackerGetLogged`, a non-200 check that logs and maps a 2xx-other status to
  `TrackerErrorUnknown(detail, status)` and every other status through `TrackerErrorFromHttpStatus`,
  then `json_safe::ParseBounded` with a `TrackerErrorParse` on failure. `dup_audit.py` reports it as
  cross-file clones, which carry `rule=duplication` exemptions (reason "tracker GET/ParseBounded
  error-handling idiom, shared helper backlogged", `revisit=2027-03-31`) in `GitHubActivityFeed.cpp`,
  `GitHubClient.cpp`, `JiraIssueSearch.cpp`, `JiraUserAndMeta.cpp`, `LinearClient.cpp`,
  `PlaneActivityFeed.cpp` and `TrackerFieldCatalog.cpp`. The earlier markers
  said de-duplicating would couple independent subsystems; it would not, because all of these live in
  `Tracker/` and already share `TrackerHttpUtils`. The copies drift: the 2xx-other guard was added to
  each one separately (DR20).

  Concrete next action: add one helper next to `TrackerGetLogged` in `TrackerHttpUtils`, e.g.
  `Result<nlohmann::json, TrackerError> TrackerGetJsonClassified(const char* tag, const std::string& url, const cpr::Header& headers, const char* what)`,
  which returns the classified error for every failure exit. Move the call sites onto it one client
  at a time, each with its existing HTTP-fixture tests, and delete the exemption at each moved site.
  Status: open
  Last-reviewed: 2026-09-30
