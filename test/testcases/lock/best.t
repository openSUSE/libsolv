# test that locked packages trump best rules

repo system 0 testtags <inline>
#>=Pkg: b 1 1 i686
repo available 0 testtags <inline>
#>=Pkg: a 2 1 i686
#>=Pkg: a 3 1 i686
#>=Pkg: b 2 1 i686
#>=Pkg: b 3 1 i686

system i686 * system

job install name a [forcebest]
job lock name a = 3-1
result transaction,problems <inline>
#>install a-2-1.i686@available

nextjob
job update name b [forcebest]
job lock name b = 3-1
result transaction,problems <inline>
#>upgrade b-1-1.i686@system b-2-1.i686@available

nextjob
job update name b [forcebest]
job lock name b = 1-1
result transaction,problems <inline>
