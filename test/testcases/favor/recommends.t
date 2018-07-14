repo system 0 empty
repo test 0 testtags <inline>
#>=Ver: 2.0
#>=Pkg: A 1 1 noarch
#>=Rec: X
#>=Pkg: B 1 1 noarch
#>=Prv: X
#>=Pkg: C 1 1 noarch
#>=Prv: X
system unset * system

# first favor B
job install name A
job favor name B
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install B-1-1.noarch@test

# then favor C
nextjob
job install name A
job favor name C
result transaction,problems <inline>
#>install A-1-1.noarch@test
#>install C-1-1.noarch@test

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

# check disfavor both, this is different from
# the requires case

nextjob
job install name A
job disfavor name B
job disfavor name C
result transaction,problems <inline>
#>install A-1-1.noarch@test
