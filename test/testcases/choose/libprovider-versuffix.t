# a soname with the version before ".so" (libfake-1.so),
# as shipped by some upstreams
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake-1.so
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake-1.so
#>=Pkg: lib64fake 1 1 x86_64
#>=Prv: libfake-1.so
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install lib64fake-1-1.x86_64@test
