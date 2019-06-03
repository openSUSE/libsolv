# test that locked packages trump dup rules

repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i686

system i686 * system

job distupgrade all packages
job lock name a
result transaction,problems <inline>

# but we still get a problem if only the available packages
# are locked
#
nextjob
job distupgrade all packages
job lock name a = 2-1
result transaction,problems <inline>
#>problem 1889163e info problem with installed package a-1-1.i686
#>problem 1889163e solution 25ae2253 allow a-1-1.i686@system
#>problem 1889163e solution 06ec856f deljob lock name a = 2-1
#>problem 1889163e solution e5fc66c9 erase a-1-1.i686@system
#>upgrade a-1-1.i686@system a-2-1.i686@available
