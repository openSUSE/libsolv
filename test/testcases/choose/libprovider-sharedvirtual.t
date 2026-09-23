# a non-soname dependency whose providers coincidentally bundle the
# same library must keep the default ordering: the shared virtual
# name hints that the dependency is not the soname
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 x86_64
#>=Req: browser-engine
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: browser-engine
#>=Prv: libfake.so.1()(64bit)
#>=Pkg: lib64webkit 1 1 x86_64
#>=Prv: browser-engine
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name A
result transaction,problems <inline>
#>install A-1-1.x86_64@test
#>install hik-bin-1-1.x86_64@test
