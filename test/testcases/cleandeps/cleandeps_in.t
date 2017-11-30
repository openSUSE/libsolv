repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: B1
#>=Pkg: B1 1 1 noarch
repo test 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Req: B2 = 1
#>=Pkg: B1 1 1 noarch
#>=Pkg: B2 1 1 noarch
system i686 rpm system

# check targeted
job install name A = 2 [cleandeps]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test

# check orupdate
nextjob
job install name A [cleandeps,orupdate]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test

# check installalsoupdates
nextjob
solverflags installalsoupdates
job install name A [cleandeps]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test
