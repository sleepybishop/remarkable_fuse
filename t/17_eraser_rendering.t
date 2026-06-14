#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 7;
use File::Temp qw(tempfile);

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Construct a mock version 5 .rm file binary data
# Header: 43 bytes magic
my $magic = "reMarkable .lines file, version=5";
my $header = $magic . (" " x (43 - length($magic)));

# Binary fields
my $num_layers = pack("I", 1);
my $num_strokes = pack("I", 2);

# Stroke 1: Ballpoint (color=BLACK)
my $stroke1_header = pack("IIfffI",
    2,      # pen_type: BALLPOINT
    0,      # color: BLACK
    0.0,    # unk1
    2.0,    # width
    0.0,    # unk2
    2       # num_segments
);
my $stroke1_seg1 = pack("ffffff", 100.0, 100.0, 0.0, 0.0, 2.0, 1.0);
my $stroke1_seg2 = pack("ffffff", 200.0, 100.0, 0.0, 0.0, 2.0, 1.0);

# Stroke 2: Eraser (color=WHITE)
my $stroke2_header = pack("IIfffI",
    6,      # pen_type: ERASER
    2,      # color: WHITE (WHITE = 2 in remfmt.h)
    0.0,    # unk1
    10.0,   # width
    0.0,    # unk2
    2       # num_segments
);
my $stroke2_seg1 = pack("ffffff", 150.0, 50.0, 0.0, 0.0, 10.0, 1.0);
my $stroke2_seg2 = pack("ffffff", 150.0, 150.0, 0.0, 0.0, 10.0, 1.0);

# Combine everything
my $rm_data = $header . $num_layers . $num_strokes 
            . $stroke1_header . $stroke1_seg1 . $stroke1_seg2
            . $stroke2_header . $stroke2_seg1 . $stroke2_seg2;

# Write to temp file
my ($fh, $filename) = tempfile(SUFFIX => '.rm', UNLINK => 1);
binmode($fh);
print $fh $rm_data;
close($fh);

# Render to SVG and check
my $svg_out = `./remfmt "$filename" svg`;
is($?, 0, 'rendering to svg succeeds');
like($svg_out, qr/stroke:#ffffff/i, 'SVG output contains white stroke for eraser');
like($svg_out, qr/opacity:1\.000/i, 'SVG output renders eraser with 1.0 opacity');

# Render to PDF and check
my $pdf_out = `./remfmt "$filename" pdf`;
is($?, 0, 'rendering to pdf succeeds');
like($pdf_out, qr/\/GS100 gs/m, 'PDF output uses GS100 (100% opacity) for eraser');
like($pdf_out, qr/1\.000 1\.000 1\.000 RG/m, 'PDF output draws white color for eraser');
