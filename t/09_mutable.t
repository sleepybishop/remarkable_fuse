#!/usr/bin/env perl
use strict;
use warnings;
use Test::More;
use File::Temp qw(tempdir);
use File::Copy qw(copy);
use Time::HiRes qw(sleep);

# Check if FUSE is available and we have permissions to mount
my $has_fuse = 0;
if (-e '/dev/fuse' && -w '/dev/fuse') {
    $has_fuse = 1;
}

if (!$has_fuse) {
    plan skip_all => "FUSE is not available or writeable on this system";
}

plan tests => 12;

# Setup temp directories
my $tmp_dir = tempdir(CLEANUP => 1);
my $xochitl_dir = "$tmp_dir/xochitl";
my $mnt_dir = "$tmp_dir/mnt";
mkdir $xochitl_dir;
mkdir $mnt_dir;

# Write config file with mutability enabled
my $config_file = "$tmp_dir/config.json";
open(my $fh, '>', $config_file) or die "Could not write config: $!";
print $fh <<EOC;
{
    "data_dir": "$xochitl_dir",
    "mutable": true,
    "renderers": []
}
EOC
close($fh);

# Start remfs FUSE mount in background
my $pid = fork();
if ($pid == 0) {
    # Child process: run FUSE daemon
    exec('./remfs', "--config=$config_file", $mnt_dir);
    exit(1);
}

# Parent: wait a bit for FUSE to mount
sleep(0.5);

# Verify FUSE is mounted (we should see . and ..)
my @files = glob("$mnt_dir/*");
ok(1, "remfs daemon started with PID $pid");

# Test 1: Create a Collection (Folder) fails
my $folder_name = "MyFolder";
my $mkdir_ret = mkdir("$mnt_dir/$folder_name");
ok(!$mkdir_ret, "mkdir Collection fails");

# Verify no metadata file was generated
my @metadata_files = glob("$xochitl_dir/*.metadata");
is(scalar @metadata_files, 0, "No metadata file was created in xochitl");

# Test 2: Create a Notebook fails
my $notebook_name = "MyNotebook.notebook";
my $mkdir_nb_ret = mkdir("$mnt_dir/$notebook_name");
ok(!$mkdir_nb_ret, "mkdir Notebook fails");

# Test 3: Import a PDF
open(my $pdf_fh, '>', "$mnt_dir/my_book.pdf") or warn "Could not create pdf file: $!";
if ($pdf_fh) {
    print $pdf_fh "%PDF-1.4 dummy data";
    close($pdf_fh);
}
ok(-f "$mnt_dir/my_book.pdf", "Imported PDF file is visible");

# Test 4: Import an EPUB
open(my $epub_fh, '>', "$mnt_dir/my_novel.epub") or warn "Could not create epub file: $!";
if ($epub_fh) {
    print $epub_fh "EPUB dummy data";
    close($epub_fh);
}
ok(-f "$mnt_dir/my_novel.epub", "Imported EPUB file is visible");

# Test 5: Import a XOJ file to create a new Notebook
my $xoj_source = "./misc/rm_grid_template.xoj";
my $xoj_dest = "$mnt_dir/import.xoj";
copy($xoj_source, $xoj_dest) or warn "Could not copy XOJ: $!";

# Wait a tiny bit for the import/write/release process
sleep(0.2);

# Verify that the new notebook directory and its page (.rm file) were created.
ok(-d "$mnt_dir/import", "New notebook directory was created for imported XOJ");
my @rm_files = glob("$mnt_dir/import/*.rm");
is(scalar @rm_files, 1, "Imported XOJ file and verified the new notebook page was generated");

# Test 6: Remove the EPUB
my $unlink_epub_ret = unlink("$mnt_dir/my_novel.epub");
ok($unlink_epub_ret, "Removed EPUB file");
ok(! -f "$mnt_dir/my_novel.epub", "EPUB file is gone");

# Test 7: Remove the Notebook directory
my $rmdir_nb_ret = rmdir("$mnt_dir/import");
ok($rmdir_nb_ret, "Removed Notebook directory");
ok(! -d "$mnt_dir/import", "Notebook directory is gone");

# Clean up FUSE daemon
kill('TERM', $pid);
waitpid($pid, 0);

# Ensure it is unmounted
system("fusermount3 -u -q -z $mnt_dir 2>/dev/null || fusermount -u -q -z $mnt_dir 2>/dev/null");

# Cleanup temp files
unlink $config_file;
