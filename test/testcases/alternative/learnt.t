### or in CC1
####>=Con: CC2 <UNLESS> (A1 | A2 | A3)
###
feature complex_deps
repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: Q 1 1 noarch
#>=Req: A
#>=Pkg: A1 1 1 noarch
#>=Pkg: A2 1 2 noarch
#>=Pkg: A3 1 3 noarch
#>=Req: C = 2-1
#>=Pkg: A 2 1 noarch
#>=Con: A1
#>=Con: A2
#>=Con: A3
#>=Req: B
#>=Pkg: B 1 1 noarch
#>=Req: X
#>=Pkg: C 2 1 noarch
#>=Prv: X
#>=Req: CC1
#>=Req: CC2
#>=Pkg: CC1 1 1 noarch
#>=Pkg: CC2 1 1 noarch
#>=Req: (A1 | A2 | A3) <IF> CC1
#>=Pkg: C 2 2 noarch
#>=Prv: X
#>=Req: CC1
#>=Req: CC2
#>=Pkg: A 1 1 noarch
#>=Req: C = 2-2
system unset rpm system
job install name Q
result transaction,problems,alternatives <inline>
#>alternative d80be72d  0 ((A1 or A2 or A3) if CC1), required by CC2-1-1.noarch
#>alternative d80be72d  1 + A1-1-1.noarch@test
#>alternative d80be72d  2   A2-1-2.noarch@test
#>install A-1-1.noarch@test
#>install A1-1-1.noarch@test
#>install C-2-2.noarch@test
#>install CC1-1-1.noarch@test
#>install CC2-1-1.noarch@test
#>install Q-1-1.noarch@test
