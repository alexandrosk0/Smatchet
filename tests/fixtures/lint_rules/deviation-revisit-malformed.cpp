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
