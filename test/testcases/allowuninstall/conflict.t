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
#>problem a658cbaf info package a-1-1.noarch conflicts with b provided by b-1-1.noarch
#>problem a658cbaf solution 567aa15d erase a-1-1.noarch@system
#>problem a658cbaf solution e98e1a37 deljob install name b
