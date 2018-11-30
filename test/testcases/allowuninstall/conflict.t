repo system 0 testtags <inline>
#>=Pkg: a 1 1 noarch
#>=Con: b
repo available 0 testtags <inline>
#>=Pkg: b 1 1 noarch

system x86_64 rpm system
solverflags allowuninstall
disable pkg a-1-1.noarch@system
job install name b
result transaction,problems <inline>

