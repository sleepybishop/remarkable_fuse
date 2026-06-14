#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 5;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test rendering a version 6 file to SVG
my $output_svg = `./remfmt t/test_v6.rm svg`;
is($?, 0, 'remfmt svg exit code is 0');
like($output_svg, qr/<svg/i, 'output contains svg opening tag');
like($output_svg, qr/<\/svg>/i, 'output contains svg closing tag');
like($output_svg, qr/<polyline/i, 'output contains polyline elements');
