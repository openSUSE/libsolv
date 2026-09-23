# same as libprovider-sharedvirtual.t, but the dependency comes
# from the job (install by provides): the lib*-name preference must
# not fire for a non-soname capability
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: browser-engine
#>=Prv: libfake.so.1()(64bit)
#>=Pkg: lib64webkit 1 1 x86_64
#>=Prv: browser-engine
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install provides browser-engine
result transaction,problems <inline>
#>install hik-bin-1-1.x86_64@test
