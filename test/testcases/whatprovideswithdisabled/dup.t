repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Vnd: foo
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Vnd: foo
system i686 rpm system
poolflags whatprovideswithdisabled

disable pkg A-2-1.noarch@available

job distupgrade all packages
result transaction,problems <inline>

