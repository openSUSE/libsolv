# test dup with orphaned packages
#
# part 3: a is not really an orphan, but cannot be downgraded
#

repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 1 0 i686
#>=Pkg: b 2 1 i686
system i686 * system

solverflags !dupallowdowngrade
job distupgrade all packages
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available
#>problem 7724e627 info problem with installed package a-1-1.i686
#>problem 7724e627 solution 25ae2253 allow a-1-1.i686@system
#>problem 7724e627 solution 2cf4745c replace a-1-1.i686@system a-1-0.i686@available

nextjob

solverflags !dupallowdowngrade
job distupgrade repo available
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available
#>problem 7724e627 info problem with installed package a-1-1.i686
#>problem 7724e627 solution 25ae2253 allow a-1-1.i686@system
#>problem 7724e627 solution 2cf4745c replace a-1-1.i686@system a-1-0.i686@available

### keeporphans

nextjob

solverflags !dupallowdowngrade keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available
#>problem 7724e627 info problem with installed package a-1-1.i686
#>problem 7724e627 solution 25ae2253 allow a-1-1.i686@system
#>problem 7724e627 solution 2cf4745c replace a-1-1.i686@system a-1-0.i686@available

nextjob

solverflags !dupallowdowngrade keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available
#>problem 7724e627 info problem with installed package a-1-1.i686
#>problem 7724e627 solution 25ae2253 allow a-1-1.i686@system
#>problem 7724e627 solution 2cf4745c replace a-1-1.i686@system a-1-0.i686@available

### same with allowuninstall

nextjob

solverflags !dupallowdowngrade allowuninstall
job distupgrade all packages
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags !dupallowdowngrade allowuninstall
job distupgrade repo available
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


### same with allowuninstall and keeporphans

nextjob

solverflags !dupallowdowngrade allowuninstall keeporphans
job distupgrade all packages
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available


nextjob

solverflags !dupallowdowngrade allowuninstall keeporphans
job distupgrade repo available
result transaction,problems <inline>
#>downgrade a-1-1.i686@system a-1-0.i686@available
#>upgrade b-1-1.i686@system b-2-1.i686@available



