# an installed non-lib provider already satisfies the soname:
# nothing extra must be pulled in
repo system 0 testtags <inline>
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
