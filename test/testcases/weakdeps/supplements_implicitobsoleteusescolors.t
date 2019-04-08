repo system 0 empty
repo test 0 testtags <inline>
#>=Ver: 2.0
#>=Pkg: A 1 1 noarch
#>=Pkg: B 1 1 x86_64
#>=Sup: A
#>=Pkg: B 1 1 i686
#>=Sup: A
#>=Pkg: A2 1 1 noarch
#>=Pkg: B2 1 1 x86_64
#>=Sup: A2
#>=Req: XX
#>=Pkg: B2 1 1 i686
#>=Sup: A2
system x86_64 rpm system
poolflags implicitobsoleteusescolors
job install name A
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install B-1-1.x86_64@test

nextjob
job install name A2
result transaction,problems <inline>
#>install A2-1-1.noarch@test


