repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>+Prv:
#>A = 1-1 
#>-Prv:
#>=Pkg: D 1 1 noarch
#>+Prv:
#>D = 1-1 
#>-Prv:
#>=Pkg: Z 1 1 noarch
#>+Prv:
#>Z = 1-1 
#>-Prv:
#>+Con:
#>D = 2-1 
#>-Con:
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
#>=Pkg: C 1 0 noarch
#>+Prv:
#>C = 1-0 
#>-Prv:
#>+Obs:
#>A = 1-1 
#>-Obs:
#>=Pkg: D 2 1 noarch
#>+Prv:
#>D = 2-1 
#>-Prv:
#>=Pkg: D 3 1 noarch
#>+Prv:
#>D = 3-1 
#>-Prv:
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

# then targeted to D
nextjob
job distupgrade name D
result transaction,problems <inline>
#>upgrade D-1-1.noarch@system D-3-1.noarch@available

# then targeted to D-2-1 (should not go to D-3-1)
nextjob
job distupgrade name D = 2-1 
result transaction,problems <inline>
#>problem 840e2c39 info package Z-1-1.noarch conflicts with D = 2-1 provided by D-2-1.noarch
#>problem 840e2c39 solution 3158736f erase Z-1-1.noarch@system
#>problem 840e2c39 solution 42076df5 erase D-1-1.noarch@system
#>problem 840e2c39 solution cdacbabe allow D-3-1.noarch@available
#>upgrade D-1-1.noarch@system D-3-1.noarch@available

