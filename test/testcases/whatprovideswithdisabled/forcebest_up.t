repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Vnd: foo
#>=Pkg: D 1 1 noarch
#>=Vnd: foo
#>=Con: A = 3-1
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Vnd: foo
#>=Pkg: A 3 1 noarch
#>=Vnd: bar
system i686 rpm system

poolflags whatprovideswithdisabled
disable pkg A-3-1.noarch@available

job install name A [forcebest]
result transaction,problems <inline>
#>upgrade A-1-1.noarch@system A-2-1.noarch@available

