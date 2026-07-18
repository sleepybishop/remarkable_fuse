#!/usr/bin/env perl
use strict;
use warnings;
use Test::More tests => 5;

# Check if remfmt binary exists
ok(-x './remfmt', 'remfmt binary exists and is executable');

# Construct a test v6 file with SceneAssetBlock (0x0E) and ScenePathItemBlock (0x0F)
my $magic = "reMarkable .lines file, version=6          "; # 43 bytes
my $block_0e_hex = "620000000003030e1c5d000000010c570000004be9f7ad407a129d3e4dc2316b84c8251c330000001f0290062c2a000000280135663830333635642d393732652d343135642d383033302d3666346465653133616339322e6a70672c0a0000001f00002c020000001100";
my $block_0f_hex = "a40000000002020f1f0296052f01eb063f02bf054f000054000000006c8b000000071c190000001f0292062c100000004be9f7ad407a129d3e4dc2316b84c8252f018d073c4100000010002071428094e341000000000000000035684d448094e3410000803f0000000035684d44d97245440000803f0000803f00207142d9724544000000000000803f4c19000000060000000001000000020000000200000003000000000000005f02b906";

my $file_content = $magic . pack("H*", $block_0e_hex . $block_0f_hex);

my $tmp_file = "/tmp/test_shapes_v6.rm";
open(my $fh, '>', $tmp_file) or die "Could not open $tmp_file: $!";
binmode($fh);
print $fh $file_content;
close($fh);

# Test parsing the shapes file and rendering to SVG
my $output_svg = `./remfmt $tmp_file svg`;
is($?, 0, 'remfmt svg exit code is 0');
like($output_svg, qr/<svg/i, 'output contains svg opening tag');
like($output_svg, qr/<image /i, 'output contains image element for inserted shape');
like($output_svg, qr/href="5f80365d-972e-415d-8030-6f4dee13ac92\.jpg"/i, 'image element references correct filename');

unlink($tmp_file);
