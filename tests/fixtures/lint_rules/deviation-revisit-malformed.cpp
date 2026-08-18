struct Thing {};
// Month 19 passes the old `[0-1][0-9]` glob and string-sorts after any real today, so this used
// to be a permanent exemption that no one could see.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=impossible month; owner=alex; revisit=2026-19-30)
Thing* a = new Thing();
// A PAST date that misses the glob because it is not zero-padded — read as a slug, never overdue.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=not zero-padded; owner=alex; revisit=2020-1-1)
Thing* b = new Thing();
// A day that does not exist in that month; the comparison is lexicographic, not a date parse.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=day out of range; owner=alex; revisit=2026-02-30)
Thing* c = new Thing();
// LEADING whitespace before a past date: untrimmed it misses the date shape entirely, falls to the
// slug arm and buys a permanent silent exemption — the fail-open direction this rule exists to end.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=leading space; owner=alex; revisit= 2020-01-01)
Thing* d = new Thing();
// `revisit=` typed but left EMPTY is a malformed attempt, not the sanctioned absent-field case.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=empty revisit; owner=alex; revisit=)
Thing* e = new Thing();
// A quarter that does not exist, and an ISO-basic date the grammar does not permit.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=no fifth quarter; owner=alex; revisit=2026-Q5)
Thing* f = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=ISO basic not in grammar; owner=alex; revisit=20270811)
Thing* g = new Thing();
