repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: gcc 5 1 noarch
#>=Prv: gcc
#>=Pkg: gcc 6 1 noarch
#>=Prv: gcc
#>=Pkg: build-tools 1 1 noarch
#>=Req: gcc
system unset * system

job install name build-tools
job favor pkg gcc-5-1.noarch@available
result transaction,problems,alternatives <inline>
#>alternative 6e50ec1a  0 build-tools-1-1.noarch@available requires gcc
#>alternative 6e50ec1a  1 + gcc-5-1.noarch@available
#>alternative 6e50ec1a  2   gcc-6-1.noarch@available
#>install build-tools-1-1.noarch@available
#>install gcc-5-1.noarch@available

nextjob

job install name build-tools
job disfavor pkg gcc-6-1.noarch@available
result transaction,problems,alternatives <inline>
#>alternative 6e50ec1a  0 build-tools-1-1.noarch@available requires gcc
#>alternative 6e50ec1a  1 + gcc-5-1.noarch@available
#>alternative 6e50ec1a  2   gcc-6-1.noarch@available
#>install build-tools-1-1.noarch@available
#>install gcc-5-1.noarch@available

