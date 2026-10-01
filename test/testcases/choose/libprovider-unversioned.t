# an unversioned soname: some upstreams ship libraries with
# no version in the soname or with the version before ".so"
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake.so
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so
#>=Pkg: lib64fake 1 1 x86_64
#>=Prv: libfake.so
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install lib64fake-1-1.x86_64@test
