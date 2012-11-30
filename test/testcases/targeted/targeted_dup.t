repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>+Prv:
#>A = 1-1
#>-Prv:
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>+Prv:
#>A = 2-1
#>-Prv:
#>=Pkg: B 1 0 noarch
#>+Prv:
#>B = 1-0
#>-Prv:
#>+Obs:
#>A
#>-Obs:
system i686 rpm system

# first check untargeted
job distupgrade name A = 1-1
result transaction,problems <inline>
#>erase A-1-1.noarch@system B-1-0.noarch@available
#>install B-1-0.noarch@available

# then targeted to A-2-1
nextjob
job distupgrade name A = 2-1
result transaction,problems <inline>
#>upgrade A-1-1.noarch@system A-2-1.noarch@available

# then targeted to B
nextjob
job distupgrade name B
result transaction,problems <inline>
#>erase A-1-1.noarch@system B-1-0.noarch@available
#>install B-1-0.noarch@available

# first check forced to targeted
nextjob
job distupgrade name A = 1-1 [targeted]
result transaction,problems <inline>

# second check forced to untargeted
nextjob
solverflags noautotarget
job distupgrade name A = 2-1
result transaction,problems <inline>
