repo system 0 empty
repo test 0 testtags <inline>
#>=Ver: 2.0
#>=Pkg: A 1 1 noarch
#>=Pkg: B 1 1 noarch
#>=Sup: A
#>=Pkg: C 1 1 noarch
#>=Sup: A
#>=Pkg: A2 1 1 noarch
#>=Pkg: B2 1 1 noarch
#>=Sup: A2
#>=Pkg: C2 1 1 noarch
#>=Sup: A2
#>=Con: B2
system unset * system

# first favor B
job install name A
job favor name B
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test
#>install C-1-1.noarch@test

# then favor C
nextjob
job install name A
job favor name C
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test
#>install C-1-1.noarch@test

# same with A2 where B2 and C2 conflict

nextjob
job install name A2
job favor name B2
result transaction,problems <inline>
#>install A2-1-1.noarch@test
#>install B2-1-1.noarch@test

nextjob
job install name A2
job favor name C2
result transaction,problems <inline>
#>install A2-1-1.noarch@test
#>install C2-1-1.noarch@test


# check disfavor 
nextjob
job install name A
job disfavor name B
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install C-1-1.noarch@test

nextjob
job install name A
job disfavor name C
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test

nextjob
job install name A
job disfavor name B
job disfavor name C
result transaction,problems <inline>
#>install A-1-1.noarch@test
