repo system 0 testtags <inline>
#>=Pkg: a 1 1 x86_64
#>=Req: b
#>=Pkg: b 1 1 x86_64
repo available 0 testtags <inline>
#>=Pkg: a 2 1 x86_64
#>=Req: b
#>=Pkg: b 2 1 x86_64
#>=Pkg: c 2 1 x86_64
#>=Prv: b = 4
repo available2 0 testtags <inline>
#>=Pkg: b 3 1 x86_64
system x86_64 rpm system

job update all packages [cleandeps]
result transaction,problems <inline>
#>upgrade a-1-1.x86_64@system a-2-1.x86_64@available
#>upgrade b-1-1.x86_64@system b-3-1.x86_64@available2
nextjob
job update repo available [cleandeps]
result transaction,problems <inline>
#>upgrade a-1-1.x86_64@system a-2-1.x86_64@available
#>upgrade b-1-1.x86_64@system b-2-1.x86_64@available
