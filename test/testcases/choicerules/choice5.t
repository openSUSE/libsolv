#
# test that a package split does not update unrelated packages
#
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA
#>=Pkg: B 1 1 noarch
#>=Req: libA
repo available 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA
#>=Pkg: A 2 1 noarch
#>=Pkg: Asplit 2 1 noarch
#>=Prv: libA
#>=Pkg: B 2 1 noarch
#>=Req: libA
system i686 rpm system
job update name A
result transaction,problems <inline>
#>install Asplit-2-1.noarch@available
#>upgrade A-1-1.noarch@system A-2-1.noarch@available
