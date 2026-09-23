# Suggests only reorders candidates while the soname lib*-name
# preference prunes, so lib64fake1 must win over the suggested
# hik-bin. This is deliberate: a Suggests is written for the
# package as a whole, not for satisfying a specific soname
# dependency, so it stays weaker than the lib*-name preference
# (unlike Recommends, see libprovider-recommended.t).
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Sug: hik-bin
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name A
result transaction,problems <inline>
#>install A-1-1.x86_64@test
#>install lib64fake1-1-1.x86_64@test
