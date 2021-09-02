repo @System 0 testtags <inline>

repo available -99.-1000 testtags <inline>
#>=Pkg: pkg-A 1 1 noarch
#>=Prv: pkg-A = 1-1
#>=Rec: pkg-B
#>=Pkg: pkg-B 1 1 noarch
#>=Prv: pkg-B = 1-1

system x86_64 rpm @System
poolflags implicitobsoleteusescolors
solverflags allowvendorchange keepexplicitobsoletes bestobeypolicy keeporphans yumobsoletes

job install oneof pkg-A-1-1.noarch@available [forcebest,targeted,setevr,setarch]
job excludefromweak name pkg-B
result transaction,problems <inline>
#>install pkg-A-1-1.noarch@available

nextjob
job install oneof pkg-A-1-1.noarch@available [forcebest,targeted,setevr,setarch]
job excludefromweak name pkg-A
job excludefromweak name pkg-B
result transaction,problems <inline>
#>install pkg-A-1-1.noarch@available
