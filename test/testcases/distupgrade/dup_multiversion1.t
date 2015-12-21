# test dup with multiversion packages
#
# part 1: simple update
repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i686
system i686 * system

job multiversion name a
job distupgrade all packages
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available

nextjob

job multiversion name a
job distupgrade repo available
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


### same with keeporphans

nextjob

solverflags keeporphans
job multiversion name a
job distupgrade all packages
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


nextjob

solverflags keeporphans
job multiversion name a
job distupgrade repo available
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


### same with allowuninstall

nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade all packages
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


nextjob

solverflags allowuninstall
job multiversion name a
job distupgrade repo available
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


### same with allowuninstall and keeporphans

nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade all packages
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available


nextjob

solverflags allowuninstall keeporphans
job multiversion name a
job distupgrade repo available
# a-1-1 is treated as orphaned and stays behind
result transaction,problems <inline>
#>install a-2-1.i686@available



