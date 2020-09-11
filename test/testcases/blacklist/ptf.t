repo system 0 testtags <inline>
#>=Pkg: ptf-2 1 1 noarch
#>=Prv: ptf-package()
repo available 0 testtags <inline>
#>=Pkg: ptf-1 1 1 noarch
#>=Prv: ptf-package()
#>=Pkg: ptf-2 2 1 noarch
#>=Prv: ptf-package()
#>=Pkg: A 1 1 noarch
#>=Req: ptf-1

system unset * system

#
# test 1: a ptf package cannot be pulled in via a dependency
#
job blacklist provides ptf-package()
job install name A
result transaction,problems <inline>
#>problem 78613afb info package A-1-1.noarch requires ptf-1, but none of the providers can be installed
#>problem 78613afb solution 23f73f5b deljob install name A
#>problem 78613afb solution b79aeb6f allow ptf-1-1-1.noarch@available

#
# test 2: a ptf package cannot be pulled in via a unspecific job
#
nextjob
job blacklist provides ptf-package()
job install name ptf-1
result transaction,problems <inline>
#>problem 021b17e2 info package ptf-1-1-1.noarch can only be installed by a direct request
#>problem 021b17e2 solution 932a6c2f deljob install name ptf-1
#>problem 021b17e2 solution b79aeb6f allow ptf-1-1-1.noarch@available

#
# test 3: a ptf package can be pulled in via a specific job
#
nextjob
job blacklist provides ptf-package()
job install name ptf-1 [setevr]
result transaction,problems <inline>
#>install ptf-1-1-1.noarch@available

#
# test 4: a ptf package can be updated
#
nextjob
job blacklist provides ptf-package()
job update all packages
result transaction,problems <inline>
#>upgrade ptf-2-1-1.noarch@system ptf-2-2-1.noarch@available

