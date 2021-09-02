repo @System 0 testtags <inline>
#>=Pkg: pkg-A 1.0 1 noarch
#>=Prv: pkg-A = 1.0-1
#>=Rec: pkg-C
#>=Pkg: pkg-B 1.0 1 noarch
#>=Prv: pkg-B = 1.0-1

repo available -99.-1000 testtags <inline>
#>=Pkg: pkg-A 1.0 3 noarch
#>=Prv: pkg-A = 1.0-3
#>=Rec: pkg-B
#>=Pkg: pkg-B 1.0 2 noarch
#>=Prv: pkg-B = 1.0-2
#>=Pkg: pkg-C 1.0 1 noarch
#>=Prv: pkg-C = 1.0-1
#>=Obs: pkg-B

system x86_64 rpm @System
poolflags implicitobsoleteusescolors
solverflags allowvendorchange keepexplicitobsoletes bestobeypolicy keeporphans yumobsoletes

job update all packages [forcebest]
job excludefromweak name pkg-C
result transaction,problems <inline>
#>erase pkg-B-1.0-1.noarch@@System pkg-C-1.0-1.noarch@available
#>install pkg-C-1.0-1.noarch@available
#>upgrade pkg-A-1.0-1.noarch@@System pkg-A-1.0-3.noarch@available

nextjob
job update oneof pkg-A-1.0-1.noarch@@System pkg-B-1.0-1.noarch@@System pkg-A-1.0-3.noarch@available pkg-B-1.0-2.noarch@available pkg-C-1.0-1.noarch@available [forcebest,targeted,setevr,setarch]
job excludefromweak name pkg-C
result transaction,problems <inline>
#>erase pkg-B-1.0-1.noarch@@System pkg-C-1.0-1.noarch@available
#>install pkg-C-1.0-1.noarch@available
#>upgrade pkg-A-1.0-1.noarch@@System pkg-A-1.0-3.noarch@available
