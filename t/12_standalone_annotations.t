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

# Test Case 1: standalone_annotations => false
my $config_file = "$tmp_dir/config.json";
open(my $fh, '>', $config_file) or die "Could not write config: $!";
print $fh <<EOF;
{
    "data_dir": "$xochitl_dir",
    "mutable": false,
    "renderers": ["pdf"],
    "standalone_annotations": false
}
EOF
close($fh);

# Start remfs FUSE mount in background
my $pid = fork();
if ($pid == 0) {
    exec('./remfs', "--config=$config_file", $mnt_dir);
    exit(1);
}
sleep(0.5);

ok(-f "$mnt_dir/MyPDF.pdf", "PDF file is visible");
ok(!-d "$mnt_dir/MyPDF Annotations", "Annotations directory is NOT visible when disabled");
ok(!-f "$mnt_dir/MyPDF Annotations/pdf/page_000001.pdf", "Individual PDF page is NOT accessible when disabled");

# Kill FUSE daemon
kill('TERM', $pid);
waitpid($pid, 0);
system("fusermount3 -u -q -z $mnt_dir 2>/dev/null || fusermount -u -q -z $mnt_dir 2>/dev/null");

# Test Case 2: standalone_annotations => true
open($fh, '>', $config_file) or die "Could not write config: $!";
print $fh <<EOF;
{
    "data_dir": "$xochitl_dir",
    "mutable": false,
    "renderers": ["pdf"],
    "standalone_annotations": true
}
EOF
close($fh);

# Start remfs FUSE mount in background
$pid = fork();
if ($pid == 0) {
    exec('./remfs', "--config=$config_file", $mnt_dir);
    exit(1);
}
sleep(0.5);

ok(-f "$mnt_dir/MyPDF.pdf", "PDF file is visible");
ok(-d "$mnt_dir/MyPDF Annotations", "Annotations directory is visible when enabled");
ok(-d "$mnt_dir/MyPDF Annotations/pdf", "pdf subdirectory is visible");
ok(-f "$mnt_dir/MyPDF Annotations/pdf/page_000001.pdf", "Individual PDF page is accessible when enabled");

# Kill FUSE daemon
kill('TERM', $pid);
waitpid($pid, 0);
system("fusermount3 -u -q -z $mnt_dir 2>/dev/null || fusermount -u -z $mnt_dir 2>/dev/null");

unlink $config_file;
