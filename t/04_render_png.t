#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 4;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test rendering a version 6 file to PNG
my $output_png = `./remfmt t/test_v6.rm png`;
is($?, 0, 'remfmt png exit code is 0');
my $png_magic = substr($output_png, 0, 8);
is($png_magic, "\x89PNG\x0d\x0a\x1a\x0a", 'output has PNG signature');
ok(length($output_png) > 1000000, 'output PNG size is reasonable');
