# the id hashes survive pool_createwhatprovides(); the job interns new
# strings and relations into them afterwards, which must neither lose
# nor duplicate ids
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libfoo = 1
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Prv: libfoo = 2
#>=Pkg: B 1 1 noarch
#>=Req: libfoo > 1
#>=Pkg: C 1 1 noarch
#>=Req: libbar
#>=Pkg: D 1 1 noarch
#>=Prv: libbar = 3
system i686 rpm system
poolflags keepidhashes

job install provides libfoo >= 2
job install name B
job install provides libbar > 2
job install name C
result transaction,problems <inline>
#>install B-1-1.noarch@available
#>install C-1-1.noarch@available
#>install D-1-1.noarch@available
#>upgrade A-1-1.noarch@system A-2-1.noarch@available
