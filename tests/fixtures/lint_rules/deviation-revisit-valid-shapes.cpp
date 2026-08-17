struct Thing {};
// Every shape here is legitimate and in the future (or a non-calendar trigger), so none of them
// may fire deviation-overdue — the malformed-revisit check must not over-reach onto valid values.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=leap day exists in 2028; owner=alex; revisit=2028-02-29)
Thing* a = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=quarter form; owner=alex; revisit=2099-Q4)
Thing* b = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=standing exemption; owner=alex; revisit=never)
Thing* c = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=trigger prose; owner=alex; revisit=when the shared leaf lands)
Thing* d = new Thing();
