repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Rec: X
#>=Pkg: B 1 1 noarch
#>=Prv: X
#>=Pkg: C 1 1 noarch
#>=Prv: X
system unset * system
job install name A
result transaction,problems,alternatives <inline>
#>alternative f1989d4c  0 X, recommended by A-1-1.noarch
#>alternative f1989d4c  1 + B-1-1.noarch@test
#>alternative f1989d4c  2   C-1-1.noarch@test
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test
