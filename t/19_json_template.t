#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 7;

ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test rendering a version 6 file to SVG with a JSON template background
my $output_svg = `./remfmt --template-dir templates --template-name "P Grid small" t/assets/test_v6.rm svg`;
is($?, 0, 'remfmt svg with JSON template exit code is 0');
like($output_svg, qr/href="data:image\/png;base64,/, 'output SVG has embedded background image rendered from JSON');

# Test rendering a version 6 file to PDF with a JSON template background
my $output_pdf = `./remfmt --template-dir templates --template-name "P Grid small" t/assets/test_v6.rm pdf`;
is($?, 0, 'remfmt pdf with JSON template exit code is 0');
like($output_pdf, qr/\/Type \/XObject \/Subtype \/Image/, 'output PDF has XObject image for background rendered from JSON');

# Test rendering a version 6 file to PNG with a JSON template background
my $output_png = `./remfmt --template-dir templates --template-name "P Grid small" t/assets/test_v6.rm png`;
is($?, 0, 'remfmt png with JSON template exit code is 0');
ok(length($output_png) > 1000, 'output PNG size is reasonable');
