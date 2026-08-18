struct Thing {};
// reason= carries a parenthetical and precedes rule=. With the old `[^)]*` body capture the split
// stopped at the FIRST ')', so rule= was never seen and the marker granted NO suppression — while
// dup_audit.py, include_cycle_audit.py and appcontroller_fan_in_audit all match rule= over the whole
// line and DID suppress the same marker. Neither no-raw-new nor deviation-overdue may fire here.
// SMATCHET_DEVIATION(reason=shared include set (ConfigManager / backends); rule=no-raw-new; owner=alex; revisit=2099-12-31)
Thing* a = new Thing();
// Same ordering with a PAST date: suppression still applies to the next line, and the expiry behind
// the parenthetical must still be seen, so deviation-overdue fires on the marker line itself.
// SMATCHET_DEVIATION(reason=shared include set (ConfigManager / backends); rule=no-raw-new; owner=alex; revisit=2020-01-01)
Thing* b = new Thing();
