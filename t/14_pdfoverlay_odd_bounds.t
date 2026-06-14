#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 5;
use File::Temp qw(tempdir);

# Check if pdfoverlay binary exists and is executable
ok(-x './pdfoverlay', 'pdfoverlay binary exists and is executable');

my $tmp_dir = tempdir(CLEANUP => 1);
my $src_pdf = "$tmp_dir/odd_bounds.pdf";
my $out_pdf = "$tmp_dir/out.pdf";
my $src_png = "./templates/Blank.png";

# Create a minimal PDF with an offset MediaBox [ 100 100 500 700 ]
open(my $fh, '>', $src_pdf) or die "Cannot create temp pdf: $!";
print $fh <<'EOF';
%PDF-1.4
1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj
2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj
3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [100 100 500 700] >>
endobj
xref
0 4
0000000000 65535 f 
0000000009 00000 n 
0000000058 00000 n 
0000000115 00000 n 
trailer
<< /Size 4 /Root 1 0 R >>
startxref
189
%%EOF
EOF
close($fh);

# Run pdfoverlay
# We specify a small draw_w, draw_h (e.g. 200 300) 
my $cmd = "./pdfoverlay \"$src_pdf\" \"$src_png\" \"$out_pdf\" 1 50 100 200 300";
my $ret = system("$cmd >/dev/null 2>&1");

is($ret, 0, 'pdfoverlay exit code is 0');
ok(-f $out_pdf, 'output PDF file was created');

# Verify with pdfinfo if available
my $info = `pdfinfo "$out_pdf" 2>&1`;
is($?, 0, 'pdfinfo on output PDF succeeds');

# Check that output PDF has correct MediaBox preservation and Image insertion
my $pdf_content = do {
    open(my $out_fh, '<', $out_pdf) or die "Cannot open output PDF";
    local $/;
    <$out_fh>;
};

like($pdf_content, qr/\[\s*100\s+100\s+500\s+700\s*\]/, 'output PDF preserves the offset MediaBox');
