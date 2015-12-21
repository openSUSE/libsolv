# test dup with multiversion packages
# same as with dup_multiversion1, but we can't keep the orphan

#
# part 1: simple update
repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i686
#>=Pkg: b 2 1 i686
#>=Con: a = 1-1
system i686 * system

job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob

job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with keeporphans, this will result in problems as we cannot keep the orphan

nextjob

solverflags keeporphans
job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>install a-2-1.i686@available
#>problem 4d4de423 info package b-2-1.i686 conflicts with a = 1-1 provided by a-1-1.i686
#>problem 4d4de423 solution 2cf4745c erase a-1-1.i686@system
#>problem 4d4de423 solution 2cf4745c replace a-1-1.i686@system a-2-1.i686@available
#>problem 4d4de423 solution 5a433aff allow b-1-1.i686@system
#>problem 4d4de423 solution ce4305f2 erase b-1-1.i686@system

nextjob

solverflags keeporphans
job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>install a-2-1.i686@available
#>problem 4d4de423 info package b-2-1.i686 conflicts with a = 1-1 provided by a-1-1.i686
#>problem 4d4de423 solution 2cf4745c erase a-1-1.i686@system
#>problem 4d4de423 solution 2cf4745c replace a-1-1.i686@system a-2-1.i686@available
#>problem 4d4de423 solution 5a433aff allow b-1-1.i686@system
#>problem 4d4de423 solution ce4305f2 erase b-1-1.i686@system

### same with allowuninstall

nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade all packages
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade repo available
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with allowuninstall and keeporphans

nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade all packages
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available



nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade repo available
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>erase a-1-1.i686@system
#>install a-2-1.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


