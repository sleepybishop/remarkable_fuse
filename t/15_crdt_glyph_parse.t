#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 2;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Test parsing a v6 file that contains SceneGlyphItemBlock
# This should succeed without exiting prematurely or crashing
system("./remfmt t/assets/test_v6_glyph.rm svg > /dev/null 2>&1");
is($?, 0, 'parsing a v6 file with glyph blocks succeeds (exit code 0)');
