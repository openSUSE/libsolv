repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: X
#>=Pkg: B 1 1 noarch
#>=Prv: X
#>=Pkg: C 1 1 noarch
#>=Prv: X
system unset * system
job install name A
result transaction,problems,alternatives <inline>
#>alternative 8f2fa5fa  0 X, required by A-1-1.noarch
#>alternative 8f2fa5fa  1 + B-1-1.noarch@test
#>alternative 8f2fa5fa  2   C-1-1.noarch@test
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test
