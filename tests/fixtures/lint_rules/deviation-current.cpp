struct Thing {};
// SMATCHET_DEVIATION(rule=no-raw-new; reason=bounded pool; owner=alex; revisit=2099-12-31)
Thing* t = new Thing();
