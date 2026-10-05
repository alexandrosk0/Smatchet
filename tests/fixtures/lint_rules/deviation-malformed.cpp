// deviation-malformed fixture: lines 5, 8, 11, 14, 17, 23, 26, 29, 32 and 35 are not whole markers; line 20 is.
#include <memory>
struct Thing {};

// SMATCHET_DEVIATION(rule=no-raw-new; reason=a wrapped marker, whose revisit sits on the
// next line; owner=alex; revisit=2020-01-01)
Thing* a() { return new Thing(); }
// SMATCHET_DEVIATION(rule=no-raw-new; owner=alex; revisit=2099-01-01)
Thing* b() { return new Thing(); }

// SMATCHET_DEVIATION(rule=no-raw-new; reason=no owner; revisit=2099-01-01)
Thing* c() { return new Thing(); }

// SMATCHET_DEVIATION(rule=no-raw-new): old prose grammar with no fields after the rule
Thing* d() { return new Thing(); }

// A prose mention such as SMATCHET_DEVIATION(rule=no-raw-new) is read as a marker too.
Thing* e() { return new Thing(); }

// SMATCHET_DEVIATION(rule=no-raw-new; reason=whole marker (with a parenthetical); owner=alex; revisit=2099-01-01)
std::unique_ptr<Thing> f() { return std::unique_ptr<Thing>(new Thing()); }

// SMATCHET_DEVIATION(rule=no-raw-new; reason=  ; owner=alex; revisit=2099-01-01)
Thing* g() { return new Thing(); }

// SMATCHET_DEVIATION(rule=; reason=blank rule; owner=alex; revisit=2099-01-01)
int h = 0;

// SMATCHET_DEVIATION(rule=no-raw-new; reason=blank owner; owner= ; revisit=2099-01-01)
int i = 0;

// SMATCHET_DEVIATION(rule=no-raw-new; reason=no revisit key at all; owner=alex)
int j = 0;

// SMATCHET_DEVIATION(rule=no-raw-new; owner=alex; revisit=2099-01-01; reason=its last paren closes (this aside) and the marker runs on
Thing* k() { return new Thing(); }
