#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 6;

ok(-x './misc/xoj2rm.py', 'xoj2rm.py script exists and is executable');

# Test 1: Compressed file (.xoj)
my $output = `python3 ./misc/xoj2rm.py ./misc/rm_grid_template.xoj /tmp/test_xoj.rm 2>&1`;
is($?, 0, 'xoj2rm.py runs successfully with compressed input');

my $size = -s '/tmp/test_xoj.rm';
ok(defined $size && $size >= 43, 'generated .rm file from compressed input has valid size');
unlink '/tmp/test_xoj.rm';

# Test 2: Uncompressed file
my $decompress = `gunzip -c ./misc/rm_grid_template.xoj > /tmp/test_uncompressed.xoj 2>&1`;
is($?, 0, 'gunzip of template file succeeds');

my $output2 = `python3 ./misc/xoj2rm.py /tmp/test_uncompressed.xoj /tmp/test_xoj2.rm 2>&1`;
is($?, 0, 'xoj2rm.py runs successfully with uncompressed input');

my $size2 = -s '/tmp/test_xoj2.rm';
ok(defined $size2 && $size2 >= 43, 'generated .rm file from uncompressed input has valid size');

unlink '/tmp/test_uncompressed.xoj';
unlink '/tmp/test_xoj2.rm';
