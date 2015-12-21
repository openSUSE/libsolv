# test dup with orphaned packages
#
# part 2: update conflicts with the orphan
#
# dup should leave orphaned a installed
# for "distupgrade repo available", a is not involved
# in the dup and thus not considered orphan.
#

repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: b 2 1 i686
#>=Con: a
system i686 * system

job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob

job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>problem 4d4de423 info package b-2-1.i686 conflicts with a provided by a-1-1.i686
#>problem 4d4de423 solution 2cf4745c erase a-1-1.i686@system
#>problem 4d4de423 solution 5a433aff allow b-1-1.i686@system
#>problem 4d4de423 solution ce4305f2 erase b-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available

### keeporphans

nextjob

solverflags keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>problem 4d4de423 info package b-2-1.i686 conflicts with a provided by a-1-1.i686
#>problem 4d4de423 solution 2cf4745c erase a-1-1.i686@system
#>problem 4d4de423 solution 5a433aff allow b-1-1.i686@system
#>problem 4d4de423 solution ce4305f2 erase b-1-1.i686@system

nextjob

solverflags keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>problem 4d4de423 info package b-2-1.i686 conflicts with a provided by a-1-1.i686
#>problem 4d4de423 solution 2cf4745c erase a-1-1.i686@system
#>problem 4d4de423 solution 5a433aff allow b-1-1.i686@system
#>problem 4d4de423 solution ce4305f2 erase b-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with allowuninstall

nextjob

solverflags allowuninstall
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags allowuninstall
job distupgrade repo available
result transaction,problems <inline>
#>erase b-1-1.i686@system


### same with allowuninstall and keeporphans

nextjob

solverflags allowuninstall keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags allowuninstall keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>upgrade b-1-1.i686@system b-2-1.i686@available



