#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 7;
use File::Temp qw(tempdir);

# Check if pdfoverlay binary exists and is executable
ok(-x './pdfoverlay', 'pdfoverlay binary exists and is executable');

my $tmp_dir = tempdir(CLEANUP => 1);
my $out_pdf = "$tmp_dir/out.pdf";
my $src_pdf = "./xochitl/98a5e265-2a12-4d23-81f7-03fee0b5554c.pdf";
my $src_png = "./templates/Blank.png";

# Run pdfoverlay
my $cmd = "./pdfoverlay \"$src_pdf\" \"$src_png\" \"$out_pdf\" 1 50 100 200 300";
my $ret = system($cmd);

is($ret, 0, 'pdfoverlay exit code is 0');
ok(-f $out_pdf, 'output PDF file was created');
ok(-s $out_pdf > -s $src_pdf, 'output PDF is larger than source PDF');

# Verify with pdfinfo
my $info = `pdfinfo "$out_pdf" 2>&1`;
is($?, 0, 'pdfinfo on output PDF succeeds');

# Check that output PDF has correct properties
my $pdf_content = do {
    open(my $fh, '<', $out_pdf) or die "Cannot open output PDF";
    local $/;
    <$fh>;
};

like($pdf_content, qr/\/Type \/XObject \/Subtype \/Image/m, 'output PDF has the Image XObject');
like($pdf_content, qr/\/Im1 Do/m, 'output PDF has the draw image command');
