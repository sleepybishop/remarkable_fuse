#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 5;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test rendering a version 6 file to XOJ
system("./remfmt t/assets/test_v6.rm xoj > /tmp/out.xoj");
is($?, 0, 'remfmt xoj exit code is 0');
my $size = -s '/tmp/out.xoj';
ok($size > 100, 'output has non-zero length');

# Decompress the output to verify XML
my $uncompressed = `zcat -f /tmp/out.xoj 2>/dev/null`;
like($uncompressed, qr/<xournal/, 'uncompressed output contains xournal tag');
like($uncompressed, qr/<stroke tool="pen"/, 'uncompressed output contains stroke elements');
unlink '/tmp/out.xoj';
