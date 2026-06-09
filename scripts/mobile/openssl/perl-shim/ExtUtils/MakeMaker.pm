package ExtUtils::MakeMaker;
# Minimal shim — Smatchet Android/OpenSSL build. The full ExtUtils::MakeMaker is absent
# from Git-for-Windows perl and pulls a large MM_Unix/MM_Any/Pod cascade. OpenSSL's
# Configure reaches it through one path only: IPC::Cmd::can_run -> MM->maybe_command,
# a "is this path an executable file" test. We provide just that. Under msys-flavor perl
# -x resolves extensionless `clang` to `clang.exe`, so NDK tool detection works.
use strict;
use warnings;
our $VERSION = '0.000_shim';

package MM;
sub maybe_command {
    my ($self, $file) = @_;
    return $file if !-d $file && -x $file;
    return;
}
1;
