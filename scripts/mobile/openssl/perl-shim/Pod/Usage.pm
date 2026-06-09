package Pod::Usage;
# Minimal shim — Smatchet Android/OpenSSL build. OpenSSL's generated configdata.pm does
# `use Pod::Usage` (compile-time, inside `unless(caller)`) but only calls pod2usage() for
# its own --help/--options paths, never on a normal Configure run. Full Pod::Usage needs a
# Pod parser absent from Git-for-Windows perl; we provide a load-safe pod2usage.
use strict;
use warnings;
use Exporter ();
our @ISA     = qw(Exporter);
our @EXPORT  = qw(pod2usage);
our $VERSION = '0.000_shim';

sub pod2usage {
    my %o = (@_ == 1 && (!ref $_[0])) ? (-exitval => $_[0])
          : (ref $_[0] eq 'HASH')     ? %{ $_[0] }
          :                              @_;
    my $msg = defined $o{-message} ? $o{-message} : $o{-msg};
    print STDERR "$msg\n" if defined $msg;
    print STDERR "(usage text unavailable: Pod::Usage shim)\n";
    my $ev = $o{-exitval};
    return if defined $ev && $ev eq 'NOEXIT';
    $ev = 1 unless defined $ev && $ev =~ /^\d+$/;
    exit $ev;
}
1;
