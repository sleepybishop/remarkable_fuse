#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 7;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test rendering a version 6 file to PDF
my $output_pdf = `./remfmt t/test_v6.rm pdf`;
is($?, 0, 'remfmt pdf exit code is 0');
like($output_pdf, qr/^%PDF-1\.4/m, 'output has PDF signature');
like($output_pdf, qr/\/Type \/Catalog/m, 'output has Catalog object');
like($output_pdf, qr/\/Type \/Page/m, 'output has Page object');
like($output_pdf, qr/startxref/m, 'output has startxref section');
like($output_pdf, qr/%%EOF/m, 'output has EOF signature');
