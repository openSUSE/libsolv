repo available 0 testtags <inline>
#>=Pkg: A1 1 1 noarch
#>=Req: B > 1 + B < 3
#>=Pkg: A2 1 1 noarch
#>=Req: B > 1 & B < 3
#>=Pkg: X 1 1 noarch
#>=Prv: B = 4
#>=Pkg: Y 1 1 noarch
#>=Prv: B = 2
#>=Pkg: A1 2 1 noarch
system i686 rpm

job noop selection_matchsolvable solvable:requires X-1-1.noarch@available flat
result jobs <inline>
#>job noop pkg A2-1-1.noarch@available [noautoset]

nextjob
job noop selection_matchsolvable solvable:requires Y-1-1.noarch@available flat
result jobs <inline>
#>job noop oneof A1-1-1.noarch@available A2-1-1.noarch@available

nextjob
job noop selection A1 name
job noop selection_matchsolvable solvable:requires Y-1-1.noarch@available flat,filter
result jobs <inline>
#>job noop pkg A1-1-1.noarch@available [noautoset]

