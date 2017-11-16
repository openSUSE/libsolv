repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Pkg: AP 3 1 noarch
#>=Prv: A = 3.1
#>=Pkg: A 2 2 i686
#>=Req: BBB > 5
#>=Pkg: B 1 1 src
#>=Pkg: A 2 2 badarch
system i686 rpm

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

