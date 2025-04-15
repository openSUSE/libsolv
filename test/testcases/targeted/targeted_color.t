# testcase for issue 583
# targeted update with forcebest and implicitobsoleteusescolor
repo @System 0 testtags <inline>
#>=Ver: 3.0
#>=Pkg: a 1 1 x86_64
#>=Prv: a = 1-1
#>=Con: c < 1-1
#>
#>=Pkg: c 1 1 x86_64
#>=Req: c-dep = 1-1
#>=Prv: c = 1-1
#>
#>=Pkg: c-dep 1 1 x86_64
#>=Prv: c-dep = 1-1
#>
#>=Pkg: d 1 1 x86_64
#>=Req: c = 1-1
#>=Req: multiarch = 1-1
#>=Prv: d = 1-1
#>
#>=Pkg: multiarch 1 1 x86_64
#>=Prv: multiarch = 1-1

repo available 0 testtags <inline>
#>=Ver: 3.0
#>=Pkg: a 2 2 x86_64
#>=Prv: a = 2-2
#>=Con: c < 2-2
#>
#>=Pkg: c 2 2 x86_64
#>=Req: c-dep = 2-2
#>=Prv: c = 2-2
#>
#>=Pkg: c-dep 2 2 x86_64
#>=Prv: c-dep = 2-2
#>
#>=Pkg: d 2 2 x86_64
#>=Req: multiarch >= 2-2
#>=Prv: d = 2-2
#>
#>=Pkg: multiarch 2 2 x86_64
#>=Prv: multiarch = 2-2
#>
#>=Pkg: multiarch 2 2 i686
#>=Prv: multiarch = 2-2

system x86_64 rpm @System
poolflags implicitobsoleteusescolors
solverflags bestobeypolicy
job update oneof a-2-2.x86_64@available multiarch-1-1.x86_64@@System multiarch-2-2.i686@available [forcebest,targeted,setevr,setarch]
result transaction,problems <inline>
#>upgrade a-1-1.x86_64@@System a-2-2.x86_64@available
#>upgrade multiarch-1-1.x86_64@@System multiarch-2-2.x86_64@available
#>upgrade c-dep-1-1.x86_64@@System c-dep-2-2.x86_64@available
#>upgrade c-1-1.x86_64@@System c-2-2.x86_64@available
#>upgrade d-1-1.x86_64@@System d-2-2.x86_64@available
