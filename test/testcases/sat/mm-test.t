#
# testcase to check enabling/disabling of learnt rules
#
repo system 0 susetags mm-system.repo.gz
repo test 0 susetags mm-packages.repo.gz
system i686 rpm system
job install provides E
job verify all packages
result transaction,problems <inline>
#>erase D-1.0-1.noarch@system
#>erase D2-1.0-1.noarch@system
#>problem a3755a16 info package E-2.0-1.noarch requires foo, but none of the providers can be installed
#>problem a3755a16 solution 6d40bce1 deljob install provides E
#>problem a3755a16 solution c06ed43e erase D-1.0-1.noarch@system
#>problem a3755a16 solution c8a04f77 erase D2-1.0-1.noarch@system
#>upgrade A-1.0-1.noarch@system A-2.0-1.noarch@test
#>upgrade A2-1.0-1.noarch@system A2-2.0-1.noarch@test
