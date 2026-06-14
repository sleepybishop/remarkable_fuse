#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 4;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test parsing a valid v6 file (success case)
system("./remfmt t/assets/test_v6.rm svg > /dev/null");
is($?, 0, 'parsing a valid v6 file succeeds (exit code 0)');

# Test parsing an empty file (failure case)
system("./remfmt /dev/null svg > /dev/null 2>&1");
isnt($?, 0, 'parsing an empty file fails (exit code is non-zero)');

# Test parsing a non-existent file (failure case)
system("./remfmt t/does_not_exist.rm svg > /dev/null 2>&1");
isnt($?, 0, 'parsing a non-existent file fails (exit code is non-zero)');
