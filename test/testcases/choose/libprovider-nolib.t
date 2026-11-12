# no lib*-named provider for a soname: keep the default ordering
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Pkg: foo-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
#>=Pkg: bar-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install bar-bin-1-1.x86_64@test
