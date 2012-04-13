repo system 0 testtags mistake-system.repo
repo test 0 testtags mistake-packages.repo
system i686 rpm system
job install name A = 2 [cleandeps]
result transaction,problems <inline>
#>problem b5abcb9c info package A-2-1.noarch requires B = 1, but none of the providers can be installed
#>problem b5abcb9c solution 37b448af deljob install name A = 2 [cleandeps]
#>problem b5abcb9c solution 3c170283 replace B-2-1.noarch@system B-1-1.noarch@test
