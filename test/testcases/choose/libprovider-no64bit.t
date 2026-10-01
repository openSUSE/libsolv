# a 32-bit soname dependency without the ()(64bit) marker
# (32-bit ROSA packages are named lib*, not lib64*)
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 i686
#>=Req: libfake.so.1
#>=Pkg: hik-bin 1 1 i686
#>=Prv: libfake.so.1
#>=Pkg: libfake1 1 1 i686
#>=Prv: libfake.so.1
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.i686@test
#>install libfake1-1-1.i686@test
