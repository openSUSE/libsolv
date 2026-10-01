# prefer the lib*-named provider when resolving a soname dependency
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libz.so.1()(64bit)
#>=Pkg: hik-bin 1.0 1 x86_64
#>=Prv: libz.so.1()(64bit)
#>=Pkg: lib64z1 1.2.13 2 x86_64
#>=Prv: libz.so.1()(64bit)
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install lib64z1-1.2.13-2.x86_64@test
