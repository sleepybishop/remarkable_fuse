#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 3;
use File::Temp qw(tempdir);

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

my $tmp_dir = tempdir(CLEANUP => 1);
my $out_svg = "$tmp_dir/out.svg";

# We run remfmt on test_v6_glyph.rm which contains Highlighter V2 strokes
system("./remfmt t/assets/test_v6_glyph.rm svg > $out_svg 2>/dev/null");
is($?, 0, 'remfmt svg exit code is 0');

# Verify that the generated SVG contains stroke-linecap="square" or opacity="0.25"
my $svg_content = do {
    open(my $fh, '<', $out_svg) or die "Cannot open output SVG";
    local $/;
    <$fh>;
};

like($svg_content, qr/opacity="0\.25"|stroke-linecap="square"/, 'SVG contains Highlighter V2 properties (opacity or square linecap)');
