repo system 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
repo available 0 testtags <inline>
#>=Pkg: A 1 1 x86_64
#>=Req: X
#>=Pkg: B 1 1 x86_64
job install name A
result transaction,problems,rules <inline>
#>install A-1-1.x86_64@available
#>install X-1-1.x86_64@system
#>rule job 300db6ce502dde94261e267a8c535441  A-1-1.x86_64@available
#>rule pkg 11c27e407a56aad27bd6b3eadc17374b  X-1-1.x86_64@system
#>rule pkg 11c27e407a56aad27bd6b3eadc17374b -A-1-1.x86_64@available
nextjob reusesolver
job install name B
result transaction,problems,rules <inline>
#>install B-1-1.x86_64@available
#>rule job ad168c1819736b8aa6f507ab075b3494  B-1-1.x86_64@available
#>rule pkg 11c27e407a56aad27bd6b3eadc17374b  X-1-1.x86_64@system
#>rule pkg 11c27e407a56aad27bd6b3eadc17374b -A-1-1.x86_64@available
