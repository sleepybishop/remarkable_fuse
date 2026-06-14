#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 5;
use File::Temp qw(tempfile);

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Render v6 file to RM format (which compiles as a v5 RM file)
my ($fh, $filename) = tempfile(SUFFIX => '.rm', UNLINK => 1);
close($fh);

system("./remfmt t/assets/test_v6.rm rm > $filename");
is($?, 0, 'rendering to rm exit code is 0');

# Verify output is a valid RM file by checking header magic
open(my $in, '<:raw', $filename) or die "Cannot open $filename: $!";
my $header;
read($in, $header, 43);
close($in);

like($header, qr/^reMarkable \.lines file, version=\d/, 'output file starts with remarkable magic header');

# Round-trip check: parse the generated RM file and render it to SVG
my $svg_out = `./remfmt $filename svg`;
is($?, 0, 'parsing the generated rm file and rendering to svg succeeds');
like($svg_out, qr/<polyline/i, 'round-tripped svg contains polylines');
