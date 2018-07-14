# test dup with orphaned packages
#
# part 1: simple update
#
# dup should leave orphaned a installed
#
repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: b 2 1 i686
system i686 * system

job distupgrade all packages
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob

job distupgrade repo available
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with keeporphans

nextjob

solverflags keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob

solverflags keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with allowuninstall

nextjob

solverflags allowuninstall
job distupgrade all packages
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags allowuninstall
job distupgrade repo available
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with allowuninstall and keeporphans

nextjob

solverflags allowuninstall keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags allowuninstall keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available



