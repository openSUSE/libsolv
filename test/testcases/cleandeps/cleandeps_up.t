repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: B1
#>=Pkg: B1 1 1 noarch
#>=Pkg: C 1 1 noarch
#>=Rec: D
#>=Pkg: D 1 1 noarch
repo test 0 testtags <inline>
#>=Pkg: A 1 2 noarch
#>=Req: B1
#>=Pkg: A 2 1 noarch
#>=Req: B2 = 1
#>=Pkg: B1 1 1 noarch
#>=Pkg: B2 1 1 noarch
#>=Pkg: C 1 1 noarch
#>=Rec: D
system i686 rpm system

# check untargeted
job update name A [cleandeps]
job update name C [cleandeps]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test

# check targeted
nextjob
job update name A = 2 [cleandeps]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test

# check targeted to 1-2
nextjob
job update name A = 1-2 [cleandeps]
result transaction,problems,cleandeps <inline>
#>upgrade A-1-1.noarch@system A-1-2.noarch@test

# check all packages
nextjob
job update all packages [cleandeps]
result transaction,problems,cleandeps <inline>
#>cleandeps B1-1-1.noarch@system
#>erase B1-1-1.noarch@system
#>install B2-1-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test
