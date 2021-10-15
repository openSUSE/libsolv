repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
repo available 1 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: B
system i686 rpm system

solverflags strictrepopriority
job install name A
result transaction,problems <inline>
#>problem 30c1639e info nothing provides B needed by A-1-1.noarch
#>problem 30c1639e solution 23f73f5b deljob install name A
#>problem 30c1639e solution 5dd1416b allow A-2-1.noarch@available

nextjob

solverflags strictrepopriority
job install pkg A-2-1.noarch@available
result transaction,problems <inline>
#>install A-2-1.noarch@available

