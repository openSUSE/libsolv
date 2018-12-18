# test strong recommends
#
# with normal recommends, the solver will
# not backtrack to fulfill them.
#
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Con: C
#>=Pkg: A2 1 1 noarch
#>=Con: C2
repo available 0 testtags <inline>
#>=Pkg: A 1 2 noarch
#>=Pkg: B 1 1 noarch
#>=Rec: C
#>=Pkg: C 1 1 noarch
#>=Pkg: B2 1 1 noarch
#>=Rec: C2
#>=Pkg: C2 1 1 noarch
#>=Pkg: D 1 1 noarch
#>=Prv: C
#>=Pkg: E 1 1 noarch
#>=Prv: C
system i686 rpm system
solverflags strongrecommends
job install name B
job install name B2
job disfavor name C
result transaction,problems,alternatives <inline>
#>alternative 6b91d100  1 + D-1-1.noarch@available
#>alternative 6b91d100  2   E-1-1.noarch@available
#>install B-1-1.noarch@available
#>install B2-1-1.noarch@available
#>install D-1-1.noarch@available
#>upgrade A-1-1.noarch@system A-1-2.noarch@available
