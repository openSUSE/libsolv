repo system 0 testtags <inline>
#>=Pkg: B 1 1 noarch
repo available 0 testtags <inline>
#>=Pkg: patch 1 1 noarch
#>=Con: B < 2-1
#>=Pkg: B 2 1 noarch
#>=Prv: retracted-patch-package()

system unset rpm system

job blacklist provides retracted-patch-package()
job install name patch
result transaction,problems <inline>
#>problem 3a66200a info package patch-1-1.noarch conflicts with B < 2-1 provided by B-1-1.noarch
#>problem 3a66200a solution 14805cf8 deljob install name patch
#>problem 3a66200a solution 4a9277b8 allow B-2-1.noarch@available
#>problem 3a66200a solution 718064ed erase B-1-1.noarch@system

nextjob
job blacklist provides retracted-patch-package()
job install pkg B-2-1.noarch@available
result transaction,problems <inline>
#>upgrade B-1-1.noarch@system B-2-1.noarch@available
