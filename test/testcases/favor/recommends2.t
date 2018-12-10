repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: a 1 1 noarch
#>=Req: b
#>=Rec: b1
#>=Pkg: b1 1 1 noarch
#>=Prv: b
#>=Pkg: b2 1 1 noarch
#>=Prv: b
system x86_64 rpm system
poolflags implicitobsoleteusescolors
solverflags ignorerecommended
job install pkg a-1-1.noarch@available
job favor name b2
result transaction,problems,alternatives <inline>
#>alternative 64eb4d87  0 a-1-1.noarch@available requires b
#>alternative 64eb4d87  1 + b2-1-1.noarch@available
#>alternative 64eb4d87  2   b1-1-1.noarch@available
#>install a-1-1.noarch@available
#>install b2-1-1.noarch@available
