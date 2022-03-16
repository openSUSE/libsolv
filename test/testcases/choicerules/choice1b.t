feature complex_deps
repo system 0 testtags <inline>
#>=Pkg: B 1 1 noarch
#>=Prv: P = 1
repo available 0 testtags <inline>
#>=Pkg: X 1 1 noarch
#>=Pkg: Y 1 1 noarch
#>=Pkg: A 2 1 noarch
#>=Req: P = 2 <IF> (X & Y)
#>=Pkg: B 2 1 noarch
#>=Prv: P = 2
#>=Pkg: C 2 1 noarch
#>=Prv: P = 2
system i686 rpm system

job install name A
result transaction,problems <inline>
result transaction,problems <inline>
#>install A-2-1.noarch@available
