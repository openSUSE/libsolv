#
# A recommends B and B recommends C
#
# With strongrecommends, we get the following
# rules:
#   A -> B
#   B -> C
# after pkg rules sorting, this will be (-B is less than -A)
#   B -> C (weak)
#   A -> B (weak)
# If just the last weak rule is broken, only A will be
# installed but but B. So the code now breaks all weak
# recommends rules.
repo system 0 testtags <inline>
#>=Pkg: X 1 1 noarch
#>=Con: C
repo available 0 testtags <inline>
#>=Pkg: A 1 2 noarch
#>=Rec: B
#>=Pkg: B 1 1 noarch
#>=Rec: C
#>=Pkg: C 1 1 noarch
system i686 rpm system
solverflags strongrecommends
job install name A
result transaction,problems <inline>
#>install A-1-2.noarch@available
#>install B-1-1.noarch@available
