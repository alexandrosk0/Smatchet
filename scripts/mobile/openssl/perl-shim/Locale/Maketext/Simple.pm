package Locale::Maketext::Simple;
# Minimal shim — Smatchet Android/OpenSSL build (Git-for-Windows perl lacks this leaf).
# Real module is a fat Locale::Maketext front-end; OpenSSL's Configure only needs the
# gettext-style loc() that Params::Check / IPC::Cmd import for (rarely-hit) error text.
use strict;
use warnings;

sub import {
    my $class  = shift;
    my $caller = caller;
    no strict 'refs';
    # gettext-style loc("fmt %1 %2 %s", @args): handle %N positional + printf %s/%d.
    *{"${caller}::loc"} = sub {
        my $fmt = shift;
        return '' unless defined $fmt;
        my @a = @_;
        $fmt =~ s/%(\d+)/defined $a[$1-1] ? $a[$1-1] : ''/ge;
        # leftover printf-style conversions consume remaining args in order
        my $i = 0;
        $fmt =~ s/%([sd%])/$1 eq '%' ? '%' : (defined $a[$i] ? $a[$i++] : '')/ge;
        return $fmt;
    };
    *{"${caller}::loc_lang"} = sub { 1 };
    *{"${caller}::loc_advance"} = sub { 1 };
    *{"${caller}::loc_tag"} = sub { 1 };
}
1;
