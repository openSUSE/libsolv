repo available 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Pkg: A 2 1 noarch
#>=Prv: A = 3.1
#>=Pkg: A 2 2 i686
#>=Pkg: A 1:1 1 i686
#>=Pkg: A 2 2 badarch
#>=Pkg: A 1:3 1 i686
system i686 rpm

disable pkg E-1-1.src@available
disable pkg F-1-1.src@available

job noop selection A-2 canon
result jobs <inline>
#>job noop name A = 2 [setev]

nextjob
job noop selection A-2-1 canon
result jobs <inline>
#>job noop name A = 2-1 [setevr]

nextjob
job noop selection A-3 canon
result jobs <inline>
#>job noop name A = 1:3 [setev]

nextjob
job noop selection A-3-1 canon
result jobs <inline>
#>job noop name A = 1:3-1 [setevr]

nextjob
job noop selection A-1 canon
result jobs <inline>
#>job noop oneof A-1-1.noarch@available A-1:1-1.i686@available

nextjob
job noop selection A-1-1 canon
result jobs <inline>
#>job noop oneof A-1-1.noarch@available A-1:1-1.i686@available


nextjob
job noop selection A-0:1-1 canon
result jobs <inline>
#>job noop name A = 0:1-1 [setevr]

nextjob
job noop selection A-1:1-1 canon
result jobs <inline>
#>job noop name A = 1:1-1 [setevr]

