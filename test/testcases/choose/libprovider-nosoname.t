# no lib*-name preference for non-soname dependencies
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: Y
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: Y
#>=Pkg: lib64foo2 1 1 x86_64
#>=Prv: Y
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@test
#>install hik-bin-1-1.x86_64@test
