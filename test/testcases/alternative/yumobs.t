repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
repo test 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Obs: A < 2
#>=Pkg: B 2 1 noarch
#>=Obs: A < 2
#>=Pkg: B 2 2 noarch
#>=Obs: A < 2
system unset rpm system
solverflags yumobsoletes
job update all packages
job disfavor name B = 2-2
result transaction,problems,alternatives <inline>
#>alternative 80ce092e  0 B, obsoleting A < 2
#>alternative 80ce092e  1 + B-2-1.noarch@test
#>alternative 80ce092e  2   B-2-2.noarch@test
#>install B-2-1.noarch@test
#>upgrade A-1-1.noarch@system A-2-1.noarch@test
