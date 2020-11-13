repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA
#>=Pkg: B 1 1 noarch
#>=Req: libA
repo available 0 testtags <inline>
#>=Pkg: B 1 1 noarch
#>=Req: libA
#>=Pkg: A 2 1 noarch
#>=Pkg: Anew 2 1 noarch
#>=Prv: libA
system i686 rpm system

job update all packages
result transaction,problems <inline>
#>install Anew-2-1.noarch@available
#>upgrade A-1-1.noarch@system A-2-1.noarch@available
