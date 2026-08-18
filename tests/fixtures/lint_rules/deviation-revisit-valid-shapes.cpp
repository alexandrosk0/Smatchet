struct Thing {};
// Every shape here is legitimate and in the future (or a non-calendar trigger), so none of them
// may fire deviation-overdue — the malformed-revisit check must not over-reach onto valid values.
// A far-future leap day on purpose: 2028-02-29 would have expired on 2028-03-01 and red-walled this
// very assertion for a reason unrelated to the code under test.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=leap day exists in 2096; owner=alex; revisit=2096-02-29)
Thing* a = new Thing();
// A digit-LEADING slug. The grammar permits slugs and says they do not expire; an earlier draft of
// revisit_datelike_but_invalid read any `YYYY-*` as a calendar attempt and turned this into an
// unsuppressable hard FAIL, because deviation-overdue is absolute and cannot itself be deviated.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=digit-leading slug; owner=alex; revisit=2026-roadmap)
Thing* e = new Thing();
// Trailing whitespace after a FUTURE date: the value must be trimmed before the date-shape check,
// or it misses the 10-char glob, classifies as a malformed attempt and is reported overdue on 2099.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=trailing space; owner=alex; revisit=2099-12-31 )
Thing* f = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=quarter form; owner=alex; revisit=2099-Q4)
Thing* b = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=standing exemption; owner=alex; revisit=never)
Thing* c = new Thing();
// SMATCHET_DEVIATION(rule=no-raw-new; reason=trigger prose; owner=alex; revisit=when the shared leaf lands)
Thing* d = new Thing();
