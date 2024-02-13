# testcase for issue #550
repo @System 0 testtags <inline>
#>=Pkg: kernel-core 1.0.2 2 x86_64
#>=Prv: kernel-core = 1.0.2-2
#>=Prv: kernel-core-multiversion = 1.0.2-2
#>=Pkg: kernel 1.0.2 2 x86_64
#>=Req: kernel-core = 1.0.2-2
#>=Prv: kernel = 1.0.2-2
#>=Prv: kernel-core-multiversion = 1.0.2-2
#>=Pkg: kernel-core 1.0.4 2 x86_64
#>=Prv: kernel-core = 1.0.4-2
#>=Prv: kernel-core-multiversion = 1.0.4-2
#>=Pkg: kernel 1.0.4 2 x86_64
#>=Req: kernel-core = 1.0.4-2
#>=Prv: kernel = 1.0.4-2
#>=Prv: kernel-core-multiversion = 1.0.4-2
repo updates 0 testtags <inline>
#>=Pkg: kernel-core 1.0.2 2 x86_64
#>=Prv: kernel-core = 1.0.2-2
#>=Prv: kernel-core-multiversion = 1.0.2-2
#>=Pkg: kernel 1.0.2 2 x86_64
#>=Req: kernel-core = 1.0.2-2
#>=Prv: kernel = 1.0.2-2
#>=Prv: kernel-core-multiversion = 1.0.2-2
#>=Pkg: kernel-core 1.0.4 2 x86_64
#>=Prv: kernel-core = 1.0.4-2
#>=Prv: kernel-core-multiversion = 1.0.4-2
#>=Pkg: kernel 1.0.4 2 x86_64
#>=Req: kernel-core = 1.0.4-2
#>=Prv: kernel = 1.0.4-2
#>=Prv: kernel-core-multiversion = 1.0.4-2
#>=Pkg: kernel-core 1.0.5 2 x86_64
#>=Prv: kernel-core = 1.0.5-2
#>=Prv: kernel-core-multiversion = 1.0.5-2
#>=Pkg: kernel 1.0.5 2 x86_64
#>=Req: kernel-core = 1.0.5-2
#>=Prv: kernel = 1.0.5-2
#>=Prv: kernel-core-multiversion = 1.0.5-2
system x86_64 rpm @System
poolflags implicitobsoleteusescolors
solverflags allowvendorchange keepexplicitobsoletes bestobeypolicy keeporphans yumobsoletes
job multiversion provides kernel-core-multiversion
job update oneof kernel-core-1.0.4-2.x86_64@@System kernel-core-1.0.2-2.x86_64@@System kernel-core-1.0.5-2.x86_64@updates [forcebest,targeted,setevr,setarch]
job install pkg kernel-core-1.0.5-2.x86_64@updates
job install pkg kernel-core-1.0.4-2.x86_64@@System
job erase pkg kernel-core-1.0.2-2.x86_64@@System
job allowuninstall pkg kernel-core-1.0.2-2.x86_64@@System
job allowuninstall pkg kernel-core-1.0.4-2.x86_64@@System
job allowuninstall pkg kernel-1.0.2-2.x86_64@@System
job allowuninstall pkg kernel-1.0.4-2.x86_64@@System
result transaction,problems <inline>
#>install kernel-core-1.0.5-2.x86_64@updates
#>erase kernel-core-1.0.2-2.x86_64@@System
#>erase kernel-1.0.2-2.x86_64@@System
