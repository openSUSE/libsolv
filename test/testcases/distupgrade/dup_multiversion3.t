# test dup with multiversion packages where we cannot install the
# target. Should give problems except for allowuninstall.
#
# part 1: simple update
repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i686
#>=Req: c
system i686 * system

job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>problem fc3d647e info nothing provides c needed by a-2-1.i686
#>problem fc3d647e solution 179b72ed allow a-1-1.i686@system
#>problem fc3d647e solution e5fc66c9 erase a-1-1.i686@system

nextjob

job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>problem fc3d647e info nothing provides c needed by a-2-1.i686
#>problem fc3d647e solution 179b72ed allow a-1-1.i686@system
#>problem fc3d647e solution e5fc66c9 erase a-1-1.i686@system

### same with keeporphans

nextjob

solverflags keeporphans
job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>problem 771581fd info nothing provides c needed by a-2-1.i686
#>problem 771581fd solution 179b72ed allow a-1-1.i686@system
#>problem 771581fd solution 2cf4745c erase a-1-1.i686@system

nextjob

solverflags keeporphans
job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>problem 771581fd info nothing provides c needed by a-2-1.i686
#>problem 771581fd solution 179b72ed allow a-1-1.i686@system
#>problem 771581fd solution 2cf4745c erase a-1-1.i686@system

### same with allowuninstall

nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system


nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system


### same with allowuninstall and keeporphans

nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system


nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system


