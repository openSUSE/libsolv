# a "lib"-prefixed non-soname virtual dep: no lib*-name preference
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install hik-bin-1-1.x86_64@test
