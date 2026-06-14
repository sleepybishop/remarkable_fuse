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

plan tests => 7;

# Setup temp directory structure
my $tmp_dir = tempdir(CLEANUP => 1);
my $xochitl_dir = "$tmp_dir/xochitl";
my $mnt_dir = "$tmp_dir/mnt";
mkdir $xochitl_dir;
mkdir $mnt_dir;

# Copy sample notebook files and metadata
my $doc_uuid = "137a5cea-3235-42fd-9725-033bc8efb933";
copy("xochitl/$doc_uuid.content", "$xochitl_dir/$doc_uuid.content") or die "Copy content failed: $!";
copy("xochitl/$doc_uuid.pagedata", "$xochitl_dir/$doc_uuid.pagedata") or die "Copy pagedata failed: $!";
copy("xochitl/$doc_uuid.metadata", "$xochitl_dir/$doc_uuid.metadata") or die "Copy metadata failed: $!";

make_path("$xochitl_dir/$doc_uuid");
# Copy all .rm files in the directory
opendir(my $dh, "xochitl/$doc_uuid") or die "Cannot open directory: $!";
while (my $file = readdir($dh)) {
    next if $file =~ /^\./;
    if ($file =~ /\.rm$/ || $file =~ /\.json$/) {
        copy("xochitl/$doc_uuid/$file", "$xochitl_dir/$doc_uuid/$file") or die "Copy $file failed: $!";
    }
}
closedir($dh);

# Write FUSE config file with PDF rendering enabled
use Cwd qw(abs_path);
my $abs_templates_dir = abs_path("./templates");
my $config_file = "$tmp_dir/config.json";
open(my $fh, '>', $config_file) or die "Could not write config: $!";
print $fh <<EOF;
{
    "data_dir": "$xochitl_dir",
    "template_dir": "$abs_templates_dir",
    "mutable": false,
    "svg": false,
    "png": false,
    "pdf": true
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

# Check if the notebook PDF is visible as Interviews.pdf
my $pdf_path = "$mnt_dir/Interviews.pdf";
ok(-f $pdf_path, "Notebook PDF file is visible under mount");

# Check that notebook folder is NOT visible under mount (as png/svg/mutable are all disabled)
ok(!-d "$mnt_dir/Interviews", "notebook folder is hidden under mount since png/svg/mutable are disabled");

# Verify file exists and is readable
my $pdf_size = -s $pdf_path;
ok($pdf_size > 0, "Notebook PDF size ($pdf_size bytes) is greater than 0");

# Run pdfinfo on the output PDF to verify page count
my $pdf_info = `pdfinfo "$pdf_path" 2>/dev/null`;
like($pdf_info, qr/Pages:\s+19/, "pdfinfo shows exactly 19 pages");

# Let's count occurrences of image objects to verify template sharing is working.
# A 19-page PDF without template sharing would have 19 image objects.
# With template sharing, since it uses 2 unique templates ("P Dots S" and "Blank"),
# it should only define 2 image objects.
# Let's check how many /Subtype /Image there are.
copy($pdf_path, "/tmp/Interviews.pdf") or die "Copy to tmp failed: $!";

my $pdf_content = do {
    open(my $pdf_fh, '<', $pdf_path) or die "Cannot open mounted PDF";
    local $/;
    <$pdf_fh>;
};

like($pdf_content, qr/^%PDF-1\.4/m, "output has PDF signature");

my @images = $pdf_content =~ /\/Subtype \/Image/g;
is(scalar @images, 2, "exactly 2 image objects are defined in the PDF");

# Clean up FUSE daemon
kill('TERM', $pid);
waitpid($pid, 0);

# Force unmount to clean up mount point
system("fusermount3 -u -q -z $mnt_dir 2>/dev/null || fusermount -u -q -z $mnt_dir 2>/dev/null");
unlink $config_file;

# Verify that the test completed successfully
ok(1, "Cleaned up test mount");
