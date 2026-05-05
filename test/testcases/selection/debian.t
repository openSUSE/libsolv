repo available 0 testtags <inline>
#>=Pkg: A 1 1 all
#>=Pkg: B 1 1 amd64
#>=Pkg: C 1 1 arm64
system amd64 deb

job noop selection A name
job noop selection B name
job noop selection C name
result jobs <inline>
#>job noop name A
#>job noop name B

nextjob

job noop selection A_1-1 canon
job noop selection B_1-1 canon
job noop selection C_1-1 canon
result jobs <inline>
#>job noop name B = 1-1 [setevr]
#>job noop name A = 1-1 [setevr]

nextjob

job noop selection A_1-1_all canon
job noop selection B_1-1_amd64 canon
job noop selection C_1-1_arm64 canon
result jobs <inline>
#>job noop name (A . all) = 1-1 [setevr,setarch]
#>job noop name (B . amd64) = 1-1 [setevr,setarch]
