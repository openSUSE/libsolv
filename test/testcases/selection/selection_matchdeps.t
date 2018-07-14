repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Pkg: AP 3 1 noarch
#>=Prv: A = 3.1
#>=Pkg: A 2 2 i686
#>=Req: BBB > 5
#>=Pkg: B 1 1 src
#>=Pkg: A 2 2 badarch
system i686 rpm

job noop selection_matchdeps solvable:name a = 2 rel,flat,nocase
result jobs <inline>
#>job noop oneof A-2-1.noarch@available A-2-2.i686@available

nextjob
job noop selection_matchdeps solvable:name a = 2-* flat,glob,nocase,depstr
result jobs <inline>
#>job noop oneof A-2-1.noarch@available A-2-2.i686@available

nextjob
job noop selection_matchdepid solvable:name A = 2-1 flat
result jobs <inline>
#>job noop pkg A-2-1.noarch@available [noautoset]

nextjob
job noop selection_matchdepid solvable:name A = 2-2 flat,depstr
result jobs <inline>
#>job noop pkg A-2-2.i686@available [noautoset]

nextjob
job noop selection_matchdepid solvable:name A = 2-2 flat,depstr,withbadarch
result jobs <inline>
#>job noop oneof A-2-2.i686@available A-2-2.badarch@available

nextjob
job noop selection_matchdeps solvable:requires bbb < 10 rel,flat,nocase
result jobs <inline>
#>job noop pkg A-2-2.i686@available [noautoset]

nextjob
job noop selection_matchdeps solvable:requires bbb >* depstr,glob,flat,nocase
result jobs <inline>
#>job noop pkg A-2-2.i686@available [noautoset]

nextjob
job noop selection_matchdepid solvable:requires BBB < 10 flat
result jobs <inline>
#>job noop pkg A-2-2.i686@available [noautoset]

nextjob
job noop selection_matchdepid solvable:requires BBB > 5 flat,depstr
result jobs <inline>
#>job noop pkg A-2-2.i686@available [noautoset]

