repo system 0 testtags <inline>
#>=Pkg: a 1 1 i686
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i586
#>=Pkg: b 2 1 i586
#>=Pkg: b 2 1 i686
system i686 * system
solverflags !dupallowarchchange
job distupgrade all packages
result transaction,problems <inline>
#>problem 7724e627 info problem with installed package a-1-1.i686
#>problem 7724e627 solution 25ae2253 allow a-1-1.i686@system
#>problem 7724e627 solution 2cf4745c replace a-1-1.i686@system a-2-1.i586@available
#>upgrade a-1-1.i686@system a-2-1.i586@available
#>upgrade b-1-1.i686@system b-2-1.i686@available
