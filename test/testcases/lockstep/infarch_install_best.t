# test for issue 292
repo system 0 testtags <inline>
#>=Pkg: nss 3.39.0 2.fc29 x86_64
#>=Pkg: nss-sysinit 3.39.0 2.fc29 x86_64
#>=Req: nss = 3.39.0-2.fc29
repo available 0 testtags <inline>
#>=Pkg: nss 3.39.0 2.fc29 i686
#>=Pkg: nss 3.41.0 1.fc29 x86_64
#>=Pkg: nss-sysinit 3.41.0 1.fc29 x86_64
#>=Req: nss = 3.41.0-1.fc29

system x86_64 rpm system

poolflags implicitobsoleteusescolors
solverflags allowvendorchange keepexplicitobsoletes bestobeypolicy keeporphans yumobsoletes

job install oneof nss-3.41.0-1.fc29.x86_64@available [setevr,setarch]
result transaction,problems <inline>
#>upgrade nss-3.39.0-2.fc29.x86_64@system nss-3.41.0-1.fc29.x86_64@available
#>upgrade nss-sysinit-3.39.0-2.fc29.x86_64@system nss-sysinit-3.41.0-1.fc29.x86_64@available
