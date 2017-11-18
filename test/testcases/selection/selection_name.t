repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Pkg: AP 3 1 noarch
#>=Prv: A = 3.1
#>=Pkg: A 2 2 i686
#>=Req: BBB > 5
#>=Pkg: B 1 1 src
#>=Pkg: A 2 2 badarch
#>=Pkg: C 2 2 badarch
#>=Pkg: D 2 2 noarch
#>=Pkg: D 2 2 badarch
#>=Pkg: E 1 1 src
#>=Pkg: F 1 1 src
#>=Pkg: F 1 2 src
#>=Pkg: G 1 1 src
#>=Pkg: G 1 2 src
system i686 rpm

disable pkg E-1-1.src@available
disable pkg F-1-1.src@available

job noop selection A name
result jobs <inline>
#>job noop name A

nextjob
job noop selection A.i686 name,dotarch
result jobs <inline>
#>job noop name A . i686 [setarch]

nextjob
job noop selection A.i686>1 name,dotarch,rel
result jobs <inline>
#>job noop name (A . i686) > 1 [setarch]

nextjob
job noop selection B* glob,name,withsource
result jobs <inline>
#>job noop pkg B-1-1.src@available [noautoset]

nextjob
job noop selection A=2-2 name,dotarch,rel,withbadarch
result jobs <inline>
#>job noop oneof A-2-2.i686@available A-2-2.badarch@available [setevr]

nextjob
job noop selection C name,withbadarch
result jobs <inline>
#>job noop pkg C-2-2.badarch@available [noautoset]

nextjob
job noop selection D name,withbadarch
result jobs <inline>
#>job noop oneof D-2-2.noarch@available D-2-2.badarch@available

nextjob
job noop selection E name,sourceonly,withdisabled
result jobs <inline>
#>job noop pkg E-1-1.src@available [noautoset]

nextjob
job noop selection E name,withsource,withdisabled
result jobs <inline>
#>job noop pkg E-1-1.src@available [noautoset]

nextjob
job noop selection F name,sourceonly,withdisabled
result jobs <inline>
#>job noop oneof F-1-1.src@available F-1-2.src@available

nextjob
job noop selection F name,withsource,withdisabled
result jobs <inline>
#>job noop oneof F-1-1.src@available F-1-2.src@available

nextjob
job noop selection G name,sourceonly,withdisabled
result jobs <inline>
#>job noop name G . src

nextjob
job noop selection G name,withsource,withdisabled
result jobs <inline>
#>job noop oneof G-1-1.src@available G-1-2.src@available

