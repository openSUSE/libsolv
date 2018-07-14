repo system 0 testtags <inline>
#>=Pkg: k 1 0 x86_64
#>=Pkg: k-m 1 0 x86_64
#>=Req: k = 1-0
#>=Pkg: k-freak-1-0 1 0 x86_64
#>=Req: k = 1-0
#>=Pkg: k 1 1 x86_64
#>=Pkg: k-m 1 1 x86_64
#>=Req: k = 1-1
#>=Pkg: k 2 0 x86_64
#>=Pkg: k-m 2 0 x86_64
#>=Req: k = 2-0
#>=Pkg: k 3 0 x86_64
#>=Pkg: k-m 3 0 x86_64
#>=Req: k = 3-0
repo available 0 testtags <inline>
#>=Pkg: k 3 1 x86_64
#>=Pkg: k-m 3 1 x86_64
#>=Req: k = 3-1
#>=Pkg: k 3 6 x86_64
#>=Pkg: k-m 3 6 x86_64
#>=Req: k = 3-6
#>=Pkg: c 1 1 noarch
#>=Con: k = 3-6
system x86_64 rpm system
poolflags implicitobsoleteusescolors

job multiversion provides k
job multiversion provides k-m
job update all packages
result transaction,problems <inline>
#>install k-3-6.x86_64@available
#>install k-m-3-6.x86_64@available

nextjob

job multiversion provides k
job multiversion provides k-m
job install name c
job update all packages
result transaction,problems <inline>
#>install k-3-1.x86_64@available
#>install k-m-3-1.x86_64@available
#>install c-1-1.noarch@available


nextjob

job multiversion provides k
job multiversion provides k-m
job install name c
job update all packages [forcebest]
result transaction,problems <inline>
#>install k-3-6.x86_64@available
#>install k-m-3-6.x86_64@available
#>problem ca7106eb info package c-1-1.noarch conflicts with k = 3-6 provided by k-3-6.x86_64
#>problem ca7106eb solution 4d4bc71f allow k-1-0.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-1-1.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-2-0.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-3-0.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-m-1-0.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-m-1-1.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-m-2-0.x86_64@system
#>problem ca7106eb solution 4d4bc71f allow k-m-3-0.x86_64@system
#>problem ca7106eb solution 86764155 deljob install name c
