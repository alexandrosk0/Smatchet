struct Thing {};
// A reason= carrying a parenthetical used to hide revisit= from DEV_RE's `[^)]*` body capture,
// so this marker suppressed no-raw-new but could never go overdue. Both must fire correctly.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=bounded pool (arena-backed, see Thing); owner=alex; revisit=2020-01-01)
Thing* t = new Thing();
