#!/usr/bin/env perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);
use File::Copy qw(copy);
use File::Path qw(make_path);
use Time::HiRes qw(sleep);

# Check if FUSE is available and writeable
my $has_fuse = 0;
if (-e '/dev/fuse' && -w '/dev/fuse') {
    $has_fuse = 1;
}
if (!$has_fuse) {
    plan skip_all => "FUSE is not available or writeable on this system";
}

# Check if pdfimages is available
my $has_pdfimages = `which pdfimages 2>/dev/null`;
if (!$has_pdfimages) {
    plan skip_all => "pdfimages utility is required for this test";
}

plan tests => 5;

# Setup temp directory structure
my $tmp_dir = tempdir(CLEANUP => 1);
my $xochitl_dir = "$tmp_dir/xochitl";
my $mnt_dir = "$tmp_dir/mnt";
mkdir $xochitl_dir;
mkdir $mnt_dir;

# Copy sample PDF files and metadata
my $doc_uuid = "98a5e265-2a12-4d23-81f7-03fee0b5554c";
copy("t/assets/xochitl/$doc_uuid.content", "$xochitl_dir/$doc_uuid.content");
copy("t/assets/xochitl/$doc_uuid.pagedata", "$xochitl_dir/$doc_uuid.pagedata");
copy("t/assets/xochitl/$doc_uuid.pdf", "$xochitl_dir/$doc_uuid.pdf");

# Write custom metadata to place it at the root of the FUSE mount as "MyPDF"
open(my $met_fh, '>', "$xochitl_dir/$doc_uuid.metadata") or die "Could not write metadata: $!";
print $met_fh <<EOF;
{
    "deleted": false,
    "lastModified": "1650142183090",
    "lastOpenedPage": 0,
    "metadatamodified": false,
    "modified": true,
    "parent": "",
    "pinned": false,
    "synced": false,
    "type": "DocumentType",
    "version": 0,
    "visibleName": "MyPDF"
}
EOF
close($met_fh);

# Create the page strokes subdirectory and copy our sample annotations
my $page_uuid = "5609e168-e583-4741-8ac0-3c1098adf112";
make_path("$xochitl_dir/$doc_uuid");
copy("t/assets/test_v6.rm", "$xochitl_dir/$doc_uuid/$page_uuid.rm") or die "Could not copy test strokes: $!";

# Write FUSE config file with PDF rendering enabled
my $config_file = "$tmp_dir/config.json";
open(my $fh, '>', $config_file) or die "Could not write config: $!";
print $fh <<EOF;
{
    "data_dir": "$xochitl_dir",
    "mutable": false,
    "renderers": ["pdf"]
}
EOF
close($fh);

# Start remfs FUSE mount in background
my $pid = fork();
if ($pid == 0) {
    exec('./remfs', "--config=$config_file", $mnt_dir);
    exit(1);
}

# Wait for FUSE to mount
sleep(0.5);

# Check if the PDF is visible as MyPDF.annotated.pdf
my $pdf_path = "$mnt_dir/MyPDF.annotated.pdf";
ok(-f $pdf_path, "PDF file is visible under mount");

# Verify file size reflects the overlay
my $orig_size = -s "xochitl/$doc_uuid.pdf";
my $new_size = -s $pdf_path;
ok($new_size > $orig_size, "Annotated PDF ($new_size bytes) is larger than original ($orig_size bytes)");

# Verify that the overlay contains the annotation image via pdfimages
my $pdf_images_out = `pdfimages -list -f 1 -l 1 "$pdf_path" 2>/dev/null`;
like($pdf_images_out, qr/image/i, "Image overlay object is detected on page 1");

# Check for presence of draw command /Im1 in the PDF content stream
my $pdf_content = do {
    open(my $pdf_fh, '<', $pdf_path) or die "Cannot open mounted PDF";
    local $/;
    <$pdf_fh>;
};
like($pdf_content, qr/\/Im1 Do/m, "Overlay image drawing command /Im1 Do is present in the stream");
like($pdf_content, qr/\/Type \/XObject \/Subtype \/Image/m, "Image object is defined in the resources");

# Clean up FUSE daemon
kill('TERM', $pid);
waitpid($pid, 0);

# Force unmount to clean up mount point
system("fusermount3 -u -q -z $mnt_dir 2>/dev/null || fusermount -u -q -z $mnt_dir 2>/dev/null");
unlink $config_file;
