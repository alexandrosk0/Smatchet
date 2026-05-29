struct Thing {};
// SMATCHET_DEVIATION(rule=no-raw-new; reason=bounded pool; owner=alex; revisit=2020-01-01)
Thing* t = new Thing();
