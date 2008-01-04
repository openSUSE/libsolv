#!/usr/bin/perl

use lib '/usr/share/kiwi/modules';

use strict;
use KIWIXML;

our $BasePath = "/usr/share/kiwi";
our $Scheme   = $BasePath."/modules/KIWIScheme.rng";

my $kiwi = new KIWILog ("tiny");
my $xml  = new KIWIXML ($kiwi,"/usr/share/kiwi/image/isoboot/suse-10.3");

#my @list = ("http://download.opensuse.org/distribution/10.3/repo/oss");
my @list = ("/image/CDs/full-10.3-i386");
my $data = $xml -> getInstSourceSatSolvable (\@list);

if (defined $data) {
	print "$data\n";
}
