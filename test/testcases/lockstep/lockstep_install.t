repo system 0 empty
repo test 0 testtags <inline>
#>=Pkg: A 1 1 i586
#>=Prv: A(x32)
#>=Pkg: A 1 1 x86_64
#>=Prv: A(x64)
system x86_64 rpm system
poolflags implicitobsoleteusescolors
job install provides A(x32)
result transaction,problems <inline>
#>install A-1-1.i586@test
#>install A-1-1.x86_64@test
