# make sure that orphaned packages do not get
# deinstalled by choice rules bnc#1190465
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: P = 1
#>=Pkg: P 1 1 noarch
repo available 0 testtags <inline>
#>=Pkg: P 2 1 noarch
#>=Pkg: Q 2 1 noarch
#>=Prv: P = 1
system i686 rpm system

job distupgrade all packages
result transaction,problems <inline>
#>install Q-2-1.noarch@available
#>upgrade P-1-1.noarch@system P-2-1.noarch@available
