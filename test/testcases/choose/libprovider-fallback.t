# fall back to a non-lib provider if the preferred lib* provider
# is uninstallable (here: has an unsatisfiable dependency itself)
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
#>=Req: missing.so.1()(64bit)
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install hik-bin-1-1.x86_64@test
