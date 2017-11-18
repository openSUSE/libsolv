repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Pkg: AP 3 1 noarch
#>=Prv: A = 3.1
#>=Pkg: A 2 2 i686
#>=Req: BBB > 5
#>=Pkg: B 1 1 src
#>=Pkg: A 2 2 badarch
#>=Pkg: C 2 2 badarch
#>=Pkg: D 2 2 badarch
#>=Pkg: D 2 2 noarch
system i686 rpm

job noop selection A provides
result jobs <inline>
#>job noop provides A

nextjob
job noop selection A.i686 provides,dotarch
result jobs <inline>
#>job noop provides A . i686 [setarch]

nextjob
job noop selection A.i686>1 provides,dotarch,rel
result jobs <inline>
#>job noop provides (A . i686) > 1 [setarch]

nextjob
job noop selection A* glob,provides,withbadarch
result jobs <inline>
#>job noop oneof A-2-1.noarch@available AP-3-1.noarch@available A-2-2.i686@available A-2-2.badarch@available
#>job noop provides AP

nextjob
job noop selection A*>=2 glob,provides,dotarch,rel,withbadarch
result jobs <inline>
#>job noop oneof A-2-1.noarch@available AP-3-1.noarch@available A-2-2.i686@available A-2-2.badarch@available
#>job noop provides AP >= 2

nextjob
job noop selection C provides,withbadarch
result jobs <inline>
#>job noop pkg C-2-2.badarch@available [noautoset]


nextjob
job noop selection D provides,withbadarch
result jobs <inline>
#>job noop oneof D-2-2.badarch@available D-2-2.noarch@available
