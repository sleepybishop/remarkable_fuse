#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 2;

ok(-x './t/test_remfs', 'test_remfs binary exists');

my $output = `./t/test_remfs`;
is($?, 0, 'remfs parses .pagedata successfully and assigns template names to pages');
