repo system 0 testtags <inline>
#>=Pkg: X 1 1 noarch
repo test 0 testtags <inline>
#>=Pkg: X 2 1 noarch
#>=Pkg: B 1 1 noarch
#>=Obs: X = 1-1
system unset * system
job update all packages
result transaction,problems,alternatives <inline>
#>alternative 6014509b  0 X-1-1.noarch
#>alternative 6014509b  1 + X-2-1.noarch@test
#>alternative 6014509b  2   B-1-1.noarch@test
#>upgrade X-1-1.noarch@system X-2-1.noarch@test

nextjob
job distupgrade all packages [forcebest]
result transaction,problems,alternatives <inline>
#>alternative 6014509b  0 X-1-1.noarch
#>alternative 6014509b  1 + X-2-1.noarch@test
#>alternative 6014509b  2   B-1-1.noarch@test
#>upgrade X-1-1.noarch@system X-2-1.noarch@test
