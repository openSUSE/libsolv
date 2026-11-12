# like Suggests (see libprovider-suggested.t), an Enhances on the
# non-lib provider stays weaker than the soname lib*-name preference
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
#>=Enh: A
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name A
result transaction,problems <inline>
#>install A-1-1.x86_64@test
#>install lib64fake1-1-1.x86_64@test
