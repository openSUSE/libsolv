repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: X
#>=Pkg: B 1 1 noarch
#>=Prv: X
system unset * system
job install provides X
result transaction,problems,alternatives <inline>
#>alternative 63535fbb  0 X
#>alternative 63535fbb  1 + A-1-1.noarch@test
#>alternative 63535fbb  2   B-1-1.noarch@test
#>install A-1-1.noarch@test

nextjob
job install oneof A-1-1.noarch@test B-1-1.noarch@test
result transaction,problems,alternatives <inline>
#>alternative 63535fbb  0 A-1-1.noarch, B-1-1.noarch
#>alternative 63535fbb  1 + A-1-1.noarch@test
#>alternative 63535fbb  2   B-1-1.noarch@test
#>install A-1-1.noarch@test

