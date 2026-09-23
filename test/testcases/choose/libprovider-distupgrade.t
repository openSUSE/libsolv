# distupgrade of an installed non-lib soname provider must keep
# updating it by name and not switch to the lib* package
repo system 0 testtags <inline>
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
repo test 0 testtags <inline>
#>=Pkg: hik-bin 2 1 x86_64
#>=Prv: libfake.so.1()(64bit)
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job distupgrade all packages
result transaction,problems <inline>
#>upgrade hik-bin-1-1.x86_64@system hik-bin-2-1.x86_64@test
