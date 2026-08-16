struct Thing {};
// Same parenthetical-reason shape as deviation-overdue-paren-reason.cpp, but the revisit date is
// still in the future — the marker must suppress no-raw-new WITHOUT firing deviation-overdue.
// SMATCHET_DEVIATION(rule=no-raw-new; reason=bounded pool (arena-backed, see Thing); owner=alex; revisit=2099-12-31)
Thing* t = new Thing();
