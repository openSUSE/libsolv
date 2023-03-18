repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Rec: X
#>=Pkg: B 1 1 noarch
#>=Prv: X
#>=Pkg: C 1 1 noarch
#>=Prv: X
#>=Pkg: D 1 1 noarch
#>=Prv: X
system unset * system
solverflags strongrecommends
job install name A
result transaction,problems,alternatives <inline>
#>alternative 432b0214  0 X, recommended by A-1-1.noarch
#>alternative 432b0214  1 + B-1-1.noarch@test
#>alternative 432b0214  2   C-1-1.noarch@test
#>alternative 432b0214  3   D-1-1.noarch@test
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test

nextjob
solverflags strongrecommends
job install name A
job disfavor name B
result transaction,problems,alternatives <inline>
#>alternative eb7c0cd8  0 X, recommended by A-1-1.noarch
#>alternative eb7c0cd8  1 + C-1-1.noarch@test
#>alternative eb7c0cd8  2   D-1-1.noarch@test
#>install A-1-1.noarch@test
#>install C-1-1.noarch@test
