repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: P = 1
#>=Pkg: B 1 1 noarch
#>=Prv: P = 1
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Req: P = 2
#>=Pkg: A 2 2 noarch
#>=Req: P = 2
#>=Pkg: B 2 1 noarch
#>=Prv: P = 2
#>=Pkg: C 1 1 noarch
#>=Prv: P = 1
#>=Pkg: C 2 1 noarch
#>=Prv: P = 2
system i686 rpm system

job update name B
result transaction,problems <inline>
result transaction,problems <inline>
#>upgrade A-1-1.noarch@system A-2-2.noarch@available
#>upgrade B-1-1.noarch@system B-2-1.noarch@available
