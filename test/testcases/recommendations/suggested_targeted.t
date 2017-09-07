repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Sug: B = 2-1
#>=Pkg: B 1 1 noarch
#>=Pkg: B 3 1 noarch
#>=Pkg: B 2 1 noarch
system i686 rpm system

solverflags ignorerecommended
job install name A

result transaction,recommended,problems <inline>
#>install A-1-1.noarch@available
#>suggested B-2-1.noarch@available
