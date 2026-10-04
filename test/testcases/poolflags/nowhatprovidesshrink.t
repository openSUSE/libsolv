# without the unification, ids with identical provider lists keep
# separate copies of the list; lookups must give the same answers
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>+Prv:
#>cap1
#>cap2
#>cap3
#>-Prv:
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>+Prv:
#>cap1
#>cap2
#>cap3
#>-Prv:
#>=Pkg: B 1 1 noarch
#>+Prv:
#>cap1
#>cap2
#>cap3
#>-Prv:
#>=Pkg: C 1 1 noarch
#>=Req: cap2
#>=Con: B
#>=Pkg: D 1 1 noarch
#>=Req: cap3
system i686 rpm system
poolflags nowhatprovidesshrink

job install name C
job install name D
job install provides cap1
result transaction,problems <inline>
#>install C-1-1.noarch@available
#>install D-1-1.noarch@available
