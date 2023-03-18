repo system 0 testtags <inline>
#>=Ver: 2.0
#>=Pkg: c 27 1 x86_64
repo available 0 testtags <inline>
#>=Ver: 2.0
#>=Pkg: d 28 1 x86_64
#>=Obs: c
#>=Pkg: e 28 1 x86_64
#>=Obs: c
#>=Pkg: f 28 1 x86_64
#>=Obs: c < 30
#>=Pkg: g 28 1 x86_64
#>=Obs: c < 30

system x86_64 rpm system
solverflags yumobsoletes
job update all packages
result transaction,problems,alternatives <inline>
#>alternative 6234e4e8  0 c-27-1.x86_64
#>alternative 6234e4e8  1 + d-28-1.x86_64@available
#>alternative 6234e4e8  2   f-28-1.x86_64@available
#>erase c-27-1.x86_64@system d-28-1.x86_64@available
#>install d-28-1.x86_64@available
#>install e-28-1.x86_64@available
